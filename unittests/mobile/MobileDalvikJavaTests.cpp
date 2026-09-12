#include "MobileDalvik.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <functional>
#include <set>
#include <stdexcept>
#include <tuple>

using namespace neverd::mobile;
using namespace neverd::mobile::dalvik;
namespace {
Instruction op(uint32_t PC, std::string Name,
               std::vector<unsigned> Registers = {}, Literal Value = {},
               Reference Ref = {}, std::optional<uint32_t> Target = {}) {
  Instruction I;
  I.pc = PC;
  I.opcode = std::move(Name);
  I.registers = std::move(Registers);
  I.literal = std::move(Value);
  I.reference = std::move(Ref);
  I.target = Target;
  return I;
}
Method method(std::string Name, std::vector<std::string> Parameters,
              std::string Returns, unsigned Registers,
              std::vector<Instruction> Code,
              std::vector<TryRegion> Tries = {}) {
  Method M;
  M.reference = {"Lfixture/Core;", std::move(Name), std::move(Parameters),
                 std::move(Returns)};
  M.access = {"public", "static"};
  M.registers = Registers;
  M.instructions = std::move(Code);
  M.tries = std::move(Tries);
  M.code_end = M.instructions.empty() ? 0 : M.instructions.back().pc + 1;
  return M;
}
Class klass(std::string Name, std::vector<Method> Methods = {}) {
  Class C;
  C.name = std::move(Name);
  C.superclass = "Ljava/lang/Object;";
  C.access = {"public"};
  C.source_id = "owned-fixture";
  C.methods = std::move(Methods);
  for (auto &M : C.methods)
    M.reference.owner = C.name;
  return C;
}
llvm::json::Object recover(std::vector<Class> Input, Limits L = {}) {
  ClassMap Classes;
  for (auto &C : Input) {
    std::string Name = C.name;
    Classes.emplace(std::move(Name), std::move(C));
  }
  Budget B(L);
  return recoverJava(Classes, B);
}
llvm::json::Object recoverMethods(std::vector<Method> Methods, Limits L = {}) {
  return recover({klass("Lfixture/Core;", std::move(Methods))}, L);
}
std::string source(const llvm::json::Object &Report, size_t Index = 0) {
  const auto *Units = Report.getArray("source_units");
  if (!Units || Index >= Units->size())
    return {};
  const auto *Unit = (*Units)[Index].getAsObject();
  if (!Unit)
    return {};
  auto Source = Unit->getString("source");
  return Source ? Source->str() : "";
}
void rejected(const std::function<void()> &Run, std::string_view Fragment) {
  try {
    Run();
    FAIL() << "accepted an unsupported source body";
  } catch (const std::runtime_error &E) {
    EXPECT_NE(std::string(E.what()).find(Fragment), std::string::npos)
        << E.what();
  }
}

// The binary descriptor is deliberately unrelated to the source local name.
// These direct-emitter fixtures also exercise validation without linkClasses.
std::vector<Class>
localClasses(std::string Entry = "run", std::string Parameter = "I",
             std::string Binary = "Lfixture/Core$17Worker;") {
  MethodRef Init{Binary, "<init>", {}, "V"};
  MethodRef Value{Binary, "value", {"I"}, "I"};
  auto Run = method(Entry, {Parameter}, "I", 3,
                    {op(0, "new-instance", {0}, {}, Binary),
                     op(1, "invoke-direct", {0}, {}, Init),
                     op(2, "invoke-virtual", {0, 2}, {}, Value),
                     op(3, "move-result", {1}), op(4, "return", {1})});
  Class Outer = klass("Lfixture/Core;", {Run});
  FieldRef Count{Outer.name, "count", "I"};
  Outer.fields.push_back({Count, {"public", "static"}, {}});
  auto Ctor = method("<init>", {}, "V", 2,
                     {op(0, "invoke-direct", {1}, {},
                         MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                      op(1, "sget", {0}, {}, Count),
                      op(2, "add-int/lit8", {0, 0}, int64_t(1)),
                      op(3, "sput", {0}, {}, Count), op(4, "return-void")});
  Ctor.access = {"constructor"};
  auto ValueBody = method("value", {"I"}, "I", 2, {op(0, "return", {1})});
  ValueBody.access = {"public"};
  Class Local = klass(Binary, {Ctor, ValueBody});
  Local.access.clear();
  Local.inner_class_present = true;
  Local.inner_name = "Worker";
  Local.enclosing_method = Run.reference;
  Local.source_id = "local-second-dex";
  return {Outer, Local};
}

TEST(MobileDalvikJava, LocalBodiesAndEnclosingMethodRemainExplicitProjections) {
  auto Input = localClasses();
  Input[0].methods.push_back(
      method("ordinary", {"I"}, "I", 1, {op(0, "return", {0})}));
  auto R = recover(Input);
  EXPECT_EQ(R.getString("status"), "partial");
  EXPECT_EQ(R.getInteger("method_count"), 4);
  EXPECT_EQ(R.getInteger("projected_method_count"), 3);
  EXPECT_EQ(R.getInteger("recovered_method_count"), 1);
  EXPECT_EQ(R.getInteger("declaration_only_method_count"), 0);
  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
  const auto *Units = R.getArray("source_units");
  ASSERT_NE(Units, nullptr);
  ASSERT_EQ(Units->size(), 1u);
  EXPECT_EQ((*Units)[0].getAsObject()->getString("path"), "fixture/Core.java");
  const auto Text = source(R);
  const auto MethodStart = Text.find("int run(int arg0) {");
  const auto LocalStart = Text.find("class Worker {");
  const auto OuterRegisters = Text.find("\n    int v0 = 0;", LocalStart);
  ASSERT_NE(MethodStart, std::string::npos);
  ASSERT_NE(LocalStart, std::string::npos);
  ASSERT_NE(OuterRegisters, std::string::npos);
  EXPECT_LT(MethodStart, LocalStart);
  EXPECT_LT(LocalStart, OuterRegisters);
  EXPECT_EQ(Text.find("class Core$17Worker"), std::string::npos);
  EXPECT_EQ(Text.find("static class Worker"), std::string::npos);
  EXPECT_NE(Text.find("new Worker()"), std::string::npos);
  EXPECT_NE(Text.find("super();"), std::string::npos);
  EXPECT_NE(Text.find(".count = v0;"), std::string::npos);
  EXPECT_NE(Text.find("private <E extends java.lang.Throwable>"),
            std::string::npos);
  const auto *Rows = R.getArray("methods");
  ASSERT_NE(Rows, nullptr);
  for (const auto &Value : *Rows) {
    const auto *Row = Value.getAsObject();
    ASSERT_NE(Row, nullptr);
    const bool Ordinary = Row->getString("name") == "ordinary";
    EXPECT_EQ(Row->getString("status"),
              Ordinary ? "recovered" : "source-projected");
    EXPECT_EQ(Row->getString("projection_kind").has_value(), !Ordinary);
    EXPECT_GT(Row->getInteger("instruction_count").value_or(0), 0);
  }
  const auto *Bindings = R.getArray("class_source_bindings");
  ASSERT_NE(Bindings, nullptr);
  ASSERT_EQ(Bindings->size(), 1u);
  const auto *Binding = (*Bindings)[0].getAsObject();
  ASSERT_NE(Binding, nullptr);
  EXPECT_EQ(Binding->getString("class"), Input[1].name);
  EXPECT_EQ(Binding->getString("input"), "local-second-dex");
  EXPECT_EQ(Binding->getString("source_unit"), "fixture/Core.java");
  EXPECT_EQ(Binding->getString("source_name"), "Worker");
  EXPECT_EQ(Binding->getString("binary_name_status"), "unverified");
  EXPECT_EQ(Binding->get("generated_binary_name"), nullptr);
  const auto *Enclosing = Binding->getObject("enclosing_method");
  ASSERT_NE(Enclosing, nullptr);
  EXPECT_EQ(Enclosing->getString("identity"),
            Input[0].methods[0].reference.identity());
  EXPECT_EQ(Enclosing->getString("prototype"), "(I)I");
  ASSERT_NE(Enclosing->getArray("parameters"), nullptr);
  ASSERT_EQ(Enclosing->getArray("parameters")->size(), 1u);
  EXPECT_EQ((*Enclosing->getArray("parameters"))[0].getAsString(), "I");
}

TEST(MobileDalvikJava,
     SameLocalNameInDifferentOverloadsHasExactLexicalBinding) {
  auto First = localClasses();
  auto Second = localClasses("run", "B", "Lfixture/Core$91Worker;");
  First[0].methods.push_back(Second[0].methods[0]);
  First.push_back(Second[1]);
  auto R = recover(First);
  EXPECT_EQ(R.getInteger("projected_method_count"), 6);
  const auto *Bindings = R.getArray("class_source_bindings");
  ASSERT_NE(Bindings, nullptr);
  ASSERT_EQ(Bindings->size(), 2u);
  std::set<std::string> Prototypes;
  for (const auto &Value : *Bindings) {
    const auto *Binding = Value.getAsObject();
    ASSERT_NE(Binding, nullptr);
    EXPECT_EQ(Binding->getString("source_name"), "Worker");
    const auto *Enclosing = Binding->getObject("enclosing_method");
    ASSERT_NE(Enclosing, nullptr);
    ASSERT_TRUE(Enclosing->getString("prototype").has_value());
    Prototypes.insert(Enclosing->getString("prototype")->str());
  }
  EXPECT_EQ(Prototypes, (std::set<std::string>{"(B)I", "(I)I"}));
  const auto Text = source(R);
  EXPECT_NE(Text.find("int run(byte arg0)"), std::string::npos);
  const auto FirstLocal = Text.find("class Worker {");
  ASSERT_NE(FirstLocal, std::string::npos);
  EXPECT_NE(Text.find("class Worker {", FirstLocal + 1), std::string::npos);
}

TEST(MobileDalvikJava, ProjectedExportsAccountForEveryAuxiliaryDeclaration) {
  auto Input = localClasses();
  Input[0].fields.push_back({{Input[0].name, "constant", "I"},
                             {"public", "static", "final"},
                             int64_t(7)});
  auto Clash = method("__neverdThrow", {"I"}, "I", 2, {op(0, "return", {1})});
  Clash.reference.owner = Input[1].name;
  Clash.access = {"private"};
  Input[1].methods.push_back(Clash);
  auto R = recover(Input);
  const auto *Helpers = R.getArray("generated_source_helpers");
  ASSERT_NE(Helpers, nullptr);
  ASSERT_EQ(Helpers->size(), 5u);
  std::set<std::tuple<std::string, std::string, std::string, bool>> Actual;
  for (const auto &Value : *Helpers) {
    const auto *Helper = Value.getAsObject();
    ASSERT_NE(Helper, nullptr);
    for (const auto *Key : {"class", "name", "prototype", "kind"})
      ASSERT_TRUE(Helper->getString(Key).has_value());
    ASSERT_TRUE(Helper->getBoolean("static").has_value());
    EXPECT_EQ(Helper->getString("source_unit"), "fixture/Core.java");
    Actual.emplace(
        Helper->getString("class")->str(), Helper->getString("name")->str(),
        Helper->getString("prototype")->str(), *Helper->getBoolean("static"));
    EXPECT_EQ(Helper->getString("kind"),
              Helper->getString("name") == "<init>"     ? "default-constructor"
              : Helper->getString("name") == "<clinit>" ? "field-initializer"
              : Helper->getString("name") == "__neverdConstant"
                  ? "constant-helper"
                  : "throw-helper");
  }
  const std::string ThrowType =
      "(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;";
  EXPECT_TRUE(Actual.contains({Input[0].name, "<init>", "()V", false}));
  EXPECT_TRUE(Actual.contains({Input[0].name, "<clinit>", "()V", true}));
  EXPECT_TRUE(
      Actual.contains({Input[0].name, "__neverdThrow", ThrowType, true}));
  EXPECT_TRUE(
      Actual.contains({Input[0].name, "__neverdConstant", "(I)I", true}));
  EXPECT_TRUE(
      Actual.contains({Input[1].name, "__neverdThrow_", ThrowType, false}));
  EXPECT_EQ(R.getInteger("method_count"), 4);
  EXPECT_EQ(R.getInteger("projected_method_count"), 4);
  auto Ordinary =
      recoverMethods({method("id", {"I"}, "I", 1, {op(0, "return", {0})})});
  EXPECT_EQ(Ordinary.get("projected_method_count"), nullptr);
  EXPECT_EQ(Ordinary.get("class_source_bindings"), nullptr);
  EXPECT_EQ(Ordinary.get("generated_source_helpers"), nullptr);
}

TEST(MobileDalvikJava, OriginalClassInitializerNeverBecomesAnAuxiliaryMethod) {
  auto Input = localClasses();
  Input[0].fields[0].value = int64_t(7);
  auto WithoutInitializer = recover(Input);
  const auto *Extra = WithoutInitializer.getArray("generated_source_helpers");
  ASSERT_NE(Extra, nullptr);
  ASSERT_EQ(Extra->size(), 4u);
  size_t Synthesized = 0;
  for (const auto &Value : *Extra)
    if (Value.getAsObject()->getString("name") == "<clinit>") {
      ++Synthesized;
      EXPECT_EQ(Value.getAsObject()->getString("kind"), "field-initializer");
      EXPECT_EQ(Value.getAsObject()->getBoolean("static"), true);
    }
  EXPECT_EQ(Synthesized, 1u);

  auto Init = method("<clinit>", {}, "V", 0, {op(0, "return-void")});
  Init.access = {"static", "constructor"};
  Input[0].methods.push_back(Init);
  auto WithInitializer = recover(Input);
  EXPECT_EQ(WithInitializer.getInteger("method_count"), 4);
  EXPECT_EQ(WithInitializer.getInteger("projected_method_count"), 3);
  EXPECT_EQ(WithInitializer.getInteger("recovered_method_count"), 1);
  const auto *Helpers = WithInitializer.getArray("generated_source_helpers");
  ASSERT_NE(Helpers, nullptr);
  ASSERT_EQ(Helpers->size(), 3u);
  for (const auto &Value : *Helpers)
    EXPECT_NE(Value.getAsObject()->getString("name"), "<clinit>");
  const auto *Methods = WithInitializer.getArray("methods");
  ASSERT_NE(Methods, nullptr);
  size_t Original = 0;
  for (const auto &Value : *Methods)
    if (Value.getAsObject()->getString("name") == "<clinit>") {
      ++Original;
      EXPECT_EQ(Value.getAsObject()->getString("status"), "recovered");
      EXPECT_EQ(Value.getAsObject()->getString("identity"),
                Init.reference.identity());
    }
  EXPECT_EQ(Original, 1u);
}

TEST(MobileDalvikJava,
     LocalProjectionPreservesCatchCarrierWithoutEscapingReceiver) {
  auto Input = localClasses();
  auto &Value = Input[1].methods[1];
  Value.registers = 4;
  Value.instructions = {op(0, "const/4", {0}, int64_t(7)),
                        op(1, "div-int", {0, 0, 3}),
                        op(2, "return", {0}),
                        op(3, "move-exception", {1}),
                        op(4, "move-object", {0, 1}),
                        op(5, "throw", {0})};
  Value.tries = {{1, 2, {{std::string("Ljava/lang/ArithmeticException;"), 3}}}};
  Value.code_end = 6;
  auto R = recover(Input);
  EXPECT_EQ(R.getInteger("projected_method_count"), 3);
  EXPECT_NE(source(R).find("failure instanceof java.lang.ArithmeticException"),
            std::string::npos);
  EXPECT_NE(source(R).find("throw __neverdThrow("), std::string::npos);
}

TEST(MobileDalvikJava, LocalProjectionRejectsUnprovedObjectEffects) {
  for (const auto &Bad : std::vector<Instruction>{
           op(2, "check-cast", {0}, {}, std::string("Ljava/lang/Object;")),
           op(2, "instance-of", {1, 0}, {}, std::string("Ljava/lang/Object;")),
           op(2, "invoke-static", {0}, {},
              MethodRef{
                  "Lexternal/Sink;", "take", {"Ljava/lang/Object;"}, "V"}),
           op(2, "invoke-virtual", {0}, {},
              MethodRef{"Ljava/lang/Object;", "hashCode", {}, "I"}),
           op(2, "new-array", {1, 2}, {}, std::string("[I")),
           op(2, "const-class", {1}, {}, std::string("Ljava/lang/Object;"))}) {
    auto Input = localClasses();
    auto &Code = Input[0].methods[0].instructions;
    Code.insert(Code.begin() + 2, Bad);
    for (size_t I = 0; I < Code.size(); ++I)
      Code[I].pc = static_cast<uint32_t>(I);
    Input[0].methods[0].code_end = static_cast<uint32_t>(Code.size());
    rejected([&] { recover(Input); }, "local-class object");
  }
}

TEST(MobileDalvikJava, LocalProjectionDoesNotUseNullAsProvenReceiver) {
  auto Input = localClasses();
  auto &Code = Input[0].methods[0].instructions;
  Code.insert(Code.begin() + 2, op(2, "const/4", {0}, int64_t(0)));
  for (size_t I = 0; I < Code.size(); ++I)
    Code[I].pc = static_cast<uint32_t>(I);
  Input[0].methods[0].code_end = static_cast<uint32_t>(Code.size());
  rejected([&] { recover(Input); }, "local-class object receiver");
}

TEST(MobileDalvikJava, LocalOriginsJoinAllNormalPredecessors) {
  auto Input = localClasses();
  auto &Run = Input[0].methods[0];
  const MethodRef Init{Input[1].name, "<init>", {}, "V"};
  const MethodRef Value{Input[1].name, "value", {"I"}, "I"};
  Run.instructions = {op(0, "if-eqz", {2}, {}, {}, 4),
                      op(1, "new-instance", {0}, {}, Input[1].name),
                      op(2, "invoke-direct", {0}, {}, Init),
                      op(3, "goto", {}, {}, {}, 6),
                      op(4, "new-instance", {0}, {}, Input[1].name),
                      op(5, "invoke-direct", {0}, {}, Init),
                      op(6, "invoke-virtual", {0, 2}, {}, Value),
                      op(7, "move-result", {1}),
                      op(8, "return", {1})};
  Run.code_end = 9;
  auto R = recover(Input);
  EXPECT_EQ(R.getInteger("projected_method_count"), 3);

  // Both static types are assignable to Worker, but one predecessor supplies
  // null. A union that keeps only the first local origin would accept this.
  Run.instructions[4] = op(4, "const/4", {0}, int64_t(0));
  Run.instructions[5] = op(5, "nop");
  rejected([&] { recover(Input); }, "local-class object receiver");
}

TEST(MobileDalvikJava, LocalOriginsUsePreWriteStateOnExceptionalEdges) {
  auto Input = localClasses();
  auto &Run = Input[0].methods[0];
  Run.instructions = {op(0, "new-instance", {0}, {}, Input[1].name),
                      op(1, "invoke-direct", {0}, {},
                         MethodRef{Input[1].name, "<init>", {}, "V"}),
                      op(2, "div-int", {0, 2, 2}),
                      op(3, "return", {0}),
                      op(4, "move-exception", {1}),
                      op(5, "invoke-virtual", {0, 2}, {},
                         MethodRef{Input[1].name, "value", {"I"}, "I"}),
                      op(6, "move-result", {1}),
                      op(7, "return", {1})};
  Run.tries = {{2, 3, {{std::string("Ljava/lang/ArithmeticException;"), 4}}}};
  Run.code_end = 8;
  auto R = recover(Input);
  EXPECT_EQ(R.getInteger("projected_method_count"), 3);
  EXPECT_NE(source(R).find("((Worker) o0).value("), std::string::npos);
}

TEST(MobileDalvikJava, LocalOriginsJoinEveryThrowingSiteInOneHandler) {
  auto Input = localClasses();
  auto &Run = Input[0].methods[0];
  Run.instructions = {op(0, "new-instance", {0}, {}, Input[1].name),
                      op(1, "invoke-direct", {0}, {},
                         MethodRef{Input[1].name, "<init>", {}, "V"}),
                      op(2, "div-int", {1, 2, 2}),
                      op(3, "const/4", {0}, int64_t(0)),
                      op(4, "div-int", {1, 2, 0}),
                      op(5, "return", {1}),
                      op(6, "move-exception", {1}),
                      op(7, "invoke-virtual", {0, 2}, {},
                         MethodRef{Input[1].name, "value", {"I"}, "I"}),
                      op(8, "move-result", {1}),
                      op(9, "return", {1})};
  Run.tries = {{2, 5, {{std::string("Ljava/lang/ArithmeticException;"), 6}}}};
  Run.code_end = 10;
  rejected([&] { recover(Input); }, "local-class object receiver");
}

TEST(MobileDalvikJava, LocalSourceNamesCannotShadowRuntimeOrEnclosingClass) {
  for (const std::string Name : {"java", "Core"}) {
    auto Input = localClasses();
    Input[1].inner_name = Name;
    rejected([&] { recover(Input); },
             Name == "java" ? "runtime package" : "enclosing class name");
  }
}

TEST(MobileDalvikJava, LocalProjectionRejectsInvalidScopesAndJava8Modifiers) {
  for (const auto *Flag : {"public", "protected", "private", "static"}) {
    for (bool Inner : {false, true}) {
      auto Input = localClasses();
      (Inner ? Input[1].inner_access : Input[1].access).insert(Flag);
      rejected([&] { recover(Input); }, "Java 8 local class");
    }
  }
  auto Input = localClasses();
  auto Other = Input[0].methods[0];
  Other.reference.name = "unrelated";
  Input[0].methods.push_back(Other);
  rejected([&] { recover(Input); }, "local type reference");
  Input = localClasses();
  auto Duplicate = Input[1];
  Duplicate.name = "Lfixture/Core$18Worker;";
  for (auto &M : Duplicate.methods)
    M.reference.owner = Duplicate.name;
  Input.push_back(Duplicate);
  rejected([&] { recover(Input); }, "duplicate local source name");
  Input = localClasses();
  Input[1].enclosing_method->parameters = {"J"};
  rejected([&] { recover(Input); }, "exact enclosing method definition");
}

TEST(MobileDalvikJava, OrdinarySourceBytesAndGenerationChargesRemainUnchanged) {
  Class Ordinary = klass("Lordinary/Keep;",
                         {method("done", {}, "V", 0, {op(0, "return-void")})});
  ClassMap Classes;
  Classes.emplace(Ordinary.name, Ordinary);
  Budget B;
  auto Before = recoverJava(Classes, B);
  const auto Text = source(Before);
  const std::string Prefix = "package ordinary;\n\n";
  const std::string Header = "  public static void done() {\n";
  ASSERT_TRUE(Text.starts_with(Prefix));
  auto Begin = Text.find(Header);
  ASSERT_NE(Begin, std::string::npos);
  Begin += Header.size();
  const auto End = Text.find("\n  }\n", Begin);
  ASSERT_NE(End, std::string::npos);
  const auto IndentedBody = Text.substr(Begin, End - Begin);
  const uint64_t Lines =
      1 + std::count(IndentedBody.begin(), IndentedBody.end(), '\n');
  // Existing accounting charges Body's own lines once, then the containing
  // class once. A new cumulative prelude wrapper must not charge it a third
  // time. The four spaces per body line are added only by the class emitter.
  const uint64_t BodyBytes = IndentedBody.size() + 1 - 4 * Lines;
  EXPECT_EQ(B.output_bytes, Text.size() - Prefix.size() + BodyBytes);

  auto Input = localClasses();
  Input.push_back(Ordinary);
  auto After = recover(Input);
  const auto *Units = After.getArray("source_units");
  ASSERT_NE(Units, nullptr);
  bool Found = false;
  for (const auto &Value : *Units) {
    const auto *Unit = Value.getAsObject();
    ASSERT_NE(Unit, nullptr);
    if (Unit->getString("class") == Ordinary.name) {
      Found = true;
      EXPECT_EQ(Unit->getString("source"), Text);
    }
  }
  EXPECT_TRUE(Found);
  Limits Small;
  Small.max_bytes = 128;
  rejected([&] { recover(localClasses(), Small); }, "byte budget");
}

TEST(MobileDalvikJava, RejectsUndefinedRegisterBeforePublication) {
  rejected(
      [] {
        recoverMethods({method("bad", {}, "I", 1, {op(0, "return", {0})})});
      },
      "undefined");
}
TEST(MobileDalvikJava, DefinitionOnOneConditionalPathDoesNotInitializeTheJoin) {
  rejected(
      [] {
        recoverMethods({method("bad", {"I"}, "I", 2,
                               {op(0, "if-eqz", {1}, {}, {}, 2),
                                op(1, "const/4", {0}, int64_t(7)),
                                op(2, "return", {0})})});
      },
      "undefined");
}
TEST(MobileDalvikJava, OverwrittenHighWordCannotSupplyAWideReturn) {
  rejected(
      [] {
        recoverMethods({method("bad", {}, "J", 2,
                               {op(0, "const-wide/16", {0}, int64_t(9)),
                                op(1, "const/4", {1}, int64_t(0)),
                                op(2, "return-wide", {0})})});
      },
      "undefined");
}
TEST(MobileDalvikJava, RetiresBothWordsOfAnOverlappingOldPair) {
  auto R = recoverMethods(
      {method("overlap", {}, "J", 4,
              {op(0, "const-wide/16", {2}, int64_t(9)),
               op(1, "const-wide/16", {1}, int64_t(7)),
               op(2, "const/4", {3}, int64_t(0)), op(3, "return-wide", {1})})});
  EXPECT_EQ(R.getInteger("recovered_method_count"), 1);
  EXPECT_NE(source(R).find("w1 = 0x0000000000000007L;"), std::string::npos);
  EXPECT_NE(source(R).find("return w1;"), std::string::npos);
}
TEST(MobileDalvikJava, BranchCannotBypassInvocationResultProducer) {
  rejected(
      [] {
        MethodRef Ref{"Ljava/lang/Math;", "abs", {"I"}, "I"};
        recoverMethods(
            {method("bad", {"I"}, "I", 2,
                    {op(0, "if-eqz", {1}, {}, {}, 2),
                     op(1, "invoke-static", {1}, {}, Ref),
                     op(2, "move-result", {0}), op(3, "return", {0})})});
      },
      "bypasses");
}
TEST(MobileDalvikJava, ExceptionalEdgeUsesStateBeforeTheThrowingWrite) {
  rejected(
      [] {
        recoverMethods({method(
            "bad", {"I", "I"}, "I", 4,
            {op(0, "div-int", {0, 2, 3}), op(1, "return", {0}),
             op(2, "move-exception", {1}), op(3, "return", {0})},
            {{0, 1, {{std::string("Ljava/lang/ArithmeticException;"), 2}}}})});
      },
      "undefined");
}
TEST(MobileDalvikJava, NormalFlowCannotEnterAnExceptionOnlyValue) {
  rejected(
      [] {
        recoverMethods({method(
            "bad", {}, "V", 1,
            {op(0, "nop"), op(1, "move-exception", {0}), op(2, "return-void")},
            {{0, 1, {{std::nullopt, 1}}}})});
      },
      "normal flow");
}
TEST(MobileDalvikJava, UnknownAndUnreachableOperationsAreNotEmptyBodies) {
  rejected(
      [] {
        recoverMethods({method("bad", {}, "V", 0,
                               {op(0, "unrecognized"), op(1, "return-void")})});
      },
      "unsupported instruction");
  rejected(
      [] {
        recoverMethods({method("bad", {}, "V", 0,
                               {op(0, "return-void"), op(1, "unrecognized")})});
      },
      "unreachable");
}
TEST(MobileDalvikJava, LocalCalleeIdentityIncludesReturnTypeAndStaticKind) {
  Method Target = method("target", {"I"}, "I", 1, {op(0, "return", {0})});
  rejected(
      [&] {
        MethodRef Ref{"Lfixture/Core;", "target", {"I"}, "J"};
        recoverMethods({Target, method("bad", {"I"}, "J", 3,
                                       {op(0, "invoke-static", {2}, {}, Ref),
                                        op(1, "move-result-wide", {0}),
                                        op(2, "return-wide", {0})})});
      },
      "member type differs");
  rejected(
      [&] {
        MethodRef Ref{"Lfixture/Core;", "target", {"I"}, "I"};
        recoverMethods(
            {Target,
             method("bad", {"Lfixture/Core;", "I"}, "I", 3,
                    {op(0, "invoke-virtual", {1, 2}, {}, Ref),
                     op(1, "move-result", {0}), op(2, "return", {0})})});
      },
      "static/instance");
}
TEST(MobileDalvikJava, MatchingLocalCalleeRetainsArgumentsAndResult) {
  MethodRef Ref{"Lfixture/Core;", "target", {"I"}, "I"};
  auto R = recoverMethods(
      {method("target", {"I"}, "I", 1, {op(0, "return", {0})}),
       method("caller", {"I"}, "I", 2,
              {op(0, "invoke-static", {1}, {}, Ref), op(1, "move-result", {0}),
               op(2, "return", {0})})});
  EXPECT_EQ(R.getInteger("recovered_method_count"), 2);
  EXPECT_NE(source(R).find("result32 = ((fixture.Core) null).target(v1);"),
            std::string::npos);
  EXPECT_NE(source(R).find("v0 = result32;"), std::string::npos);
}
TEST(MobileDalvikJava,
     ExactFieldBindingRejectsWrongTypeStaticKindAndMissingMember) {
  Class C = klass("Lfixture/Core;");
  C.fields.push_back(
      {{"Lfixture/Core;", "value", "I"}, {"public", "static"}, {}});
  for (const auto &Ref : {FieldRef{"Lfixture/Core;", "value", "J"},
                          FieldRef{"Lfixture/Core;", "missing", "I"}}) {
    Class Input = C;
    Input.methods = {
        method("bad", {}, Ref.type, 2,
               {op(0, Ref.type == "J" ? "sget-wide" : "sget", {0}, {}, Ref),
                op(1, Ref.type == "J" ? "return-wide" : "return", {0})})};
    rejected([&] { recover({Input}); }, "member");
  }
  C.methods = {method(
      "bad", {"Lfixture/Core;"}, "I", 2,
      {op(0, "iget", {0, 1}, {}, FieldRef{"Lfixture/Core;", "value", "I"}),
       op(1, "return", {0})})};
  rejected([&] { recover({C}); }, "static/instance");
}
TEST(MobileDalvikJava, SuperCallRequiresActualThisProvenance) {
  MethodRef Ref{"Ljava/lang/Object;", "hashCode", {}, "I"};
  Method M = method("run", {"Lfixture/Core;"}, "I", 3,
                    {op(0, "invoke-super", {2}, {}, Ref),
                     op(1, "move-result", {0}), op(2, "return", {0})});
  M.access.erase("static");
  rejected([&] { recoverMethods({M}); }, "actual receiver");
  M.instructions[0].registers = {1};
  auto R = recoverMethods({M});
  EXPECT_NE(source(R).find("result32 = super.hashCode();"), std::string::npos);
}
TEST(MobileDalvikJava,
     ExternalSuperConstructorRequiresInheritedTypeDeclarations) {
  MethodRef Ref{
      "Ljava/io/FileInputStream;", "<init>", {"Ljava/lang/String;"}, "V"};
  Method M =
      method("<init>", {"Ljava/lang/String;"}, "V", 2,
             {op(0, "invoke-direct", {0, 1}, {}, Ref), op(1, "return-void")});
  M.access = {"public", "constructor"};
  Class C = klass("Lfixture/Core;", {M});
  C.superclass = Ref.owner;
  // The external superclass may contribute member types that obscure
  // java.lang.String or the runtime helpers. Missing declarations do not
  // establish that those qualified names retain their intended bindings.
  rejected([&] { recover({C}); }, "unresolved inherited Java type");
}
TEST(MobileDalvikJava,
     KnownSuperConstructorPreservesCheckedThrowableAndOriginalArguments) {
  Method BaseInit =
      method("<init>", {"Ljava/lang/Throwable;"}, "V", 2,
             {op(0, "invoke-direct", {0}, {},
                 MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
              op(1, "if-eqz", {1}, {}, {}, 3), op(2, "throw", {1}),
              op(3, "return-void")});
  BaseInit.access = {"public", "constructor"};
  Class Base = klass("Lfixture/CheckedBase;", {BaseInit});
  Method M = method(
      "<init>", {"Ljava/lang/Throwable;"}, "V", 2,
      {op(0, "invoke-direct", {0, 1}, {},
          MethodRef{Base.name, "<init>", {"Ljava/lang/Throwable;"}, "V"}),
       op(1, "return-void")});
  M.access = {"public", "constructor"};
  Class C = klass("Lfixture/Core;", {M});
  C.superclass = Base.name;
  auto Report = recover({Base, C});
  EXPECT_EQ(Report.getInteger("recovered_method_count"), 2);
  EXPECT_NE(source(Report).find("throw __neverdThrow((java.lang.Throwable)"),
            std::string::npos);
  EXPECT_NE(source(Report, 1).find("super(((java.lang.Throwable) arg0));"),
            std::string::npos);
  C.methods[0].instructions[0].registers = {0, 0};
  rejected([&] { recover({Base, C}); }, "unavailable argument");
}
TEST(MobileDalvikJava,
     AllocationRequiresAdjacentMatchingInitializerAndNoBypass) {
  MethodRef Ref{"Ljava/lang/Object;", "<init>", {}, "V"};
  auto Good = method(
      "allocate", {}, "Ljava/lang/Object;", 1,
      {op(0, "new-instance", {0}, {}, std::string("Ljava/lang/Object;")),
       op(1, "invoke-direct", {0}, {}, Ref), op(2, "return-object", {0})});
  auto R = recoverMethods({Good});
  EXPECT_NE(source(R).find("o0 = new java.lang.Object();"), std::string::npos);
  Good.instructions[1] = op(1, "nop");
  rejected([&] { recoverMethods({Good}); }, "adjacent initializer");
}
TEST(MobileDalvikJava, FinalConstantsAvoidCompileTimeInliningAndKeepFloatBits) {
  Class C = klass("Lfixture/Core;");
  C.fields = {
      {{C.name, "magic", "I"}, {"public", "static", "final"}, int64_t(7)},
      {{C.name, "negativeZero", "F"},
       {"public", "static", "final"},
       FloatBits{UINT64_C(0x80000000), false}},
      {{C.name, "nan", "D"},
       {"public", "static", "final"},
       FloatBits{UINT64_C(0x7ff8123456789abc), true}}};
  auto R = recover({C});
  auto S = source(R);
  EXPECT_NE(S.find("magic = __neverdConstant(0x00000007);"), std::string::npos);
  EXPECT_NE(S.find("intBitsToFloat(0x80000000)"), std::string::npos);
  EXPECT_NE(S.find("longBitsToDouble(0x7ff8123456789abcL)"), std::string::npos);
  C.fields[0].value = std::monostate{};
  rejected([&] { recover({C}); }, "verified initialization");
}
TEST(MobileDalvikJava, NativeAndAbstractDeclarationsHaveSeparateCoverage) {
  Class C = klass("Lfixture/Core;", {method("body", {}, "I", 1,
                                            {op(0, "const/4", {0}, int64_t(42)),
                                             op(1, "return", {0})})});
  C.access.insert("abstract");
  Method Native = method("nativeEntry", {"J"}, "I", 0, {});
  Native.access.insert("native");
  Method Abstract = method("abstractEntry", {}, "V", 0, {});
  Abstract.access = {"public", "abstract"};
  C.methods.push_back(Native);
  C.methods.push_back(Abstract);
  auto R = recover({C});
  EXPECT_EQ(R.getInteger("method_count"), 3);
  EXPECT_EQ(R.getInteger("recovered_method_count"), 1);
  EXPECT_EQ(R.getInteger("declaration_only_method_count"), 2);
  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
  ASSERT_EQ(R.getArray("methods")->size(), 3u);
  EXPECT_NE(source(R).find("abstract void abstractEntry();"),
            std::string::npos);
}
TEST(MobileDalvikJava, NestedClassesShareAUnitWithoutDroppingMethods) {
  Class Outer = klass(
      "Lfixture/Outer;",
      {method("outer", {}, "I", 1,
              {op(0, "const/4", {0}, int64_t(1)), op(1, "return", {0})})});
  Class Inner = klass(
      "Lfixture/Outer$Nested;",
      {method("inner", {}, "I", 1,
              {op(0, "const/4", {0}, int64_t(2)), op(1, "return", {0})})});
  Inner.enclosing = Outer.name;
  Inner.inner_class_present = true;
  Inner.inner_name = "Nested";
  Inner.inner_access = {"public", "static"};
  auto R = recover({Outer, Inner});
  EXPECT_EQ(R.getInteger("class_count"), 2);
  EXPECT_EQ(R.getInteger("method_count"), 2);
  ASSERT_EQ(R.getArray("source_units")->size(), 1u);
  EXPECT_NE(source(R).find("public static class Nested"), std::string::npos);
  EXPECT_NE(source(R).find("int inner()"), std::string::npos);
  Inner.inner_access.erase("static");
  rejected([&] { recover({Outer, Inner}); }, "outer-instance initialization");
}
TEST(MobileDalvikJava,
     CyclicOrExcessiveNestedOwnershipCannotReachTypeRecursion) {
  Class A = klass("LA;"), B = klass("LB;");
  A.enclosing = B.name;
  A.inner_class_present = true;
  A.inner_name = "A";
  A.inner_access = {"static"};
  B.enclosing = A.name;
  B.inner_class_present = true;
  B.inner_name = "B";
  B.inner_access = {"static"};
  rejected([&] { recover({A, B}); }, "recursive nested");
  std::vector<Class> Classes;
  for (unsigned I = 0; I < 129; ++I) {
    Class C = klass("LC" + std::to_string(I) + ";");
    if (I) {
      C.enclosing = "LC" + std::to_string(I - 1) + ";";
      C.inner_class_present = true;
      C.inner_name = "N" + std::to_string(I);
      C.inner_access = {"static"};
    }
    Classes.push_back(std::move(C));
  }
  rejected([&] { recover(Classes); }, "source depth limit");
}
TEST(
    MobileDalvikJava,
    StringEscapingPreservesLoneSurrogatesSupplementaryCharactersAndSlashUText) {
  std::string Value = "\\u000a\n";
  Value += std::string("\xed\xa0\x80", 3);     // lone UTF-16 high surrogate
  Value += std::string("\xed\xb0\x80", 3);     // lone UTF-16 low surrogate
  Value += std::string("\xf0\x9f\x98\x80", 4); // supplementary code point
  Value.push_back('\0');
  auto R = recoverMethods({method(
      "text", {}, "Ljava/lang/String;", 1,
      {op(0, "const-string", {0}, Value), op(1, "return-object", {0})})});
  EXPECT_NE(
      source(R).find("\"\\\\u000a\\n\\ud800\\udc00\\ud83d\\ude00\\u0000\""),
      std::string::npos);
  rejected(
      [] {
        recoverMethods(
            {method("bad", {}, "Ljava/lang/String;", 1,
                    {op(0, "const-string", {0}, std::string("\xc0\x80", 2)),
                     op(1, "return-object", {0})})});
      },
      "invalid UTF-8");
}
TEST(MobileDalvikJava, ArrayPayloadChecksCompleteRangeBeforeAnyStore) {
  Instruction Fill = op(0, "fill-array-data", {0});
  Fill.element_width = 4;
  Fill.data = {UINT64_C(0xffffffff), 7};
  auto R = recoverMethods(
      {method("fill", {"[I"}, "V", 1, {Fill, op(1, "return-void")})});
  auto S = source(R);
  auto Check = S.find(
      ".length < 2) throw new java.lang.ArrayIndexOutOfBoundsException();");
  auto Store = S.find("[0] = (int) 0xffffffff;");
  ASSERT_NE(Check, std::string::npos);
  ASSERT_NE(Store, std::string::npos);
  EXPECT_LT(Check, Store);
  Fill.element_width = 8;
  rejected(
      [&] {
        recoverMethods(
            {method("bad", {"[I"}, "V", 1, {Fill, op(1, "return-void")})});
      },
      "element width");
}
TEST(MobileDalvikJava, SwitchAndExceptionDispatchPreserveExplicitDestinations) {
  Instruction Switch = op(0, "sparse-switch", {1}, {}, {}, 10);
  Switch.keys = {-1, 7};
  Switch.targets = {3, 5};
  auto R = recoverMethods(
      {method("choose", {"I"}, "I", 2,
              {Switch, op(1, "const/4", {0}, int64_t(3)), op(2, "return", {0}),
               op(3, "const/4", {0}, int64_t(4)), op(4, "return", {0}),
               op(5, "const/4", {0}, int64_t(5)), op(6, "return", {0})}),
       method(
           "divide", {"I", "I"}, "I", 3,
           {op(0, "div-int", {0, 1, 2}), op(1, "return", {0}),
            op(2, "move-exception", {0}), op(3, "const/4", {0}, int64_t(-1)),
            op(4, "return", {0})},
           {{0, 1, {{std::string("Ljava/lang/ArithmeticException;"), 2}}}})});
  EXPECT_EQ(R.getInteger("recovered_method_count"), 2);
  EXPECT_NE(source(R).find("case 0xffffffff: pc = 3; break;"),
            std::string::npos);
  EXPECT_NE(source(R).find("failure instanceof java.lang.ArithmeticException"),
            std::string::npos);
  EXPECT_NE(source(R).find("caught = failure; pc = 2; continue dispatch;"),
            std::string::npos);
}
TEST(MobileDalvikJava, FloatingArgumentsAndReturnCarriersRetainRawBits) {
  auto R = recoverMethods(
      {method("floatBits", {"F"}, "F", 1, {op(0, "return", {0})}),
       method("doubleBits", {"D"}, "D", 2, {op(0, "return-wide", {0})})});
  auto S = source(R);
  EXPECT_NE(S.find("floatToRawIntBits(arg0)"), std::string::npos);
  EXPECT_NE(S.find("return ((java.lang.Float) null).intBitsToFloat(v0);"),
            std::string::npos);
  EXPECT_NE(S.find("doubleToRawLongBits(arg0)"), std::string::npos);
  EXPECT_NE(S.find("return ((java.lang.Double) null).longBitsToDouble(w0);"),
            std::string::npos);
}
TEST(MobileDalvikJava, ArithmeticAndLoopUseVerifiedMutableCarriers) {
  auto R = recoverMethods({method(
      "sum", {"[I"}, "I", 5,
      {op(0, "const/4", {0}, int64_t(0)), op(1, "const/4", {1}, int64_t(0)),
       op(2, "array-length", {2, 4}), op(3, "if-ge", {1, 2}, {}, {}, 10),
       op(4, "aget", {3, 4, 1}), op(5, "if-gez", {3}, {}, {}, 7),
       op(6, "neg-int", {3, 3}), op(7, "add-int/2addr", {0, 3}),
       op(8, "add-int/lit8", {1, 1}, int64_t(1)), op(9, "goto", {}, {}, {}, 3),
       op(10, "return", {0})})});
  auto S = source(R);
  EXPECT_NE(S.find("pc = (v1 >= v2) ? 10 : 4;"), std::string::npos);
  EXPECT_NE(S.find("v0 = ((v0) + (v3));"), std::string::npos);
  EXPECT_NE(S.find("pc = 3;"), std::string::npos);
  EXPECT_NE(S.find("return v0;"), std::string::npos);
}
TEST(MobileDalvikJava, ResourceLimitsRejectBeforeReturningSourceUnits) {
  Limits L;
  L.max_bytes = 64;
  rejected(
      [&] {
        recoverMethods({method("body", {}, "I", 1,
                               {op(0, "const/4", {0}, int64_t(42)),
                                op(1, "return", {0})})},
                       L);
      },
      "byte budget");
  Budget B;
  B.remaining = 0;
  Class C = klass("Lfixture/Core;",
                  {method("body", {}, "V", 0, {op(0, "return-void")})});
  ClassMap Classes;
  Classes.emplace(C.name, C);
  rejected([&] { recoverJava(Classes, B); }, "budget");
}
TEST(MobileDalvikJava, StaticExternalTypeQualifierCannotBindToAShadowingField) {
  Class C = klass("Lfixture/Core;");
  C.fields.push_back({{C.name, "java", "I"}, {"public", "static"}, {}});
  C.methods = {method("bad", {"I"}, "I", 1,
                      {op(0, "invoke-static", {0}, {},
                          MethodRef{"Ljava/lang/Math;", "abs", {"I"}, "I"}),
                       op(1, "move-result", {0}), op(2, "return", {0})})};
  rejected([&] { recover({C}); }, "shadowed");
}
TEST(MobileDalvikJava, SamePackageClassNameCannotObscureItsPackageQualifier) {
  Class Core = klass(
      "Lfixture/Core;",
      {method("value", {}, "I", 1,
              {op(0, "const/16", {0}, int64_t(17)), op(1, "return", {0})})});
  Class Shadow =
      klass("Lfixture/fixture;",
            {method("identity", {Core.name}, Core.name, 1,
                    {op(0, "return-object", {0})}),
             method("value", {}, "I", 1,
                    {op(0, "invoke-static", {}, {},
                        MethodRef{Core.name, "value", {}, "I"}),
                     op(1, "move-result", {0}), op(2, "return", {0})})});
  // Fields do not shadow names in a type context. The resolution must not
  // confuse this legal field with a member type named Core.
  Shadow.fields.push_back(
      {{Shadow.name, "Core", "I"}, {"public", "static"}, {}});
  const auto report = recover({Core, Shadow});
  EXPECT_EQ(report.getInteger("recovered_method_count"), 3);
  const auto text = source(report, 1);
  EXPECT_NE(text.find("Core identity(Core arg0)"), std::string::npos);
  EXPECT_NE(text.find("return ((Core) o0);"), std::string::npos);
  EXPECT_NE(text.find("((Core) null).value()"), std::string::npos);
  EXPECT_EQ(text.find("fixture.Core"), std::string::npos);
}
TEST(MobileDalvikJava, ShortenedSamePackageNameCannotSelectANestedNamesake) {
  Class Core = klass("Lfixture/Core;");
  Class Shadow =
      klass("Lfixture/fixture;", {method("identity", {Core.name}, Core.name, 1,
                                         {op(0, "return-object", {0})})});
  Class Nested = klass("Lfixture/fixture$Core;");
  Nested.enclosing = Shadow.name;
  Nested.inner_class_present = true;
  Nested.inner_name = "Core";
  Nested.inner_access = {"public", "static"};
  rejected([&] { recover({Core, Shadow, Nested}); }, "ambiguous Java type");
}
TEST(MobileDalvikJava, UnknownInheritedMemberCannotValidateAPackageQualifier) {
  // Map.Entry is inherited into the body of C. The original Java can name
  // Peer through an import, but Entry.Peer binds Entry to the inherited type.
  // Without Map's declarations, neither absence nor identity is proven.
  Class Peer = klass("LEntry/Peer;");
  Class C = klass("Limpl/C;", {method("identity", {Peer.name}, Peer.name, 1,
                                      {op(0, "return-object", {0})})});
  C.access.insert("abstract");
  C.interfaces = {"Ljava/util/Map;"};
  rejected([&] { recover({Peer, C}); }, "unresolved inherited Java type");

  Class Base = klass("Limpl/Base;");
  Base.access.insert("abstract");
  Base.interfaces = C.interfaces;
  C.interfaces.clear();
  C.superclass = Base.name;
  rejected([&] { recover({Peer, Base, C}); }, "unresolved inherited Java type");
}
TEST(MobileDalvikJava, OwnInheritedMembersAreOutsideTheSupertypeHeaderScope) {
  Class C = klass("Limpl/Bridge;");
  C.access = {"public", "interface", "abstract"};
  C.interfaces = {"Ljava/util/Map;"};
  auto Size = method("observedSize", {}, "I", 0, {});
  Size.reference.owner = C.name;
  Size.access = {"public", "abstract"};
  C.methods = {Size};
  auto Report = recover({C});
  EXPECT_EQ(Report.getInteger("declaration_only_method_count"), 1);
  EXPECT_EQ(Report.getInteger("recovered_method_count"), 0);
  EXPECT_NE(source(Report).find("interface Bridge extends java.util.Map"),
            std::string::npos);
}
TEST(MobileDalvikJava, ObjectInheritanceHasNoUnknownMemberTypeDeclarations) {
  Class Peer = klass("LEntry/Peer;");
  Class C = klass("Limpl/C;", {method("identity", {Peer.name}, Peer.name, 1,
                                      {op(0, "return-object", {0})})});
  auto Report = recover({Peer, C});
  EXPECT_EQ(Report.getInteger("recovered_method_count"), 1);
  EXPECT_NE(source(Report, 1).find("Entry.Peer identity(Entry.Peer arg0)"),
            std::string::npos);
}
TEST(MobileDalvikJava, RuntimeHelperTypesAlsoRequireProvenPackageBindings) {
  Class C = klass("Limpl/C;", {method("value", {}, "I", 1,
                                      {op(0, "const/4", {0}, int64_t(17)),
                                       op(1, "return", {0})})});
  C.interfaces = {"Lexternal/Contract;"};
  rejected([&] { recover({C}); }, "unresolved inherited Java type");
}

TEST(MobileDalvikJava, GenericReturnsRetainErasedRegisterCarriers) {
  auto Identity =
      method("identity", {"Ljava/lang/Object;"}, "Ljava/lang/Object;", 1,
             {op(0, "return-object", {0})});
  Identity.generic_signature = "<T:Ljava/lang/Object;>(TT;)TT;";
  auto Array = method("array", {"[Ljava/lang/Object;"}, "[Ljava/lang/Object;",
                      1, {op(0, "return-object", {0})});
  Array.generic_signature = "<T:Ljava/lang/Object;>([TT;)[TT;";
  auto R = recoverMethods({Identity, Array});
  auto Text = source(R);
  EXPECT_NE(Text.find("<T extends java.lang.Object> T identity(T arg0)"),
            std::string::npos);
  EXPECT_NE(Text.find("T[] array(T[] arg0)"), std::string::npos);
  EXPECT_NE(Text.find("return ((T) ((java.lang.Object) o0));"),
            std::string::npos);
  EXPECT_NE(Text.find("return ((T[]) ((java.lang.Object[]) o0));"),
            std::string::npos);
  EXPECT_NE(Text.find("java.lang.Object o0 = null;"), std::string::npos);
  EXPECT_EQ(R.getInteger("recovered_method_count"), 2);
  ASSERT_NE(R.getArray("generated_source_helpers"), nullptr);
  EXPECT_EQ(R.getArray("generated_source_helpers")->size(), 2);
}

TEST(MobileDalvikJava, ClassVariablesAndMethodShadowingHaveDistinctBindings) {
  FieldRef Value{"Lfixture/Core;", "value", "Ljava/lang/Object;"};
  auto Exchange = method(
      "exchange", {"Ljava/lang/Object;"}, "Ljava/lang/Object;", 3,
      {op(0, "iget-object", {0, 1}, {}, Value),
       op(1, "iput-object", {2, 1}, {}, Value), op(2, "return-object", {0})});
  Exchange.access = {"public"};
  Exchange.generic_signature = "(TT;)TT;";
  auto Shadow = method("shadow", {"Ljava/lang/Number;"}, "Ljava/lang/Number;",
                       2, {op(0, "return-object", {1})});
  Shadow.access = {"public"};
  Shadow.generic_signature =
      "<T:Ljava/lang/Number;:Ljava/lang/Comparable<TT;>;>(TT;)TT;";
  Class C = klass("Lfixture/Core;", {Exchange, Shadow});
  C.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  Field F{Value, {"public"}, {}};
  F.generic_signature = "TT;";
  C.fields.push_back(F);
  const auto Text = source(recover({C}));
  EXPECT_NE(Text.find("class Core<T extends java.lang.Object>"),
            std::string::npos);
  EXPECT_NE(Text.find("public T value;"), std::string::npos);
  EXPECT_NE(Text.find("public T exchange(T arg0)"), std::string::npos);
  EXPECT_NE(Text.find(".value = ((java.lang.Object) o2);"), std::string::npos);
  EXPECT_NE(Text.find("<T extends java.lang.Number & "
                      "java.lang.Comparable<T>> T shadow(T arg0)"),
            std::string::npos);
  EXPECT_NE(Text.find("return ((T) ((java.lang.Number) o1));"),
            std::string::npos);
}

TEST(MobileDalvikJava, GenericWildcardsAndInterfaceFirstBoundsArePreserved) {
  auto Lower = method("lower", {"Ljava/util/List;"}, "Ljava/util/List;", 1,
                      {op(0, "return-object", {0})});
  Lower.generic_signature = "<T:Ljava/lang/Object;>(Ljava/util/List<-TT;>;)"
                            "Ljava/util/List<-TT;>;";
  auto Bound =
      method("bounded", {"Ljava/lang/CharSequence;"},
             "Ljava/lang/CharSequence;", 1, {op(0, "return-object", {0})});
  Bound.generic_signature = "<T::Ljava/lang/CharSequence;>(TT;)TT;";
  Class C = klass("Lfixture/Core;", {Lower, Bound});
  Field F{{C.name, "values", "Ljava/util/List;"}, {"public"}, {}};
  F.generic_signature = "Ljava/util/List<*>;";
  C.fields.push_back(F);
  const auto Text = source(recover({C}));
  EXPECT_NE(Text.find("java.util.List<?> values;"), std::string::npos);
  EXPECT_NE(Text.find("java.util.List<? super T> lower("), std::string::npos);
  EXPECT_NE(Text.find("<T extends java.lang.CharSequence> T bounded("),
            std::string::npos);
}

TEST(MobileDalvikJava, ExternalFormalBoundsCannotChangeDeclarationKind) {
  auto M = method("identity", {"Ljava/lang/CharSequence;"},
                  "Ljava/lang/CharSequence;", 1, {op(0, "return-object", {0})});
  M.generic_signature = "<T:Ljava/lang/CharSequence;>(TT;)TT;";
  rejected([&] { recoverMethods({M}); },
           "class-bound position names a known interface");
  M.reference.parameters = {"Ljava/lang/Object;"};
  M.reference.returns = "Ljava/lang/Object;";
  M.generic_signature = "<T::Ljava/lang/Object;>(TT;)TT;";
  rejected([&] { recoverMethods({M}); }, "interface bound names a known class");
}

TEST(MobileDalvikJava, GenericTypeVariablesCannotObscureRequiredPackages) {
  auto M = method("identity", {"Ljava/lang/Object;"}, "Ljava/lang/Object;", 1,
                  {op(0, "return-object", {0})});
  M.generic_signature = "<java:Ljava/lang/Object;>(Tjava;)Tjava;";
  rejected([&] { recoverMethods({M}); }, "shadowed");
  M = method("identity", {"Lfixture/Core;"}, "Lfixture/Core;", 1,
             {op(0, "return-object", {0})});
  M.generic_signature =
      "<fixture:Ljava/lang/Object;>(Lfixture/Core;)Lfixture/Core;";
  // The enclosing class's proven simple name is an available fallback.
  auto Text = source(recoverMethods({M}));
  EXPECT_NE(Text.find("Core identity(Core arg0)"), std::string::npos);
  EXPECT_EQ(Text.find("fixture.Core"), std::string::npos);
}

TEST(MobileDalvikJava, GenericConstructorsKeepExactTypeAndThrowsDeclarations) {
  auto M = method("<init>", {"Ljava/lang/Exception;"}, "V", 2,
                  {op(0, "invoke-direct", {0}, {},
                      MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                   op(1, "return-void")});
  M.access = {"public", "constructor"};
  M.generic_signature = "<E:Ljava/lang/Exception;>(TE;)V^TE;";
  M.declared_throws = std::vector<std::string>{"Ljava/lang/Exception;"};
  auto Text = source(recoverMethods({M}));
  EXPECT_NE(Text.find("public <E extends java.lang.Exception> "
                      "Core(E arg0) throws E"),
            std::string::npos);
  EXPECT_NE(Text.find("super();"), std::string::npos);
  EXPECT_EQ(Text.find("throws java.lang.Throwable {"), std::string::npos);
}

TEST(MobileDalvikJava, ConstructorPrefixesKeepErasedOverloadArgumentTypes) {
  auto Constructor = [](std::string Parameter) {
    auto M = method("<init>", {std::move(Parameter)}, "V", 2,
                    {op(0, "invoke-direct", {0}, {},
                        MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                     op(1, "return-void")});
    M.access = {"public", "constructor"};
    return M;
  };
  for (bool Super : {false, true}) {
    Class Base =
        klass("Lfixture/Base;", {Constructor("Ljava/lang/Object;"),
                                 Constructor("Ljava/lang/CharSequence;")});
    const std::string Owner = Super ? Base.name : "Lfixture/Core;";
    auto M =
        method("<init>", {"Ljava/lang/Object;", "I"}, "V", 3,
               {op(0, "invoke-direct", {0, 1}, {},
                   MethodRef{Owner, "<init>", {"Ljava/lang/Object;"}, "V"}),
                op(1, "return-void")});
    M.access = {"public", "constructor"};
    M.generic_signature =
        "<T:Ljava/lang/Object;:Ljava/lang/CharSequence;>(TT;I)V";
    Class C = klass("Lfixture/Core;", {M});
    llvm::json::Object Report;
    if (Super) {
      C.superclass = Base.name;
      Report = recover({Base, C});
    } else {
      C.methods.push_back(Constructor("Ljava/lang/Object;"));
      C.methods.push_back(Constructor("Ljava/lang/CharSequence;"));
      Report = recover({C});
    }
    auto Text = source(Report, Super ? 1 : 0);
    EXPECT_NE(Text.find("<T extends java.lang.Object & "
                        "java.lang.CharSequence> Core(T arg0, int arg1)"),
              std::string::npos);
    EXPECT_NE(Text.find(std::string(Super ? "super" : "this") +
                        "(((java.lang.Object) arg0));"),
              std::string::npos);
    EXPECT_EQ(Report.getInteger("recovered_method_count"), 3);
  }
}

TEST(MobileDalvikJava, ConstructorPrefixesRejectClassVariableSubstitution) {
  for (bool Array : {false, true}) {
    std::string Parameter =
        Array ? "[Ljava/lang/Object;" : "Ljava/lang/Object;";
    auto Target =
        method("<init>", {Parameter}, "V", 2,
               {op(0, "invoke-direct", {0}, {},
                   MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                op(1, "return-void")});
    Target.access = {"public", "constructor"};
    Target.generic_signature = Array ? "([TT;)V" : "(TT;)V";
    auto Caller = method("<init>", {Parameter, "I"}, "V", 3,
                         {op(0, "invoke-direct", {0, 1}, {}, Target.reference),
                          op(1, "return-void")});
    Caller.access = {"public", "constructor"};
    Caller.generic_signature = Array ? "([TT;I)V" : "(TT;I)V";
    Class C = klass("Lfixture/Core;", {Target, Caller});
    C.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
    rejected([&] { recover({C}); }, "constructor overload");
  }
}

TEST(MobileDalvikJava, ConstructorPrefixesRejectUnprovedGenericSiblings) {
  auto Constructor = [](std::string Parameter) {
    auto M = method("<init>", {std::move(Parameter)}, "V", 2,
                    {op(0, "invoke-direct", {0}, {},
                        MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                     op(1, "return-void")});
    M.access = {"public", "constructor"};
    return M;
  };
  for (bool OwnFormal : {false, true}) {
    auto Target = Constructor("Ljava/lang/Object;");
    auto Sibling = Constructor(OwnFormal ? "Ljava/lang/CharSequence;"
                                         : "Ljava/util/List;");
    Sibling.generic_signature =
        OwnFormal ? "<U:Ljava/lang/Object;>(Ljava/lang/CharSequence;)V"
                  : "(Ljava/util/List<Ljava/lang/String;>;)V";
    auto Caller = method("<init>", {"Ljava/lang/Object;", "I"}, "V", 3,
                         {op(0, "invoke-direct", {0, 1}, {}, Target.reference),
                          op(1, "return-void")});
    Caller.access = {"public", "constructor"};
    rejected([&] { recoverMethods({Target, Sibling, Caller}); },
             "constructor overload");
  }
}

TEST(MobileDalvikJava, ConcreteThrowsAreKeptWhenSignatureOmitsThem) {
  auto M = method("value", {"I"}, "I", 1, {op(0, "return", {0})});
  M.generic_signature = "<T:Ljava/lang/Object;>(I)I";
  M.declared_throws = std::vector<std::string>{"Ljava/lang/Exception;"};
  const auto Text = source(recoverMethods({M}));
  EXPECT_NE(Text.find("int value(int arg0) throws java.lang.Exception"),
            std::string::npos);
}

TEST(MobileDalvikJava, GenericInheritanceRequiresMemberSubstitutionProof) {
  Class Base = klass("Lfixture/Base;");
  Base.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  Class Child = klass("Lfixture/Child;");
  Child.superclass = Base.name;
  Child.generic_signature = "Lfixture/Base<Ljava/lang/String;>;";
  rejected([&] { recover({Base, Child}); }, "generic inheritance");
}

TEST(MobileDalvikJava, GenericBridgesAreNeverDroppedToAvoidSourceClashes) {
  auto M = method("bridge", {"Ljava/lang/Object;"}, "Ljava/lang/Object;", 1,
                  {op(0, "return-object", {0})});
  M.access.insert("bridge");
  Class C = klass("Lfixture/Core;", {M});
  C.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  rejected([&] { recover({C}); }, "generic bridge");
}

TEST(MobileDalvikJava, GenericCallsRequireErasedMemberBindingProof) {
  auto M = method("identity", {"Ljava/lang/Object;"}, "Ljava/lang/Object;", 1,
                  {op(0, "return-object", {0})});
  M.generic_signature = "<T:Ljava/lang/Object;>(TT;)TT;";
  auto Caller =
      method("call", {"Ljava/lang/Object;"}, "Ljava/lang/Object;", 1,
             {op(0, "invoke-static", {0}, {}, M.reference),
              op(1, "move-result-object", {0}), op(2, "return-object", {0})});
  rejected([&] { recoverMethods({M, Caller}); }, "generic invocation");
}

TEST(MobileDalvikJava, GenericThrowableSubclassesCannotBecomeInvalidJava) {
  Class C = klass("Lfixture/Core;");
  C.superclass = "Ljava/lang/Exception;";
  C.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Exception;";
  rejected([&] { recover({C}); }, "generic class cannot extend Throwable");
  Class Base = klass("Lfixture/Base;");
  Base.superclass = "Ljava/lang/Exception;";
  C.superclass = Base.name;
  C.generic_signature = "<T:Ljava/lang/Object;>Lfixture/Base;";
  // Use a name which sorts before Base so this declaration's guard is tested
  // before Base's missing external member namespace can reject emission.
  C.name = "Lfixture/ABad;";
  rejected([&] { recover({C, Base}); },
           "generic class cannot extend Throwable");
}

TEST(MobileDalvikJava, DeprecatedMarkersPreserveTheirExactDeclarations) {
  auto Init = method("<init>", {}, "V", 1,
                     {op(0, "invoke-direct", {0}, {},
                         MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                      op(1, "return-void")});
  Init.access = {"public", "constructor"};
  Init.deprecated = true;
  auto Old = method("oldValue", {"I"}, "I", 1, {op(0, "return", {0})});
  Old.deprecated = true;
  auto Plain = method("plainValue", {"I"}, "I", 1, {op(0, "return", {0})});
  Class C = klass("Lfixture/Core;", {Init, Old, Plain});
  C.deprecated = true;
  C.fields.push_back({{C.name, "legacy", "I"}, {"public"}, {}});
  C.fields.back().deprecated = true;
  C.fields.push_back({{C.name, "current", "I"}, {"public"}, {}});

  const auto Report = recover({C});
  const auto Java = source(Report);
  EXPECT_NE(Java.find("@java.lang.Deprecated\npublic class Core"),
            std::string::npos);
  EXPECT_NE(Java.find("  @java.lang.Deprecated\n  public int legacy;"),
            std::string::npos);
  EXPECT_NE(Java.find("  @java.lang.Deprecated\n  public Core()"),
            std::string::npos);
  EXPECT_NE(Java.find("  @java.lang.Deprecated\n  public static int oldValue("),
            std::string::npos);
  EXPECT_NE(Java.find("  public int current;"), std::string::npos);
  EXPECT_NE(Java.find("  public static int plainValue("), std::string::npos);
  size_t Markers = 0;
  for (size_t Pos = 0;
       (Pos = Java.find("@java.lang.Deprecated", Pos)) != std::string::npos;
       ++Pos)
    ++Markers;
  EXPECT_EQ(Markers, 4U);
  EXPECT_EQ(Report.getInteger("method_count"), 3);
  EXPECT_EQ(Report.getInteger("recovered_method_count"), 3);
  const auto *Methods = Report.getArray("methods");
  ASSERT_NE(Methods, nullptr);
  ASSERT_EQ(Methods->size(), 3U);
  for (const auto &Value : *Methods) {
    const auto *Row = Value.getAsObject();
    ASSERT_NE(Row, nullptr);
    EXPECT_EQ(Row->getBoolean("deprecated"),
              Row->getString("name") != "plainValue");
  }
  const auto *Helpers = Report.getArray("generated_source_helpers");
  ASSERT_NE(Helpers, nullptr);
  ASSERT_EQ(Helpers->size(), 1U);
  ASSERT_NE((*Helpers)[0].getAsObject(), nullptr);
  EXPECT_EQ((*Helpers)[0].getAsObject()->getString("kind"), "throw-helper");
}

TEST(MobileDalvikJava,
     DeprecatedDeclarationsDoNotInventNativeOrAbstractBodies) {
  auto Native = method("nativeValue", {}, "I", 0, {});
  Native.access = {"public", "native"};
  Native.deprecated = true;
  auto Abstract = method("abstractValue", {}, "I", 0, {});
  Abstract.access = {"public", "abstract"};
  Abstract.deprecated = true;
  Class C = klass("Lfixture/Core;", {Native, Abstract});
  C.access.insert("abstract");
  const auto Report = recover({C});
  const auto Java = source(Report);
  EXPECT_NE(
      Java.find("  @java.lang.Deprecated\n  public native int nativeValue();"),
      std::string::npos);
  EXPECT_NE(
      Java.find(
          "  @java.lang.Deprecated\n  public abstract int abstractValue();"),
      std::string::npos);
  EXPECT_EQ(Report.getInteger("method_count"), 2);
  EXPECT_EQ(Report.getInteger("recovered_method_count"), 0);
  EXPECT_EQ(Report.getInteger("declaration_only_method_count"), 2);
}

TEST(MobileDalvikJava, DeprecatedMethodModifiersExcludeOwnTypeParameters) {
  auto M = method("observe", {}, "V", 0, {});
  M.access = {"public", "native"};
  M.generic_signature = "<java:Lfixture/Core;>()V";
  EXPECT_EQ(recoverMethods({M}).getInteger("declaration_only_method_count"), 1);
  M.deprecated = true;
  const auto Report = recoverMethods({M});
  EXPECT_EQ(Report.getInteger("declaration_only_method_count"), 1);
  EXPECT_NE(source(Report).find("  @java.lang.Deprecated\n  public native "
                                "<java extends fixture.Core> void observe();"),
            std::string::npos);
}

TEST(MobileDalvikJava, DeprecatedClassModifiersExcludeOnlyOwnTypeParameters) {
  Class Core = klass("Lfixture/Core;");
  Class Marker = klass("Lfixture/Marker;");
  Marker.access = {"public", "interface", "abstract"};
  Marker.generic_signature = "<java:Lfixture/Core;>Ljava/lang/Object;";
  Marker.deprecated = true;
  const auto Report = recover({Core, Marker});
  EXPECT_NE(
      source(Report, 1).find("@java.lang.Deprecated\npublic abstract "
                             "interface Marker<java extends fixture.Core>"),
      std::string::npos);

  auto M = method("observe", {}, "V", 0, {});
  M.access = {"public", "abstract"};
  M.reference.owner = Marker.name;
  M.deprecated = true;
  Marker.methods = {M};
  // The interface type parameter is in scope at a member's modifiers.
  rejected([&] { recover({Core, Marker}); }, "ambiguous Java type");
}

TEST(MobileDalvikJava,
     DeprecatedClassInitializersAreRejectedByBothEntryPoints) {
  auto M = method("<clinit>", {}, "V", 0, {op(0, "return-void")});
  M.access = {"static", "constructor"};
  M.deprecated = true;
  Class C = klass("Lfixture/Core;", {M});
  rejected([&] { recover({C}); },
           "Deprecated cannot annotate a class initializer");
  rejected(
      [&] {
        Budget B(Limits{});
        linkClasses({C}, B);
      },
      "Deprecated cannot annotate a class initializer");
}

TEST(MobileDalvikJava, DeprecatedRequiresThePlatformAnnotationDefinition) {
  Class Replacement = klass("Ljava/lang/Deprecated;");
  Class C = klass("Lfixture/Core;");
  C.deprecated = true;
  rejected([&] { recover({C, Replacement}); },
           "platform annotation definition");
  rejected(
      [&] {
        Budget B(Limits{});
        linkClasses({C, Replacement}, B);
      },
      "platform annotation definition");
}

TEST(MobileDalvikJava, SuppressLintPreservesSitesValuesAndDeclarationStatus) {
  auto M = method("observe", {"J", "I"}, "V", 0, {});
  M.access = {"public", "native"};
  M.suppress_lint = std::vector<std::string>{"PrivateApi", "", "PrivateApi"};
  auto C = klass("Lfixture/Core;", {M});
  C.suppress_lint = std::vector<std::string>{};
  Field F{{C.name, "value", "I"}, {"public"}, {}};
  F.suppress_lint =
      std::vector<std::string>{std::string("a\n\"\0", 4) + "\xed\xa0\x80"};
  C.fields.push_back(F);
  const auto Report = recover({C});
  const auto Java = source(Report);
  EXPECT_NE(
      Java.find("@android.annotation.SuppressLint({})\npublic class Core"),
      std::string::npos)
      << Java;
  EXPECT_NE(
      Java.find(
          "@android.annotation.SuppressLint({\"a\\n\\\"\\u0000\\ud800\"})"),
      std::string::npos)
      << Java;
  EXPECT_NE(Java.find("@android.annotation.SuppressLint({\"PrivateApi\", \"\", "
                      "\"PrivateApi\"})"),
            std::string::npos)
      << Java;
  EXPECT_NE(Java.find("observe(long arg0, int arg1);"), std::string::npos)
      << Java;
  EXPECT_EQ(Report.getInteger("recovered_method_count"), 0);
  EXPECT_EQ(Report.getInteger("declaration_only_method_count"), 1);
}

TEST(MobileDalvikJava, SuppressLintModifiersRespectTypeParameterScopes) {
  auto M = method("observe", {"I"}, "V", 0, {});
  M.access = {"public", "native"};
  M.generic_signature = "<android:Lfixture/Core;>(I)V";
  M.suppress_lint = std::vector<std::string>{"PrivateApi"};
  auto C = klass("Lfixture/Core;", {M});
  EXPECT_NE(source(recover({C}))
                .find("@android.annotation.SuppressLint({\"PrivateApi\"})"),
            std::string::npos);
  C.generic_signature = "<android:Ljava/lang/Object;>Ljava/lang/Object;";
  rejected([&] { recover({C}); }, "ambiguous Java type");
}

TEST(MobileDalvikJava, SuppressLintRejectsInvalidOwnersAndPlatformReplacement) {
  auto rejectsBoth = [](const std::vector<Class> &Classes, const char *Reason) {
    rejected([&] { recover(Classes); }, Reason);
    rejected(
        [&] {
          Budget B;
          linkClasses(Classes, B);
        },
        Reason);
  };
  auto M = method("<clinit>", {}, "V", 0, {op(0, "return-void")});
  M.access = {"static", "constructor"};
  M.suppress_lint = std::vector<std::string>{};
  rejectsBoth({klass("Lfixture/Core;", {M})}, "class initializer");
  M = method("observe", {"I"}, "V", 0, {});
  M.access = {"public", "native"};
  M.suppress_lint = std::vector<std::string>{};
  rejectsBoth({klass("Lfixture/Core;", {M}),
               klass("Landroid/annotation/SuppressLint;")},
              "platform annotation definition");
}

Class markerClass(std::string Name = "Lfixture/ZMarker;") {
  Class C = klass(std::move(Name));
  C.access = {"public", "interface", "abstract", "annotation"};
  C.interfaces = {"Ljava/lang/annotation/Annotation;"};
  return C;
}
Class memberClass(Class C, const Class &Owner, std::string Name,
                  std::string Visibility = "public") {
  C.enclosing = Owner.name;
  C.inner_class_present = true;
  C.inner_name = std::move(Name);
  C.inner_access = C.access;
  C.inner_access.erase("public");
  if (!Visibility.empty())
    C.inner_access.insert(std::move(Visibility));
  C.inner_access.insert("static");
  return C;
}
std::string sourceAt(const llvm::json::Object &Report, llvm::StringRef Path) {
  const auto *Units = Report.getArray("source_units");
  if (!Units)
    return {};
  for (const auto &Value : *Units) {
    const auto *Unit = Value.getAsObject();
    if (Unit && Unit->getString("path") == Path) {
      auto Text = Unit->getString("source");
      return Text ? Text->str() : "";
    }
  }
  return {};
}
void rejectAnnotationBoth(const std::vector<Class> &Input) {
  EXPECT_THROW(recover(Input), Error);
  Budget B;
  EXPECT_THROW(linkClasses(Input, B), Error);
}

TEST(MobileDalvikJava, MarkerDeclarationHasMetadataAndNoInventedMembers) {
  Class C = markerClass();
  C.annotation_metadata.retention = "RUNTIME";
  C.annotation_metadata.targets =
      std::vector<std::string>{"TYPE_USE", "FIELD", "TYPE"};
  C.annotation_metadata.documented = true;
  C.annotation_metadata.inherited = true;
  C.deprecated = true;
  const auto Report = recover({C});
  const auto Java = source(Report);
  EXPECT_EQ(Report.getString("status"), "recovered");
  EXPECT_EQ(Report.getInteger("class_count"), 1);
  EXPECT_EQ(Report.getInteger("method_count"), 0);
  EXPECT_EQ(Report.getInteger("recovered_method_count"), 0);
  EXPECT_EQ(Report.getInteger("declaration_only_method_count"), 0);
  ASSERT_NE(Report.getArray("methods"), nullptr);
  EXPECT_TRUE(Report.getArray("methods")->empty());
  ASSERT_NE(Report.getArray("generated_source_helpers"), nullptr);
  EXPECT_TRUE(Report.getArray("generated_source_helpers")->empty());
  EXPECT_NE(Java.find("@java.lang.annotation.Retention("
                      "java.lang.annotation.RetentionPolicy.RUNTIME)"),
            std::string::npos);
  EXPECT_NE(Java.find("@java.lang.annotation.Target({"
                      "java.lang.annotation.ElementType.TYPE_USE, "
                      "java.lang.annotation.ElementType.FIELD, "
                      "java.lang.annotation.ElementType.TYPE})"),
            std::string::npos);
  EXPECT_NE(Java.find("@java.lang.annotation.Documented\n"), std::string::npos);
  EXPECT_NE(Java.find("@java.lang.annotation.Inherited\n"), std::string::npos);
  EXPECT_NE(Java.find("@java.lang.Deprecated\npublic @interface ZMarker {"),
            std::string::npos);
  EXPECT_EQ(Java.find("extends "), std::string::npos);
  EXPECT_EQ(Java.find("implements "), std::string::npos);
  EXPECT_EQ(Java.find("ZMarker()"), std::string::npos);
  EXPECT_EQ(Java.find("__neverd"), std::string::npos);
}

TEST(MobileDalvikJava, MarkerMetadataAbsenceAndEmptyArraySurviveEmission) {
  for (const auto &Policy : std::vector<std::optional<std::string>>{
           std::nullopt, "SOURCE", "CLASS", "RUNTIME"}) {
    for (bool ExplicitEmptyTarget : {false, true}) {
      SCOPED_TRACE(Policy.value_or("absent") +
                   (ExplicitEmptyTarget ? " empty target" : " absent target"));
      Class C = markerClass();
      C.annotation_metadata.retention = Policy;
      if (ExplicitEmptyTarget)
        C.annotation_metadata.targets = std::vector<std::string>{};
      ClassMap Classes{{C.name, C}};
      Budget B;
      const auto First = recoverJava(Classes, B);
      const auto Second = recoverJava(Classes, B);
      EXPECT_EQ(source(First), source(Second));
      const auto Java = source(First);
      EXPECT_EQ(Java.find("@java.lang.annotation.Retention(") !=
                    std::string::npos,
                Policy.has_value());
      if (Policy)
        EXPECT_NE(Java.find("RetentionPolicy." + *Policy + ")"),
                  std::string::npos);
      EXPECT_EQ(Java.find("@java.lang.annotation.Target({})") !=
                    std::string::npos,
                ExplicitEmptyTarget);
      EXPECT_EQ(Classes.at(C.name).annotation_metadata.retention, Policy);
      EXPECT_EQ(Classes.at(C.name).annotation_metadata.targets.has_value(),
                ExplicitEmptyTarget);
    }
  }
}

TEST(MobileDalvikJava, MarkerClassApplicationsRespectRetentionAndVisibility) {
  for (const auto &Policy : std::vector<std::optional<std::string>>{
           std::nullopt, "SOURCE", "CLASS", "RUNTIME"}) {
    for (unsigned Visibility : {0u, 1u, 2u, 3u}) {
      SCOPED_TRACE(Policy.value_or("absent") + " visibility " +
                   std::to_string(Visibility));
      Class Marker = markerClass();
      Marker.annotation_metadata.retention = Policy;
      Class Use = klass("Lfixture/AUse;");
      Use.marker_annotations.push_back({Marker.name, Visibility});
      const bool Accepted =
          Policy != "SOURCE" && Visibility == (Policy == "RUNTIME" ? 1u : 0u);
      if (!Accepted) {
        rejectAnnotationBoth({Use, Marker});
        continue;
      }
      Budget B;
      auto Linked = linkClasses({Use, Marker}, B);
      const auto Report = recoverJava(Linked, B);
      const auto Java = sourceAt(Report, "fixture/AUse.java");
      EXPECT_NE(Java.find("@fixture.ZMarker\npublic class AUse {"),
                std::string::npos);
      EXPECT_EQ(Report.getInteger("method_count"), 0);
      EXPECT_EQ(Report.getInteger("class_count"), 2);
      EXPECT_EQ(Linked.at(Use.name).marker_annotations[0].visibility,
                Visibility);
    }
  }
}

TEST(MobileDalvikJava, MarkerTargetsDistinguishClassAndAnnotationRoles) {
  const std::vector<std::optional<std::vector<std::string>>> Targets{
      std::nullopt,
      std::vector<std::string>{},
      std::vector<std::string>{"TYPE"},
      std::vector<std::string>{"ANNOTATION_TYPE"},
      std::vector<std::string>{"TYPE_USE"},
      std::vector<std::string>{"FIELD"}};
  for (const auto &Target : Targets) {
    for (unsigned Role = 0; Role != 3; ++Role) {
      SCOPED_TRACE(
          (Target ? (Target->empty() ? "empty" : Target->front()) : "absent") +
          " role " + std::to_string(Role));
      Class Marker = markerClass();
      Marker.annotation_metadata.targets = Target;
      Class Use =
          Role == 2 ? markerClass("Lfixture/AUse;") : klass("Lfixture/AUse;");
      if (Role == 1)
        Use.access = {"public", "interface", "abstract"};
      Use.marker_annotations.push_back({Marker.name, 0});
      const bool Accepted =
          !Target ||
          (!Target->empty() &&
           (Target->front() == "TYPE" || Target->front() == "TYPE_USE" ||
            (Role == 2 && Target->front() == "ANNOTATION_TYPE")));
      if (!Accepted) {
        rejectAnnotationBoth({Use, Marker});
        continue;
      }
      Budget B;
      auto Linked = linkClasses({Use, Marker}, B);
      const auto Report = recoverJava(Linked, B);
      EXPECT_NE(sourceAt(Report, "fixture/AUse.java").find("@fixture.ZMarker"),
                std::string::npos);
      EXPECT_EQ(Report.getInteger("method_count"), 0);
    }
  }
}

TEST(MobileDalvikJava, MarkerShapeIsValidatedByDirectAndLinkedEntryPoints) {
  const std::vector<std::function<void(Class &)>> Mutations{
      [](Class &C) { C.access.erase("annotation"); },
      [](Class &C) { C.access.erase("interface"); },
      [](Class &C) { C.access.erase("abstract"); },
      [](Class &C) { C.access.insert("synthetic"); },
      [](Class &C) { C.access.insert("final"); },
      [](Class &C) { C.interfaces.clear(); },
      [](Class &C) { C.interfaces.push_back("Ljava/io/Serializable;"); },
      [](Class &C) { C.superclass = "Ljava/lang/Number;"; },
      [](Class &C) {
        C.fields.push_back(
            {{C.name, "FLAG", "I"}, {"public", "static", "final"}, int64_t(1)});
      },
      [](Class &C) {
        auto M = method("value", {}, "I", 0, {});
        M.access = {"public", "abstract"};
        M.reference.owner = C.name;
        C.methods.push_back(M);
      },
      [](Class &C) {
        C.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;"
                              "Ljava/lang/annotation/Annotation;";
      }};
  for (size_t I = 0; I < Mutations.size(); ++I) {
    SCOPED_TRACE("marker shape " + std::to_string(I));
    auto C = markerClass();
    C.annotation_metadata.retention = "RUNTIME";
    Mutations[I](C);
    rejectAnnotationBoth({C});
  }
  auto C = markerClass();
  auto Child = memberClass(klass("Lfixture/ZMarker$Child;"), C, "Child");
  rejectAnnotationBoth({C, Child});
  auto Enumeration = klass("Lfixture/StillUnsupportedEnum;");
  Enumeration.access.insert("enum");
  rejected([&] { recover({Enumeration}); },
           "unsupported Java declaration shape: "
           "Lfixture/StillUnsupportedEnum;");
}

TEST(MobileDalvikJava, MarkerModelRejectsForgedMetadataAndDefinitions) {
  for (const auto &Policy : {"", "runtime", "RUNTIME) injected"}) {
    auto C = markerClass();
    C.annotation_metadata.retention = Policy;
    rejectAnnotationBoth({C});
  }
  for (const auto &Targets : std::vector<std::vector<std::string>>{
           {"TYPE", "TYPE"}, {"MODULE"}, {"RECORD_COMPONENT"}, {""}}) {
    auto C = markerClass();
    C.annotation_metadata.targets = Targets;
    rejectAnnotationBoth({C});
  }
  for (unsigned Meta = 0; Meta != 4; ++Meta) {
    auto C = klass("Lfixture/Ordinary;");
    if (Meta == 0)
      C.annotation_metadata.retention = "RUNTIME";
    else if (Meta == 1)
      C.annotation_metadata.targets = std::vector<std::string>{};
    else if (Meta == 2)
      C.annotation_metadata.documented = true;
    else
      C.annotation_metadata.inherited = true;
    rejectAnnotationBoth({C});
  }
  for (const auto &Name :
       {"Annotation", "Retention", "RetentionPolicy", "Target", "ElementType",
        "Documented", "Inherited"}) {
    SCOPED_TRACE(Name);
    auto C = markerClass();
    auto Replacement =
        klass("Ljava/lang/annotation/" + std::string(Name) + ";");
    rejectAnnotationBoth({C, Replacement});
  }
  auto Marker = markerClass();
  auto Use = klass("Lfixture/AUse;");
  Use.marker_annotations = {{Marker.name, 0}, {Marker.name, 0}};
  rejectAnnotationBoth({Use, Marker});
  Use.marker_annotations = {{"Lfixture/Missing;", 0}};
  rejectAnnotationBoth({Use, Marker});
  Use.marker_annotations = {{"I", 0}};
  rejectAnnotationBoth({Use, Marker});
  Use.marker_annotations = {{Marker.name, 0}};
  Marker.access = {"public", "interface", "abstract"};
  Marker.interfaces.clear();
  rejectAnnotationBoth({Use, Marker});
  for (const auto &Reserved :
       {"Ljava/lang/Deprecated;", "Ljava/lang/annotation/Retention;",
        "Ljava/lang/annotation/Target;", "Ljava/lang/annotation/Documented;",
        "Ljava/lang/annotation/Inherited;", "Ldalvik/annotation/Signature;",
        "Ldalvik/annotation/Throws;", "Ldalvik/annotation/InnerClass;",
        "Ldalvik/annotation/EnclosingClass;",
        "Ldalvik/annotation/EnclosingMethod;",
        "Ldalvik/annotation/MemberClasses;",
        "Ldalvik/annotation/AnnotationDefault;"}) {
    SCOPED_TRACE(Reserved);
    Use.marker_annotations = {{Reserved, 1}};
    rejected([&] { recover({Use}); }, "reserved marker annotation");
    rejected(
        [&] {
          Budget B;
          (void)linkClasses({Use}, B);
        },
        "reserved marker annotation");
  }
}

TEST(MobileDalvikJava, MarkerEnumsCheckOuterValueAndTypeScopesIndependently) {
  auto Outer = klass("Lfixture/Outer;");
  auto Marker =
      memberClass(markerClass("Lfixture/Outer$Marker;"), Outer, "Marker");
  Marker.annotation_metadata.retention = "RUNTIME";
  const auto Good = recover({Outer, Marker});
  EXPECT_NE(source(Good).find("public static @interface Marker"),
            std::string::npos);
  Outer.fields.push_back({{Outer.name, "java", "I"}, {"public", "static"}, {}});
  rejected([&] { recover({Outer, Marker}); }, "enum package qualifier");
  auto Base = klass("Lfixture/Base;");
  Base.fields = {{{Base.name, "java", "I"}, {"public", "static"}, {}}};
  Outer.fields.clear();
  Outer.superclass = Base.name;
  rejected([&] { recover({Base, Outer, Marker}); }, "enum package qualifier");
  Base.fields.clear();
  EXPECT_NO_THROW(recover({Base, Outer, Marker}));
  // An interface owner emits no ordinary class helper first, so this reaches
  // the marker enum-value guard rather than failing at an earlier helper.
  Outer.superclass = "Ljava/lang/Object;";
  Outer.access = {"public", "interface", "abstract"};
  Outer.interfaces = {"Loutside/Unknown;"};
  rejected([&] { recover({Outer, Marker}); },
           "unresolved inherited annotation enum value scope");
  Outer.interfaces.clear();
  Outer.access = {"public"};
  auto Shadow = memberClass(klass("Lfixture/Outer$java;"), Outer, "java");
  rejected([&] { recover({Outer, Marker, Shadow}); },
           "ambiguous Java type: required Java runtime package is shadowed in "
           "Lfixture/Outer;");
  Outer.generic_signature = "<java:Ljava/lang/Object;>Ljava/lang/Object;";
  rejected([&] { recover({Outer, Marker}); },
           "ambiguous Java type: Ljava/lang/Object; in Lfixture/Outer; "
           "(package qualifier is shadowed and no unique source name "
           "is proven)");
}

TEST(MobileDalvikJava, MarkerOwnershipMustHaveExactNonLocalBinaryIdentity) {
  auto Outer = klass("Lfixture/Outer;");
  auto Marker =
      memberClass(markerClass("Lfixture/Outer$Marker;"), Outer, "Marker");
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE("marker ownership " + std::to_string(Mutation));
    auto Changed = Marker;
    if (Mutation == 0)
      Changed.name = "Lfixture/Unrelated;";
    else if (Mutation == 1)
      Changed.inner_name = "OtherName";
    else if (Mutation == 2)
      Changed.enclosing = "Lfixture/Missing;";
    else if (Mutation == 3)
      Changed.inner_class_present = false;
    else
      Changed.inner_access.erase("static");
    rejectAnnotationBoth({Outer, Changed});
  }
  auto Local = localClasses();
  auto NestedMarker = memberClass(markerClass("Lfixture/Core$17Worker$Marker;"),
                                  Local[1], "Marker");
  Local.push_back(NestedMarker);
  rejectAnnotationBoth(Local);
  auto DirectLocal = markerClass("Lfixture/Core$17Marker;");
  DirectLocal.inner_class_present = true;
  DirectLocal.inner_name = "Marker";
  DirectLocal.enclosing_method = Local[0].methods[0].reference;
  DirectLocal.inner_access = DirectLocal.access;
  rejectAnnotationBoth({Local[0], DirectLocal});
}

TEST(MobileDalvikJava, MarkerApplicationAccessIsProvenAcrossOwnership) {
  auto Marker = markerClass();
  Marker.access.erase("public");
  auto Use = klass("Lfixture/AUse;");
  Use.marker_annotations = {{Marker.name, 0}};
  EXPECT_NO_THROW(recover({Use, Marker}));
  auto Foreign = klass("Lother/Use;");
  Foreign.marker_annotations = Use.marker_annotations;
  rejectAnnotationBoth({Foreign, Marker});
  Marker.access.insert("public");
  EXPECT_NO_THROW(recover({Foreign, Marker}));
  auto Outer = klass("Lfixture/Outer;");
  auto Private = memberClass(markerClass("Lfixture/Outer$Private;"), Outer,
                             "Private", "private");
  auto Sibling = memberClass(klass("Lfixture/Outer$Use;"), Outer, "Use");
  Sibling.marker_annotations = {{Private.name, 0}};
  const auto Report = recover({Outer, Private, Sibling});
  EXPECT_NE(source(Report).find("private static @interface Private"),
            std::string::npos);
  EXPECT_NE(source(Report).find("@fixture.Outer.Private"), std::string::npos);
  Use.marker_annotations = {{Private.name, 0}};
  rejectAnnotationBoth({Outer, Private, Use});
  Private.inner_access.erase("private");
  Private.inner_access.insert("protected");
  rejectAnnotationBoth({Outer, Private, Use});
}

TEST(MobileDalvikJava, InheritedMarkersAreNotCopiedToSubclassDeclarations) {
  auto Marker = markerClass();
  Marker.annotation_metadata.retention = "RUNTIME";
  Marker.annotation_metadata.targets = std::vector<std::string>{"TYPE"};
  Marker.annotation_metadata.inherited = true;
  auto Base = klass("Lfixture/Base;");
  Base.marker_annotations = {{Marker.name, 1}};
  auto Child = klass("Lfixture/Child;");
  Child.superclass = Base.name;
  const auto Report = recover({Base, Child, Marker});
  EXPECT_NE(sourceAt(Report, "fixture/Base.java").find("@fixture.ZMarker"),
            std::string::npos);
  EXPECT_EQ(sourceAt(Report, "fixture/Child.java").find("@fixture.ZMarker"),
            std::string::npos);
  EXPECT_NE(sourceAt(Report, "fixture/ZMarker.java")
                .find("@java.lang.annotation.Inherited"),
            std::string::npos);
  EXPECT_EQ(Report.getInteger("method_count"), 0);
  Child.marker_annotations = {{Marker.name, 1}};
  const auto Explicit = recover({Base, Child, Marker});
  EXPECT_NE(sourceAt(Explicit, "fixture/Child.java").find("@fixture.ZMarker"),
            std::string::npos);
}
} // namespace
