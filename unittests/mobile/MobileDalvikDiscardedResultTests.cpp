#include "MobileDalvik.h"
#include "MobileDalvikSignature.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <tuple>

using namespace neverd::mobile;
using namespace neverd::mobile::dalvik;
namespace {
Instruction instruction(unsigned PC, std::string Opcode,
                        std::vector<unsigned> Registers = {},
                        Reference Ref = {}) {
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
Class klass(std::string Name, std::vector<Method> Methods = {}) {
  Class C;
  C.name = std::move(Name);
  C.superclass = "Ljava/lang/Object;";
  C.access = {"public"};
  C.source_id = "owned-fixture";
  C.methods = std::move(Methods);
  return C;
}
std::vector<Class> fixture(const std::string &Type, bool SDK, bool Range,
                           bool Frame, bool Nested, bool Cast) {
  const std::string Core = Nested ? "Lfixture/Host$Core;" : "Lfixture/Core;";
  const std::string Port = "Lfixture/Port;";
  const bool Wide = Type == "J" || Type == "D";
  const unsigned Self = Frame ? 5 : 1, Incoming = Wide ? 3 : 2;
  const MethodRef Target{Core, "apply", {Type}, Core};
  const MethodRef Void{Core, "apply", {Type}, "V"};
  const MethodRef Bridge{Core, "apply", {Type}, Port};
  std::vector<unsigned> Words{Self, Self + 1};
  if (Wide)
    Words.push_back(Self + 2);
  const auto Invoke = Range ? "invoke-virtual/range" : "invoke-virtual";
  auto Body = method(Target, {"public"}, Self + Incoming,
                     {instruction(0, "return-object", {Self})});
  auto Adapter = method(Void, {"public", "final", "synthetic"}, Self + Incoming,
                        {instruction(0, Invoke, Words, Target),
                         instruction(1, "return-void")});
  std::vector<Instruction> Forward{instruction(0, Invoke, Words, Target),
                                    instruction(1, "move-result-object", {0})};
  if (Cast)
    Forward.push_back(instruction(2, "check-cast", {0}, Port));
  Forward.push_back(instruction(Forward.size(), "return-object", {0}));
  auto B = method(Bridge, {"public", "bridge", "synthetic"}, Self + Incoming,
                  std::move(Forward));
  auto Init = method({Core, "<init>", {}, "V"}, {"public", "constructor"}, 1,
                     {instruction(0, "invoke-direct", {0},
                                  MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                      instruction(1, "return-void")});
  auto C = klass(Core, {Init, Body, B, Adapter});
  C.access.insert("final");
  C.interfaces = {Port};
  auto P = klass(Port, {method({Port, "apply", {Type}, Port},
                              {"public", "abstract"}, 0, {})});
  P.access = {"public", "abstract", "interface"};
  P.external = SDK;
  std::vector<unsigned> Args{1, 2};
  if (Wide)
    Args.push_back(3);
  auto Caller = klass("Lfixture/Client;",
                      {method({"Lfixture/Client;", "call", {Core, Type}, "V"},
                              {"public", "static"}, Wide ? 4 : 3,
                              {instruction(0, Invoke, Args, Void),
                               instruction(1, "return-void")})});
  auto Object = klass("Ljava/lang/Object;",
                      {method({"Ljava/lang/Object;", "<init>", {}, "V"},
                              {"public", "constructor"}, 0, {})});
  Object.external = true;
  Object.superclass.reset();
  std::vector<Class> Input{C, Caller, P, Object};
  if (Nested) {
    auto Host = klass("Lfixture/Host;");
    Input[0].enclosing = Host.name;
    Input[0].inner_class_present = true;
    Input[0].inner_name = "Core";
    Input[0].inner_access = {"public", "final", "static"};
    Input.push_back(Host);
  }
  return Input;
}
ClassMap mapClasses(std::vector<Class> Input, unsigned Route, Budget &B) {
  if (Route == 2) {
    std::reverse(Input.begin(), Input.end());
    for (auto &C : Input)
      std::reverse(C.methods.begin(), C.methods.end());
  }
  if (Route)
    return linkClasses(Input, B);
  ClassMap Result;
  for (auto &C : Input)
    Result.emplace(C.name, C);
  return Result;
}
} // namespace

TEST(MobileDalvikDiscardedResult, FinalVoidAdaptersRetainOriginalReferenceCalls) {
  for (const auto *Type : {"Z", "B", "C", "S", "I", "J", "F", "D"})
    for (bool SDK : {false, true})
      for (bool Range : {false, true})
        for (bool Frame : {false, true})
          for (bool Nested : {false, true})
            for (bool Cast : {false, true})
              for (unsigned Route = 0; Route < 3; ++Route) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{Type, SDK, Range, Frame, Nested, Cast, Route}));
                auto Input = fixture(Type, SDK, Range, Frame, Nested, Cast);
                const auto Ref = Input[0].methods[3].reference;
                Budget B;
                auto Classes = mapClasses(Input, Route, B);
                const auto Report = recoverJava(Classes, B);
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                const auto *Rows = Report.getArray("methods");
                ASSERT_NE(Rows, nullptr);
                bool Found = false;
                for (const auto &Value : *Rows) {
                  const auto &Row = *Value.getAsObject();
                  if (Row.getString("identity") != Ref.identity())
                    continue;
                  Found = true;
                  EXPECT_EQ(Row.getString("projection_kind"),
                            "discarded-reference-result");
                  EXPECT_EQ(Row.getString("status"), "source-projected");
                  EXPECT_TRUE(Row.getString("source_identity"));
                  EXPECT_TRUE(Row.getArray("metadata_differences"));
                }
                EXPECT_TRUE(Found);
              }
}

TEST(MobileDalvikDiscardedResult, RejectsUnprovedDiscardAndInheritedVoidSlots) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 30; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = fixture("C", true, true, false, false, true);
      auto &Core = Input[0];
      auto &Target = Core.methods[1];
      auto &Void = Core.methods[3];
      auto &Call = Void.instructions[0];
      auto &Return = Void.instructions[1];
      if (Mutation == 0)
        Core.access.erase("final");
      else if (Mutation == 1)
        Void.access.erase("final");
      else if (Mutation == 2)
        Void.access.erase("synthetic");
      else if (Mutation == 3)
        Void.access.insert("static");
      else if (Mutation == 4)
        Void.access.insert("bridge");
      else if (Mutation == 5)
        Target.access = {"private"};
      else if (Mutation == 6)
        Target.access.insert("static");
      else if (Mutation == 7)
        Void.generic_signature = "(C)V";
      else if (Mutation == 8)
        Target.generic_signature = "(C)Lfixture/Core;";
      else if (Mutation == 9)
        Void.declared_throws = std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 10)
        Call.registers.pop_back();
      else if (Mutation == 11)
        Call.registers[0] = 0;
      else if (Mutation == 12)
        Call.registers[1] = 0;
      else if (Mutation == 13)
        Call.literal = int64_t(0);
      else if (Mutation == 14)
        Call.target = 1;
      else if (Mutation == 15)
        Call.targets = {1};
      else if (Mutation == 16)
        Call.keys = {0};
      else if (Mutation == 17)
        Call.data = {0};
      else if (Mutation == 18)
        Call.element_width = 1;
      else if (Mutation == 19)
        Return.reference = std::string("Ljava/lang/Object;");
      else if (Mutation == 20)
        Return.pc = Call.pc;
      else if (Mutation == 21)
        Return.literal = int64_t(0);
      else if (Mutation == 22)
        Return.registers = {0};
      else if (Mutation == 23)
        Core.superclass = "Lmissing/Parent;";
      else if (Mutation == 24) {
        auto Ref = Input[2].methods[0].reference;
        Ref.returns = "V";
        Input[2].methods.push_back(method(Ref, {"public", "abstract"}, 0, {}));
      } else if (Mutation == 25)
        Void.tries = {{0, 1, {{std::nullopt, 1}}}};
      else if (Mutation == 26)
        Void.instructions.insert(Void.instructions.begin() + 1,
                                  instruction(0, "move-result-object", {0}));
      else if (Mutation == 27)
        Call.opcode = "invoke-direct";
      else if (Mutation == 28)
        std::get<MethodRef>(Call.reference).owner = "Lfixture/Port;";
      else
        Void.registers = 1;
      Budget B;
      EXPECT_THROW(recoverJava(mapClasses(Input, Route, B), B), std::runtime_error);
    }
}
