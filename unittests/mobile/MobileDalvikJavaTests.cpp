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
std::vector<Class> localClasses(std::string Entry = "run",
                               std::string Parameter = "I",
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

TEST(MobileDalvikJava, SameLocalNameInDifferentOverloadsHasExactLexicalBinding) {
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
    Actual.emplace(Helper->getString("class")->str(),
                   Helper->getString("name")->str(),
                   Helper->getString("prototype")->str(),
                   *Helper->getBoolean("static"));
    EXPECT_EQ(Helper->getString("kind"),
              Helper->getString("name") == "<init>" ? "default-constructor"
              : Helper->getString("name") == "<clinit>" ? "field-initializer"
              : Helper->getString("name") == "__neverdConstant"
                  ? "constant-helper"
                  : "throw-helper");
  }
  const std::string ThrowType =
      "(Ljava/lang/Throwable;)Ljava/lang/RuntimeException;";
  EXPECT_TRUE(Actual.contains({Input[0].name, "<init>", "()V", false}));
  EXPECT_TRUE(Actual.contains({Input[0].name, "<clinit>", "()V", true}));
  EXPECT_TRUE(Actual.contains({Input[0].name, "__neverdThrow", ThrowType, true}));
  EXPECT_TRUE(Actual.contains({Input[0].name, "__neverdConstant", "(I)I", true}));
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
      EXPECT_EQ(Value.getAsObject()->getString("identity"), Init.reference.identity());
    }
  EXPECT_EQ(Original, 1u);
}

TEST(MobileDalvikJava,
     LocalProjectionPreservesCatchCarrierWithoutEscapingReceiver) {
  auto Input = localClasses();
  auto &Value = Input[1].methods[1];
  Value.registers = 4;
  Value.instructions = {
      op(0, "const/4", {0}, int64_t(7)), op(1, "div-int", {0, 0, 3}),
      op(2, "return", {0}), op(3, "move-exception", {1}),
      op(4, "move-object", {0, 1}), op(5, "throw", {0})};
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
              MethodRef{"Lexternal/Sink;", "take", {"Ljava/lang/Object;"}, "V"}),
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
  Run.instructions = {
      op(0, "if-eqz", {2}, {}, {}, 4),
      op(1, "new-instance", {0}, {}, Input[1].name),
      op(2, "invoke-direct", {0}, {}, Init), op(3, "goto", {}, {}, {}, 6),
      op(4, "new-instance", {0}, {}, Input[1].name),
      op(5, "invoke-direct", {0}, {}, Init),
      op(6, "invoke-virtual", {0, 2}, {}, Value),
      op(7, "move-result", {1}), op(8, "return", {1})};
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
  Run.instructions = {
      op(0, "new-instance", {0}, {}, Input[1].name),
      op(1, "invoke-direct", {0}, {},
         MethodRef{Input[1].name, "<init>", {}, "V"}),
      op(2, "div-int", {0, 2, 2}), op(3, "return", {0}),
      op(4, "move-exception", {1}),
      op(5, "invoke-virtual", {0, 2}, {},
         MethodRef{Input[1].name, "value", {"I"}, "I"}),
      op(6, "move-result", {1}), op(7, "return", {1})};
  Run.tries = {{2, 3, {{std::string("Ljava/lang/ArithmeticException;"), 4}}}};
  Run.code_end = 8;
  auto R = recover(Input);
  EXPECT_EQ(R.getInteger("projected_method_count"), 3);
  EXPECT_NE(source(R).find("((Worker) o0).value("), std::string::npos);
}

TEST(MobileDalvikJava, LocalOriginsJoinEveryThrowingSiteInOneHandler) {
  auto Input = localClasses();
  auto &Run = Input[0].methods[0];
  Run.instructions = {
      op(0, "new-instance", {0}, {}, Input[1].name),
      op(1, "invoke-direct", {0}, {},
         MethodRef{Input[1].name, "<init>", {}, "V"}),
      op(2, "div-int", {1, 2, 2}), op(3, "const/4", {0}, int64_t(0)),
      op(4, "div-int", {1, 2, 0}), op(5, "return", {1}),
      op(6, "move-exception", {1}),
      op(7, "invoke-virtual", {0, 2}, {},
         MethodRef{Input[1].name, "value", {"I"}, "I"}),
      op(8, "move-result", {1}), op(9, "return", {1})};
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
  EXPECT_NE(source(Report, 1).find("super(arg0);"), std::string::npos);
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
} // namespace
