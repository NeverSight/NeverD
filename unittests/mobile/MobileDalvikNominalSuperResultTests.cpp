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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Interface,
                           bool Checked, bool Cast, unsigned Depth) {
  const std::string Object = "Ljava/lang/Object;", Input = "Lfixture/Input;",
                    Output = "Lfixture/Result;",
                    Error = "Ljava/io/IOException;", T = Alpha ? "Value" : "T";
  auto Base = klass("Lfixture/Base;", SDK);
  Base.generic_signature = "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Target = method({Base.name, "visit", {Object, Error}, Output},
                       {"public"}, SDK ? 0 : 4, {});
  Target.generic_signature = "(T" + T + ";" + Error + ")" + Output;
  if (Checked)
    Target.declared_throws = std::vector<std::string>{Error};
  const FieldRef Result{"Lfixture/Trace;", "result", Output};
  if (!SDK) {
    Target.instructions = {op(0, "sget-object", {0}, Result),
                           op(1, "return-object", {0})};
    Target.code_end = 2;
  }
  auto constructor = [&](const Class &Owner) {
    return method({Owner.name, "<init>", {}, "V"}, {"public", "constructor"},
                  Owner.external ? 0 : 1,
                  Owner.external
                      ? std::vector<Instruction>{}
                      : std::vector<Instruction>{
                            op(0, "invoke-direct", {0},
                               MethodRef{*Owner.superclass, "<init>", {}, "V"}),
                            op(1, "return-void")});
  };
  Base.methods = {Target, constructor(Base)};
  std::vector<Class> Classes{Base};
  std::string Parent = Base.name;
  std::string Signature = "Lfixture/Base<" + Input + ">;";
  for (unsigned I = 0; I < Depth; ++I) {
    auto Middle = klass("Lfixture/Middle" + std::to_string(I) + ";", SDK);
    Middle.superclass = Parent;
    Middle.generic_signature = "<Step:Ljava/lang/Object;>" +
                               Parent.substr(0, Parent.size() - 1) +
                               "<TStep;>;";
    Middle.methods.push_back(constructor(Middle));
    Parent = Middle.name;
    Signature = Parent.substr(0, Parent.size() - 1) + "<" + Input + ">;";
    Classes.push_back(Middle);
  }
  auto Core = klass("Lfixture/Core;");
  Core.superclass = Parent;
  Core.generic_signature = Signature;
  auto Caller =
      method({Core.name, "submit", {Cast ? Object : Input, Error}, Output},
             {"public"}, 4, {});
  if (Checked)
    Caller.declared_throws = std::vector<std::string>{Error};
  Caller.instructions = {
      op(0, "invoke-static", {}, MethodRef{Result.owner, "hit", {}, "V"})};
  if (Cast)
    Caller.instructions.push_back(op(1, "check-cast", {2}, Input));
  unsigned PC = Caller.instructions.size();
  Caller.instructions.push_back(
      op(PC, Range ? "invoke-super/range" : "invoke-super", {1, 2, 3},
         Target.reference));
  Caller.instructions.push_back(op(PC + 1, "move-result-object", {0}));
  Caller.instructions.push_back(op(PC + 2, "return-object", {0}));
  Caller.code_end = PC + 3;
  Core.methods = {Caller, constructor(Core)};
  Classes.push_back(Core);
  auto Value = klass(Input, true), Returned = klass(Output, true);
  if (Interface) {
    Value.access = {"public", "interface", "abstract"};
    Returned.access = Value.access;
  }
  auto Trace = klass(Result.owner, true);
  Trace.fields.push_back({Result, {"public", "static"}, {}});
  Trace.methods.push_back(
      method({Trace.name, "hit", {}, "V"}, {"public", "static"}, 0, {}));
  auto Root = klass(Object, true);
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  Root.superclass.reset();
  auto Exception = klass(Error, true);
  Exception.superclass = "Ljava/lang/Exception;";
  Classes.insert(Classes.end(), {Value, Returned, Trace, Root, Exception});
  return Classes;
}
ClassMap input(std::vector<Class> Classes, unsigned Route, Budget &B) {
  if (Route == 2)
    std::reverse(Classes.begin(), Classes.end());
  if (Route)
    return linkClasses(Classes, B);
  ClassMap Result;
  for (auto &C : Classes)
    Result.emplace(C.name, std::move(C));
  return Result;
}
} // namespace

TEST(MobileDalvikNominalSuperResult,
     KeepClosedInputsFixedResultsAndOriginalLookup) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Interface : {false, true})
          for (bool Checked : {false, true})
            for (bool Cast : {false, true})
              for (unsigned Depth : {0u, 2u})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, Range, Alpha, Interface, Checked, Cast,
                                 Depth, Route}));
                  Budget B;
                  auto Classes = input(fixture(SDK, Range, Alpha, Interface,
                                               Checked, Cast, Depth),
                                       Route, B);
                  const auto Caller = Classes.at("Lfixture/Core;").methods[0];
                  const auto Target = Classes.at("Lfixture/Base;").methods[0];
                  const auto Plans = validateGenericSignatures(Classes, B);
                  const auto Key =
                      std::pair{Caller.reference, Target.reference};
                  ASSERT_TRUE(Plans.superclass_calls.contains(Key));
                  const auto &Contract = Plans.superclass_calls.at(Key);
                  EXPECT_EQ(Contract.types.at(Contract.parameters[0]).erasure,
                            "Lfixture/Input;");
                  EXPECT_EQ(Contract.types.at(*Contract.result).erasure,
                            Target.reference.returns);
                  EXPECT_EQ(Plans.superclass_call_declarations.at(Key),
                            Target.reference);
                  llvm::json::Object Report;
                  ASSERT_NO_THROW(Report = recoverJava(Classes, B));
                  EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(
                      Report.getArray("generic_signature_projections"));
                  EXPECT_EQ(Classes.at(Caller.reference.owner)
                                .methods[0]
                                .generic_signature,
                            Caller.generic_signature);
                }
}

TEST(MobileDalvikNominalSuperResult,
     RejectUnprovedInputResultFlagsAndPhysicalWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 12; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, false, false, false, true, false, 0);
        auto &Base = Classes[0], &Core = Classes[1];
        auto &Target = Base.methods[0], &Caller = Core.methods[0];
        switch (Mutation) {
        case 0:
          Core.generic_signature = "Lfixture/Base<*>;";
          break;
        case 1:
          Core.generic_signature = "Lfixture/Base<TMissing;>;";
          break;
        case 2:
          Core.generic_signature = "Lfixture/Base<Lfixture/Result;>;";
          break;
        case 3:
          Target.access.insert("static");
          break;
        case 4:
          Target.access = {"private"};
          break;
        case 5:
          Target.generic_signature = "(TT;Ljava/io/IOException;)TT;";
          break;
        case 6:
          Caller.reference.parameters[0] = "Ljava/lang/Object;";
          break;
        case 7:
          Caller.instructions[1].registers[0] = 2;
          break;
        case 8:
          Caller.instructions[1].registers.pop_back();
          break;
        case 9:
          Caller.instructions[2].opcode = "move-result-wide";
          break;
        case 10:
          std::get<MethodRef>(Caller.instructions[1].reference).returns =
              "Ljava/lang/Object;";
          break;
        case 11:
          Core.generic_signature = "Lfixture/Base;";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}
