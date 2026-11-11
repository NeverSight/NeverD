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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Throws,
                           unsigned Scalar, bool Alias, bool Swap,
                           bool Instance = false) {
  const std::string T = Alpha ? "Element" : "T",
                    Result = Alias ? "Lfixture/Result;" : "Ljava/io/File;",
                    Carrier = "Lfixture/Carrier;",
                    String = "Ljava/lang/String;";
  auto Provider = klass("Lfixture/Provider;", SDK);
  std::vector<std::string> Params{Carrier, Swap ? String : Result,
                                  Swap ? Result : String};
  const std::string Context = Scalar == 2 ? "J" : "Z";
  if (Scalar)
    Params.push_back(Context);
  const unsigned Words = 3 + (Scalar == 2 ? 2 : Scalar ? 1 : 0);
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Target =
      method({Provider.name, "select", Params, Result}, {"public", "static"},
             SDK ? 0 : Words + (Instance ? 2 : 1),
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{Zero, op(1, "return-object", {0})});
  Target.generic_signature =
      "<" + T + ":Ljava/lang/Object;>(Lfixture/Carrier<T" + T + ";>;" +
      (Swap ? String + Result : Result + String) + (Scalar ? Context : "") +
      ")" + Result;
  if (Throws)
    Target.declared_throws = std::vector<std::string>{"Lfixture/Failure;"};
  if (Instance)
    Target.access.erase("static");
  Provider.methods.push_back(Target);
  auto Client = klass("Lfixture/Client;");
  if (Instance)
    Params.insert(Params.begin(), Provider.name);
  const unsigned CallerWords = Words + (Instance ? 1 : 0);
  const std::string Opcode = Instance ? "invoke-virtual" : "invoke-static";
  std::vector<unsigned> Args;
  for (unsigned I = 1; I <= CallerWords; ++I)
    Args.push_back(I);
  Client.methods.push_back(
      method({Client.name, "call", Params, Result}, {"public", "static"},
             CallerWords + 1,
             {op(0, Range ? Opcode + "/range" : Opcode, Args, Target.reference),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}));
  auto Input = klass(Carrier, true);
  Input.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Output = klass(Result, true);
  auto Text = klass(String, true);
  Text.access.insert("final");
  auto Root = klass("Ljava/lang/Object;", true);
  Root.superclass.reset();
  auto Throwable = klass("Ljava/lang/Throwable;", true);
  auto Exception = klass("Ljava/lang/Exception;", true);
  Exception.superclass = Throwable.name;
  auto Runtime = klass("Ljava/lang/RuntimeException;", true);
  Runtime.superclass = Exception.name;
  auto Failure = klass("Lfixture/Failure;", true);
  Failure.superclass = Runtime.name;
  auto Number = klass("Ljava/lang/Number;", true);
  return {Client,    Provider,  Input,   Output,  Text,  Root,
          Throwable, Exception, Runtime, Failure, Number};
}
} // namespace

TEST(
    MobileDalvikCarrierNominalContext,
    RetainInvariantAnchorsIndependentNominalsExactContextsAndClosedUncheckedThrows) {
  for (bool Instance : {false, true})
    for (bool SDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Throws : {false, true})
            for (unsigned Scalar : {0u, 1u, 2u})
              for (bool Alias : {false, true})
                for (bool Swap : {false, true})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    if (Instance && Scalar == 2 && !Range)
                      continue; // Six physical words require a range invoke.
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, Range, Alpha, Throws, Scalar, Alias,
                                   Swap, Route, Instance}));
                    auto Input = fixture(SDK, Range, Alpha, Throws, Scalar,
                                         Alias, Swap, Instance);
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

TEST(MobileDalvikCarrierNominalContext,
     RejectBrokenScopesChangedDescriptorsUncheckedClaimsRolesAndPhysicalWords) {
  for (bool Instance : {false, true})
    for (bool SDK : {false, true})
      for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Instance}));
        auto Input = fixture(SDK, true, false, true, 1, true, false, Instance);
        auto &Target = Input[1].methods[0];
        auto &Signature = *Target.generic_signature;
        auto Replace = [&](const std::string &From, const std::string &To) {
          const auto At = Signature.find(From);
          ASSERT_NE(At, std::string::npos);
          Signature.replace(At, From.size(), To);
        };
        if (Mutation == 0)
          Replace("Carrier<TT;>", "Carrier<TMissing;>");
        else if (Mutation == 1)
          Replace("Object;>(", "Object;U:Ljava/lang/Object;>(");
        else if (Mutation == 2)
          Replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
        else if (Mutation == 3)
          Replace(">;Lfixture/Result;", ">;Ljava/lang/String;");
        else if (Mutation == 4)
          Replace(")Lfixture/Result;", ")Ljava/lang/Object;");
        else if (Mutation == 5)
          Replace(";Z)", ";I)");
        else if (Mutation == 6)
          Input[2].generic_signature =
              "<E:Ljava/lang/Object;F:Ljava/lang/Object;>Ljava/lang/Object;";
        else if (Mutation == 7)
          Target.access.insert("varargs");
        else if (Mutation == 8) {
          Target.access.erase("public");
          Target.access.insert("private");
        } else if (Mutation == 9) {
          if (Instance)
            Target.access.insert("static");
          else
            Target.access.erase("static");
        } else if (Mutation == 10)
          Target.access.insert("native");
        else if (Mutation == 11)
          Target.access.insert("abstract");
        else if (Mutation == 12)
          Target.access.insert("synthetic");
        else if (Mutation == 13)
          Target.access.insert("bridge");
        else if (Mutation == 14)
          Target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
        else if (Mutation == 15)
          Target.declared_throws =
              std::vector<std::string>{"Lfixture/Missing;"};
        else if (Mutation == 16)
          Input[0].methods[0].instructions[0].registers.pop_back();
        else
          Input[2].generic_signature =
              "<E:Ljava/lang/Number;>Ljava/lang/Object;";
        Budget B;
        EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
      }
}

namespace {
std::vector<Class> configuredCollectorFixture(unsigned Dependencies,
                                              unsigned Contexts, bool Range,
                                              bool Alpha, bool Swap,
                                              bool Variant, bool Used) {
  const std::string T = Alpha ? "Element" : "T", Context = "Lfixture/Context;",
                    Mapped = "Lfixture/Mapped;", Result = "Lfixture/Result;",
                    Mapper = "Lfixture/Mapper;", Carrier = "Lfixture/Carrier;";
  auto provider = klass("Lfixture/Provider;", Dependencies & 1);
  std::vector<std::string> parameters(Contexts, Context);
  parameters.insert(Swap ? parameters.begin() : parameters.end(), Mapper);
  std::string callback = "Lfixture/Mapper<" + std::string(Variant ? "-" : "") +
                         "T" + T + ";" + (Variant ? "" : "+") + Mapped + ">;";
  std::string signature = "<" + T + ":Ljava/lang/Object;>(";
  if (Swap)
    signature += callback;
  for (unsigned i = 0; i < Contexts; ++i)
    signature += Context;
  if (!Swap)
    signature += callback;
  signature += ")Lfixture/Carrier<T" + T + ";*" + Result + ">;";
  const MethodRef target{provider.name, "make", parameters, Carrier};
  const FieldRef returned{"Lfixture/Trace;", "result", Carrier};
  provider.methods.push_back(
      method(target, {"public", "static"}, provider.external ? 0 : Contexts + 2,
             provider.external
                 ? std::vector<Instruction>{}
                 : std::vector<Instruction>{op(0, "sget-object", {0}, returned),
                                            op(2, "return-object", {0})}));
  provider.methods[0].generic_signature = signature;
  auto caller = klass("Lfixture/Caller;");
  const unsigned first = Range ? 16 : 1;
  std::vector<unsigned> words;
  for (unsigned i = 0; i <= Contexts; ++i)
    words.push_back(first + i);
  std::vector<Instruction> code{
      op(0, Range ? "invoke-static/range" : "invoke-static", words, target)};
  if (Used) {
    code.push_back(op(3, "move-result-object", {0}));
    code.push_back(op(4, "return-object", {0}));
  } else
    code.push_back(op(3, "return-void"));
  caller.methods.push_back(
      method({caller.name, "call", parameters, Used ? Carrier : "V"},
             {"public", "static"}, first + Contexts + 1, std::move(code)));
  auto trace = klass(returned.owner);
  Field field;
  field.reference = returned;
  field.access = {"public", "static"};
  trace.fields.push_back(field);
  auto mapper = klass(Mapper, Dependencies & 2);
  mapper.access = {"public", "abstract", "interface"};
  mapper.generic_signature =
      "<In:Ljava/lang/Object;Out:Ljava/lang/Object;>Ljava/lang/Object;";
  auto carrier = klass(Carrier, Dependencies & 2);
  carrier.access = mapper.access;
  carrier.generic_signature =
      "<In:Ljava/lang/Object;State:Ljava/lang/Object;Out:Ljava/lang/"
      "Object;>Ljava/lang/Object;";
  auto context = klass(Context, Dependencies & 2),
       mapped = klass(Mapped, Dependencies & 2);
  context.access = mapped.access = mapper.access;
  auto result = klass(Result, Dependencies & 2);
  result.access.insert("final");
  auto root = klass("Ljava/lang/Object;", true);
  root.superclass.reset();
  auto number = klass("Ljava/lang/Number;", true),
       exception = klass("Ljava/lang/Exception;", true);
  return {caller, provider, trace, mapper, carrier,  context,
          mapped, result,   root,  number, exception};
}
ClassMap configuredInput(std::vector<Class> classes, unsigned Route,
                         Budget &B) {
  if (Route == 2)
    std::reverse(classes.begin(), classes.end());
  if (Route)
    return linkClasses(classes, B);
  ClassMap result;
  for (auto &cls : classes)
    result.emplace(cls.name, std::move(cls));
  return result;
}
} // namespace

TEST(MobileDalvikCarrierNominalContext,
     ConfiguredCollectorsKeepClosedContextsAndResultRoles) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (unsigned Contexts : {1u, 3u})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Swap : {false, true})
            for (bool Variant : {false, true})
              for (bool Used : {false, true})
                for (unsigned Route = 0; Route < 3; ++Route) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{Dependencies, Contexts, Range, Alpha, Swap,
                                 Variant, Used, Route}));
                  auto classes =
                      configuredCollectorFixture(Dependencies, Contexts, Range,
                                                 Alpha, Swap, Variant, Used);
                  const auto original = classes[1].methods[0];
                  Budget B;
                  const auto linked = configuredInput(classes, Route, B);
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

TEST(MobileDalvikCarrierNominalContext,
     ConfiguredCollectorsRejectOpenRolesNominalsAndFrames) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (unsigned Contexts : {1u, 3u})
      for (unsigned Mutation = 0; Mutation < 32; ++Mutation)
        for (unsigned Route = 1; Route < 3; ++Route) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{Dependencies, Contexts, Mutation, Route}));
          auto classes = configuredCollectorFixture(
              Dependencies, Contexts, true, false, false, false, true);
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
            replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
            break;
          case 1:
            replace("Mapper<TT;+", "Mapper<TMissing;+");
            break;
          case 2:
            replace("Mapper<TT;+", "Mapper<Ljava/lang/Number;+");
            break;
          case 3:
            replace("Mapper<TT;+", "Mapper<+TT;+");
            break;
          case 4:
            replace("Mapper<TT;+", "Mapper<TT;-");
            break;
          case 5:
            replace("Mapper<TT;+Lfixture/Mapped;>", "Mapper<TT;TT;>");
            break;
          case 6:
            replace("Carrier<TT;*", "Carrier<-TT;*");
            break;
          case 7:
            replace("Carrier<TT;*", "Carrier<TT;TT;");
            break;
          case 8:
            replace("Carrier<TT;*Lfixture/Result;>",
                    "Carrier<TT;*+Lfixture/Result;>");
            break;
          case 9:
            replace("Carrier<TT;*Lfixture/Result;>", "Carrier<TT;*TT;>");
            break;
          case 10:
            replace("Mapper<TT;+Lfixture/Mapped;>",
                    "Mapper<TT;+Lfixture/Mapped;TT;>");
            break;
          case 11:
            classes[4].generic_signature =
                "<X:Ljava/lang/Object;Y:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 12:
            classes.erase(classes.begin() + 5);
            break;
          case 13:
            classes.erase(classes.begin() + 6);
            break;
          case 14:
            classes.erase(classes.begin() + 7);
            break;
          case 15:
            classes[6].name = "Lhidden/Mapped;";
            classes[6].access.erase("public");
            replace("Lfixture/Mapped;", "Lhidden/Mapped;");
            break;
          case 16:
            classes[5].generic_signature =
                "<T:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 17:
            classes[7].generic_signature =
                "<T:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 18:
            classes[3].generic_signature = "<In:Ljava/lang/Object;Out:Ljava/"
                                           "lang/Number;>Ljava/lang/Object;";
            break;
          case 19:
            target.access = {"private", "static"};
            break;
          case 20:
            target.access.erase("static");
            break;
          case 21:
            target.access.insert("native");
            break;
          case 22:
            target.access.insert("synthetic");
            break;
          case 23:
            target.access.insert("varargs");
            break;
          case 24:
            target.declared_throws =
                std::vector<std::string>{"Ljava/lang/Exception;"};
            break;
          case 25:
            provider.generic_signature =
                "<T:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 26:
            caller.instructions[0].registers.pop_back();
            break;
          case 27:
            caller.instructions[0].registers[0] = caller.registers;
            break;
          case 28:
            caller.instructions[1].opcode = "move-result-wide";
            break;
          case 29:
            replace(">(Lfixture/Context;",
                    "U:Ljava/lang/Object;>(Lfixture/Context;");
            break;
          case 30:
            replace("Carrier<TT;*", "Carrier<TMissing;*");
            break;
          case 31:
            replace(">(Lfixture/Context;",
                    ">(Lfixture/Mapper<TT;+Lfixture/Mapped;>;");
            target.reference.parameters.front() = "Lfixture/Mapper;";
            caller.reference.parameters = target.reference.parameters;
            caller.instructions[0].reference = target.reference;
            break;
          }
          EXPECT_THROW(([&] {
                         Budget B;
                         recoverJava(configuredInput(classes, Route, B), B);
                       }()),
                       std::runtime_error);
        }
}

namespace {
std::vector<Class> contextualCollectorFixture(unsigned Dependencies,
                                              unsigned Contexts, bool Range,
                                              bool Alpha, bool Used) {
  auto classes = configuredCollectorFixture(Dependencies, Contexts - 1, Range,
                                            Alpha, false, false, Used);
  auto &target = classes[1].methods[0];
  auto &caller = classes[0].methods[0];
  const std::string T = Alpha ? "Element" : "T";
  const std::string callback = "Lfixture/Mapper<T" + T + ";+Lfixture/Mapped;>;";
  auto &signature = *target.generic_signature;
  signature.replace(signature.find(callback), callback.size(),
                    "Lfixture/Context;");
  target.reference.parameters.back() = "Lfixture/Context;";
  caller.reference.parameters = target.reference.parameters;
  caller.instructions[0].reference = target.reference;
  return classes;
}
} // namespace

TEST(MobileDalvikCarrierNominalContext,
     ContextualCollectorsKeepResultOnlyFormalsAndClosedInputs) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (unsigned Contexts : {1u, 2u, 3u, 4u})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Used : {false, true})
            for (unsigned Route = 0; Route < 3; ++Route) {
              SCOPED_TRACE(::testing::PrintToString(std::tuple{
                  Dependencies, Contexts, Range, Alpha, Used, Route}));
              auto classes = contextualCollectorFixture(Dependencies, Contexts,
                                                        Range, Alpha, Used);
              const auto original = classes[1].methods[0];
              Budget B;
              const auto linked = configuredInput(classes, Route, B);
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

TEST(MobileDalvikCarrierNominalContext,
     ContextualCollectorsRejectUnprovedDeclarationsRolesAndFrames) {
  for (unsigned Dependencies = 0; Dependencies < 4; ++Dependencies)
    for (unsigned Contexts : {1u, 3u})
      for (unsigned Mutation = 0; Mutation < 27; ++Mutation)
        for (unsigned Route = 1; Route < 3; ++Route) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{Dependencies, Contexts, Mutation, Route}));
          auto classes = contextualCollectorFixture(Dependencies, Contexts,
                                                    true, false, true);
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
            replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
            break;
          case 1:
            replace("Carrier<TT;*", "Carrier<TMissing;*");
            break;
          case 2:
            replace("T:Ljava/lang/Object;>",
                    "T:Ljava/lang/Object;U:Ljava/lang/Object;>");
            break;
          case 3:
            replace("Carrier<TT;*", "Carrier<+TT;*");
            break;
          case 4:
            replace("Carrier<TT;*", "Carrier<TT;TT;");
            break;
          case 5:
            replace("*Lfixture/Result;", "*+Lfixture/Result;");
            break;
          case 6:
            replace("*Lfixture/Result;", "*TT;");
            break;
          case 7:
            replace("(Lfixture/Context;", "(Lfixture/Context<TT;>;");
            break;
          case 8:
            classes.erase(classes.begin() + 5);
            break;
          case 9:
            classes.erase(classes.begin() + 7);
            break;
          case 10:
            classes[5].name = "Lhidden/Context;";
            classes[5].access.erase("public");
            for (auto &p : target.reference.parameters)
              p = "Lhidden/Context;";
            caller.reference.parameters = target.reference.parameters;
            caller.instructions[0].reference = target.reference;
            for (unsigned i = 0; i < Contexts; ++i)
              replace("Lfixture/Context;", "Lhidden/Context;");
            break;
          case 11:
            classes[4].generic_signature =
                "<X:Ljava/lang/Object;Y:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 12:
            classes[4].generic_signature =
                "<In:Ljava/lang/Number;State:Ljava/lang/Object;Out:Ljava/lang/"
                "Object;>Ljava/lang/Object;";
            break;
          case 13:
            classes[5].generic_signature =
                "<T:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 14:
            classes[7].generic_signature =
                "<T:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 15:
            target.access = {"private", "static"};
            break;
          case 16:
            target.access.erase("static");
            break;
          case 17:
            target.access.insert("native");
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
            provider.generic_signature =
                "<T:Ljava/lang/Object;>Ljava/lang/Object;";
            break;
          case 22:
            caller.instructions[0].registers.pop_back();
            break;
          case 23:
            caller.instructions[0].registers[0] = caller.registers;
            break;
          case 24:
            caller.instructions[1].opcode = "move-result-wide";
            break;
          case 25:
            replace("(Lfixture/Context;", "(Ljava/lang/Object;");
            break;
          case 26:
            replace("*Lfixture/Result;", "*Lfixture/Missing;");
            break;
          }
          EXPECT_THROW(([&] {
                         Budget B;
                         recoverJava(configuredInput(classes, Route, B), B);
                       }()),
                       std::runtime_error);
        }
}
