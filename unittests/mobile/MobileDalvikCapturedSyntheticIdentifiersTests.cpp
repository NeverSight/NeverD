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
std::vector<Class> fixture(bool SDK, bool Alpha, bool Range, unsigned Gap) {
  const std::string Object = "Ljava/lang/Object;";
  auto Outer = klass("Lfixture/Outer;");
  const std::string K = Alpha ? "Key" : "K", V = Alpha ? "Value" : "V";
  Outer.generic_signature = "<" + K + ":Ljava/lang/Object;" + V +
                            ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Inner = klass("Lfixture/Outer$Member;");
  Inner.access.clear();
  Inner.enclosing = Outer.name;
  Inner.inner_class_present = true;
  Inner.inner_name = "Member";
  Inner.inner_access = {"private"};
  Inner.interfaces = {"Lfixture/Scope;"};
  Inner.generic_signature = "Ljava/lang/Object;Lfixture/Scope<Lfixture/Pair<T" +
                            K + ";T" + V + ";>;>;";
  FieldRef Capture{Inner.name, "enclosure", Outer.name};
  Inner.fields = {{Capture, {"final", "synthetic"}, {}}};
  MethodRef Read{Outer.name, "read", {Object}, Object};
  MethodRef Send{Inner.name, "accept-value", {Object}, Object};
  MethodRef Init{Inner.name, "<init>", {Outer.name}, "V"};
  MethodRef RootInit{Object, "<init>", {}, "V"};
  Inner.methods = {
      method(Init, {"constructor"}, 2,
             {op(0, "iput-object", {1, 0}, Capture),
              op(1, "invoke-direct", {0}, RootInit), op(4, "return-void")}),
      method(Send, {"synthetic"}, 4,
             {op(0, "iget-object", {0, 2}, Capture),
              op(2, "move-object", {1, 3}),
              op(3, "invoke-virtual", {0, 1}, Read),
              op(6, "move-result-object", {0}), op(7, "return-object", {0})})};
  Outer.methods = {
      method(Read, {"public"}, 2, {op(0, "return-object", {1})}),
      method({Outer.name, "call", {Inner.name, Object}, Object},
             {"public", "static"}, 2,
             {op(0, "invoke-virtual", {0, 1}, Send),
              op(3, "move-result-object", {0}), op(4, "return-object", {0})}),
      method({Outer.name, "__neverdVirtualMethod0", {}, "V"},
             {"public", "static"}, 0, {op(0, "return-void")})};
  auto Scope = klass("Lfixture/Scope;", SDK);
  Scope.access = {"public", "interface", "abstract"};
  Scope.generic_signature = "<S:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Pair = klass("Lfixture/Pair;", SDK);
  Pair.access = {"public", "interface", "abstract"};
  Pair.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods = {method(RootInit, {"public", "constructor"}, 0, {})};
  std::vector<Class> Result{Outer, Inner, Scope, Pair, Root};
  for (auto &C : Result)
    for (auto &M : C.methods) {
      if (M.instructions.empty())
        continue;
      M.registers += Gap;
      for (auto &I : M.instructions) {
        for (auto &R : I.registers)
          R += Gap;
        if (Range && I.opcode.starts_with("invoke-"))
          I.opcode += "/range";
      }
    }
  return Result;
}
llvm::json::Object recover(std::vector<Class> Input, unsigned Route) {
  if (Route == 2) {
    std::reverse(Input.begin(), Input.end());
    for (auto &C : Input)
      std::reverse(C.methods.begin(), C.methods.end());
  }
  Budget B;
  ClassMap Classes;
  if (Route)
    Classes = linkClasses(Input, B);
  else
    for (const auto &C : Input)
      Classes.emplace(C.name, C);
  return recoverJava(Classes, B);
}
} // namespace

TEST(MobileDalvikCapturedSyntheticIdentifiers,
     ReuseCanonicalCaptureForCompletePackageFamily) {
  for (bool SDK : {false, true})
    for (bool Alpha : {false, true})
      for (bool Range : {false, true})
        for (unsigned Gap : {0u, 8u})
          for (unsigned Route : {0u, 1u, 2u})
            for (bool Outside : {false, true}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Alpha, Range, Gap, Route}));
              auto Input = fixture(SDK, Alpha, Range, Gap);
              if (Outside) {
                auto Caller = klass("Lfixture/Caller;");
                Caller.methods = {Input[0].methods[1]};
                Caller.methods[0].reference.owner = Caller.name;
                Caller.fields = {
                    {{Caller.name, "member", Input[1].name}, {"public"}, {}}};
                Input.push_back(Caller);
              }
              const auto Ref = Input[1].methods[1].reference;
              const auto R = recover(Input, Route);
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              unsigned Names = 0;
              for (const auto &Value : *R.getArray("methods")) {
                const auto &Row = *Value.getAsObject();
                if (Row.getString("identity") != Ref.identity())
                  continue;
                ++Names;
                EXPECT_EQ(Row.getString("projection_kind"),
                          "virtual-method-identifier");
                EXPECT_EQ(Row.getString("source_name"),
                          "__neverdVirtualMethod1");
                EXPECT_EQ(Row.getString("source_name_declaration"),
                          Ref.identity());
                ASSERT_NE(Row.getArray("source_name_family"), nullptr);
                ASSERT_EQ(Row.getArray("source_name_family")->size(), 1u);
                EXPECT_EQ(
                    Row.getArray("source_name_family")->front().getAsString(),
                    Ref.identity());
              }
              EXPECT_EQ(Names, 1u);
              const auto *Bindings = R.getArray("class_source_bindings");
              ASSERT_NE(Bindings, nullptr);
              ASSERT_EQ(Bindings->size(), 1u);
              const auto &Binding = *Bindings->front().getAsObject();
              if (Outside) {
                EXPECT_EQ(Binding.getString("source_visibility"), "package");
                const auto *Callers =
                    Binding.getArray("package_access_callers");
                ASSERT_NE(Callers, nullptr);
                ASSERT_EQ(Callers->size(), 1u);
                EXPECT_EQ(Callers->front().getAsString(),
                          Input.back().methods[0].reference.identity());
              } else {
                EXPECT_FALSE(Binding.getString("source_visibility"));
                EXPECT_FALSE(Binding.getArray("package_access_callers"));
              }

              const auto *Projections =
                  R.getArray("generic_signature_projections");
              ASSERT_NE(Projections, nullptr);
              ASSERT_EQ(Projections->size(), 1u);
              EXPECT_EQ(
                  Projections->front().getAsObject()->getString("declaration"),
                  Input[1].name);
              std::string Text;
              for (const auto &Unit : *R.getArray("source_units"))
                Text += Unit.getAsObject()->getString("source")->str();
              EXPECT_EQ(Text.find("private static class Member") ==
                            std::string::npos,
                        Outside);
              EXPECT_EQ(Text.find("accept-value"), std::string::npos);
              EXPECT_NE(Text.find("__neverdVirtualMethod1("),
                        std::string::npos);
            }
}

TEST(MobileDalvikCapturedSyntheticIdentifiers,
     RejectUnprovedScopesFamiliesAndCalls) {
  for (unsigned Mutation = 0; Mutation < 20; ++Mutation)
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input = fixture(false, false, false, 0);
      auto &Outer = Input[0];
      auto &Inner = Input[1];
      auto &Send = Inner.methods[1];
      switch (Mutation) {
      case 0:
        Send.access.erase("synthetic");
        break;
      case 1:
        Send.access.insert("public");
        break;
      case 2:
        Send.access.insert("private");
        break;
      case 3:
        Send.access.insert("protected");
        break;
      case 4:
        Send.access.insert("bridge");
        break;
      case 5:
        Send.access.insert("static");
        break;
      case 6:
        Send.access.insert("native");
        Send.instructions.clear();
        break;
      case 7:
        Send.access.insert("abstract");
        Send.instructions.clear();
        break;
      case 8:
        Send.generic_signature = "(Ljava/lang/Object;)Ljava/lang/Object;";
        break;
      case 9:
        Inner.generic_signature =
            "<X:Ljava/lang/Object;>" + *Inner.generic_signature;
        break;
      case 10:
        Inner.generic_signature =
            "Ljava/lang/Object;Lfixture/Scope<TMissing;>;";
        break;
      case 11:
        Inner.fields[0].access.erase("synthetic");
        break;
      case 12:
        Inner.methods[0].instructions.erase(
            Inner.methods[0].instructions.begin());
        break;
      case 13:
        Inner.enclosing.reset();
        break;
      case 14:
        Input.erase(Input.begin() + 2);
        break;
      case 15:
        Send.instructions[0].opcode = "unsupported-owned-instruction";
        break;
      case 16: {
        auto Caller = klass("Lother/Caller;");
        Caller.methods = {Outer.methods[1]};
        Caller.methods[0].reference.owner = Caller.name;
        Outer.methods.erase(Outer.methods.begin() + 1);
        Input.push_back(Caller);
        break;
      }
      case 17: {
        auto Other = klass("Lother/Other;");
        Other.methods = {Send};
        Other.methods[0].reference.owner = Other.name;
        Input.push_back(Other);
        break;
      }
      case 18:
        Input[2].methods = {Send};
        Input[2].methods[0].reference.owner = Input[2].name;
        Input[2].external = true;
        Input[2].methods[0].instructions.clear();
        break;
      case 19:
        Inner.superclass = "Lfixture/Missing;";
        break;
      }
      EXPECT_THROW(recover(Input, Route), std::runtime_error);
    }
}

TEST(MobileDalvikCapturedSyntheticIdentifiers,
     PackageAccessUsesOriginalCallsWithoutNameChanges) {
  for (const auto &Visibility :
       {Access{}, Access{"public"}, Access{"protected"}}) {
    auto Input = fixture(false, false, false, 0);
    const auto Original = Input[1].methods[1].reference;
    auto Ordinary = Original;
    Ordinary.name = "accept";
    Input[1].methods[1].reference = Ordinary;
    Input[1].methods[1].access = Visibility;
    auto Caller = klass("Lfixture/Caller;");
    Caller.methods = {Input[0].methods[1]};
    Caller.methods[0].reference.owner = Caller.name;
    Input.push_back(Caller);
    for (auto &C : Input)
      for (auto &M : C.methods)
        for (auto &Op : M.instructions)
          if (auto *Ref = std::get_if<MethodRef>(&Op.reference);
              Ref && *Ref == Original)
            Op.reference = Ordinary;
    const auto R = recover(Input, 1);
    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
    const auto *Bindings = R.getArray("class_source_bindings");
    ASSERT_NE(Bindings, nullptr);
    ASSERT_EQ(Bindings->size(), 1u);
    EXPECT_EQ(Bindings->front().getAsObject()->getString("source_visibility"),
              "package");
    for (const auto &Value : *R.getArray("methods")) {
      const auto &Row = *Value.getAsObject();
      EXPECT_FALSE(Row.getString("source_name"));
    }
  }
}
