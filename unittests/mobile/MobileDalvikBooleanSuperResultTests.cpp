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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Cast,
                           unsigned Depth, bool Symbolic, bool NarrowValues,
                           bool Interface) {
  const std::string Object = "Ljava/lang/Object;", Input = "Lfixture/Input;",
                    Value = NarrowValues ? "Lfixture/Value;" : Object,
                    K = Alpha ? "Key" : "K", V = Alpha ? "Value" : "V";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  auto constructor = [&](const Class &C) {
    return method({C.name, "<init>", {}, "V"}, {"public", "constructor"},
                  C.external ? 0 : 1,
                  C.external
                      ? std::vector<Instruction>{}
                      : std::vector<Instruction>{
                            op(0, "invoke-direct", {0},
                               MethodRef{*C.superclass, "<init>", {}, "V"}),
                            op(1, "return-void")});
  };
  const FieldRef Result{"Lfixture/Trace;", "result", "Z"};
  auto Base = klass("Lfixture/Base;", SDK);
  Base.generic_signature = "<" + K + ":Ljava/lang/Object;" + V +
                           ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Target =
      method({Base.name, "change", {Object, Object, Object}, "Z"}, {"public"},
             SDK ? 0 : 5,
             SDK ? std::vector<Instruction>{}
                 : std::vector<Instruction>{op(0, "sget-boolean", {0}, Result),
                                            op(1, "return", {0})});
  Target.generic_signature = "(T" + K + ";T" + V + ";T" + V + ";)Z";
  Base.methods = {Target, constructor(Base)};
  std::vector<Class> Classes{Base};
  std::string Parent = Base.name;
  for (unsigned I = 0; I < Depth; ++I) {
    auto Middle = klass("Lfixture/Middle" + std::to_string(I) + ";", SDK);
    Middle.superclass = Parent;
    Middle.generic_signature =
        "<Left:Ljava/lang/Object;Right:Ljava/lang/Object;>" +
        Parent.substr(0, Parent.size() - 1) + "<TLeft;TRight;>;";
    Middle.methods.push_back(constructor(Middle));
    Parent = Middle.name;
    Classes.push_back(Middle);
  }
  auto Core = klass("Lfixture/Core;");
  Core.superclass = Parent;
  Core.interfaces = {"Lfixture/Tag;"};
  Core.generic_signature = Parent.substr(0, Parent.size() - 1) + "<" + Input +
                           Value + ">;Lfixture/Tag<" + Input + ">;";
  const MethodRef Normalize{Core.name, "normalize", {Input}, Input};
  auto Caller =
      method({Core.name, "submit", {Cast ? Object : Input, Value, Value}, "Z"},
             {"public"}, 5, {});
  if (Cast)
    Caller.instructions.push_back(op(0, "check-cast", {2}, Input));
  unsigned PC = Caller.instructions.size();
  Caller.instructions.push_back(op(PC++, "invoke-direct", {1, 2}, Normalize));
  Caller.instructions.push_back(
      op(PC++, "move-result-object", {Range ? 2u : 0u}));
  auto Ref = Target.reference;
  if (Symbolic)
    Ref.owner = Parent;
  Caller.instructions.push_back(
      op(PC++, Range ? "invoke-super/range" : "invoke-super",
         {1, Range ? 2u : 0u, 3, 4}, Ref));
  Caller.instructions.push_back(op(PC++, "move-result", {0}));
  Caller.instructions.push_back(op(PC++, "return", {0}));
  Caller.code_end = PC;
  Core.methods = {
      Caller, method(Normalize, {"private"}, 2, {op(0, "return-object", {1})}),
      constructor(Core)};
  Classes.push_back(Core);
  auto Key = klass(Input, true), Val = klass("Lfixture/Value;", true);
  if (Interface) {
    Key.access = {"public", "interface", "abstract"};
    Val.access = Key.access;
  }
  auto Tag = klass("Lfixture/Tag;", SDK);
  Tag.access = {"public", "interface", "abstract"};
  Tag.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Trace = klass(Result.owner, true);
  Trace.fields.push_back({Result, {"public", "static"}, {}});
  Classes.insert(Classes.end(),
                 {Key, Val, Tag, Trace, Root, klass("Lfixture/Other;", true)});
  return Classes;
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

TEST(MobileDalvikBooleanSuperResult,
     KeepClosedInputWordsOpaqueBooleanResultAndNearestDeclaration) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Cast : {false, true})
          for (unsigned Depth : {0u, 2u})
            for (bool Symbolic : {false, true})
              for (bool NarrowValues : {false, true})
                for (bool Interface : {false, true})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, Range, Alpha, Cast, Depth, Symbolic,
                                   NarrowValues, Interface, Route}));
                    ASSERT_NO_THROW(([&] {
                      Budget B;
                      auto Classes =
                          input(fixture(SDK, Range, Alpha, Cast, Depth,
                                        Symbolic, NarrowValues, Interface),
                                Route, B);
                      const auto Caller =
                                     Classes.at("Lfixture/Core;").methods[0],
                                 Target =
                                     Classes.at("Lfixture/Base;").methods[0];
                      const auto &Call = *std::find_if(
                          Caller.instructions.begin(),
                          Caller.instructions.end(), [](const auto &I) {
                            return I.opcode.starts_with("invoke-super");
                          });
                      const auto Ref = std::get<MethodRef>(Call.reference);
                      const auto Plans = validateGenericSignatures(Classes, B);
                      const auto Key = std::pair{Caller.reference, Ref};
                      ASSERT_TRUE(Plans.superclass_calls.contains(Key));
                      EXPECT_EQ(Plans.superclass_call_declarations.at(Key),
                                Target.reference);
                      const auto &Contract = Plans.superclass_calls.at(Key);
                      EXPECT_EQ(Contract.types.at(*Contract.result).erasure,
                                "Z");
                      EXPECT_EQ(
                          Contract.types.at(Contract.parameters[0]).erasure,
                          "Lfixture/Input;");
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

TEST(MobileDalvikBooleanSuperResult,
     RejectUnclosedBindingsResultErasureRolesAndUnprovedPhysicalWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 16; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, false, false, false, 0, false, true, false);
        auto &Base = Classes[0], &Core = Classes[1];
        auto &Target = Base.methods[0], &Caller = Core.methods[0];
        auto &Call = Caller.instructions[2];
        switch (Mutation) {
        case 0:
          Core.generic_signature =
              "Lfixture/Base<*>;Lfixture/Tag<Lfixture/Input;>;";
          break;
        case 1:
          Core.generic_signature = "Lfixture/Base<TMissing;Lfixture/"
                                   "Value;>;Lfixture/Tag<Lfixture/Input;>;";
          break;
        case 2:
          Core.generic_signature = "Lfixture/Base<Lfixture/Other;Lfixture/"
                                   "Value;>;Lfixture/Tag<Lfixture/Input;>;";
          break;
        case 3:
          Target.generic_signature = "(TK;TV;TV;)I";
          break;
        case 4:
          Target.reference.returns = "I";
          std::get<MethodRef>(Call.reference).returns = "I";
          break;
        case 5:
          Target.generic_signature = "<X:Ljava/lang/Object;>(TK;TV;TV;)Z";
          break;
        case 6:
          Target.access.insert("static");
          break;
        case 7:
          Target.access.erase("public");
          Target.access.insert("private");
          break;
        case 8:
          Target.access.insert("abstract");
          break;
        case 9:
          Caller.instructions[3].opcode = "move-result-object";
          break;
        case 10:
          Call.registers.pop_back();
          break;
        case 11:
          Call.registers[0] = 2;
          break;
        case 12:
          Core.methods[1].reference.returns = "Ljava/lang/Object;";
          std::get<MethodRef>(Caller.instructions[0].reference).returns =
              "Ljava/lang/Object;";
          break;
        case 13:
          Call.registers[2] = 2;
          break;
        case 14:
          Call.registers.back() = 5;
          break;
        case 15:
          Target.generic_signature = "(TK;TV;TForeign;)Z";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}
