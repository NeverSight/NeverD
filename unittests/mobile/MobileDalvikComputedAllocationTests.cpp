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
Class klass(std::string Name) {
  Class C;
  C.name = std::move(Name);
  C.superclass = "Ljava/lang/Object;";
  C.access = {"public"};
  C.source_id = "owned-fixture";
  return C;
}
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Interface,
                           bool Copy) {
  const std::string Object = "Ljava/lang/Object;", E = Alpha ? "Element" : "E";
  auto Base = klass("Lfixture/Base;");
  Base.generic_signature = "<B:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Read = method({Base.name, "measure", {}, "I"}, {"public"}, 2, {});
  auto Constant = op(0, "const/16", {0});
  Constant.literal = int64_t(-37);
  Read.instructions = {Constant, op(1, "return", {0})};
  Read.code_end = 2;
  if (Interface) {
    Base.access = {"public", "interface", "abstract"};
    Read.access = {"public", "abstract"};
    Read.instructions.clear();
    Read.registers = Read.code_end = 0;
  }
  Base.methods.push_back(Read);
  auto Slot = klass("Lfixture/Slot;");
  Slot.access = {"public", "interface", "abstract"};
  Slot.generic_signature = "<S:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Context = klass("Lfixture/Context;");
  Context.methods.push_back(
      method({Context.name, "<init>", {"I"}, "V"}, {"public", "constructor"}, 2,
             {op(0, "invoke-direct", {0}, MethodRef{Object, "<init>", {}, "V"}),
              op(1, "return-void")}));
  auto Target = klass("Lfixture/Target;");
  Target.generic_signature = "<" + E + ":Ljava/lang/Object;>Ljava/lang/Object;";
  MethodRef Selected{Target.name, "<init>", {Base.name, Context.name}, "V"};
  auto Sink =
      method(Selected, {"public", "constructor"}, 3,
             {op(0, "invoke-direct", {0}, MethodRef{Object, "<init>", {}, "V"}),
              op(1, "return-void")});
  Sink.generic_signature =
      "(Lfixture/Base<Lfixture/Slot<T" + E + ";>;>;Lfixture/Context;)V";
  std::vector<Instruction> Code = {op(0, "move-object", {0, 6}),
                                   op(1, "move-object", {1, 7})};
  if (Copy)
    Code.push_back(op(2, "move-object", {4, 1}));
  Code.push_back(op(3, "new-instance", {2}, Context.name));
  Code.push_back(
      op(4,
         Interface ? (Range ? "invoke-interface/range" : "invoke-interface")
                   : (Range ? "invoke-virtual/range" : "invoke-virtual"),
         {Copy ? 4u : 1u}, Read.reference));
  Code.push_back(op(5, "move-result", {3}));
  Code.push_back(op(6, Range ? "invoke-direct/range" : "invoke-direct", {2, 3},
                    Context.methods[0].reference));
  Code.push_back(op(7, Range ? "invoke-direct/range" : "invoke-direct",
                    {0, 1, 2}, Selected));
  Code.push_back(op(8, "return-void"));
  auto Caller = method({Target.name, "<init>", {Base.name}, "V"},
                       {"public", "constructor"}, 8, Code);
  Caller.generic_signature = "(Lfixture/Base<Lfixture/Slot<T" + E + ";>;>;)V";
  Target.methods = {Caller, Sink};
  auto Root = klass(Object);
  Root.external = true;
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  if (SDK)
    for (auto *C : {&Base, &Slot, &Context}) {
      C->external = true;
      for (auto &M : C->methods) {
        M.instructions.clear();
        M.registers = M.code_end = 0;
      }
    }
  return {Target, Context, Base, Slot, Root};
}
} // namespace

TEST(MobileDalvikComputedAllocation,
     RetainOriginalIntegerGettersAndScopedConstructorArguments) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Interface : {false, true})
          for (bool Copy : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Range, Alpha, Interface, Copy, Route}));
              auto Input = fixture(SDK, Range, Alpha, Interface, Copy);
              const auto Caller = Input[0].methods[0].reference,
                         Selected = Input[0].methods[1].reference;
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
              EXPECT_TRUE(Plans.constructor_calls.contains({Caller, Selected}));
              const auto R = recoverJava(Classes, B);
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikComputedAllocation,
     RejectChangedGettersResultsInitializationAndLiveWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, false, true, false);
      auto &Call = Input[0].methods[0];
      auto &Read = Input[2].methods[0];
      if (Mutation == 0)
        Input[2].methods.clear();
      else if (Mutation == 1)
        Read.access = {"private"};
      else if (Mutation == 2)
        Read.access.insert("static");
      else if (Mutation == 3) {
        Read.access = {"public", "native"};
        Read.instructions.clear();
        Read.registers = Read.code_end = 0;
      } else if (Mutation == 4) {
        Read.reference.returns = "J";
        std::get<MethodRef>(Call.instructions[3].reference).returns = "J";
        Call.instructions[4].opcode = "move-result-wide";
      } else if (Mutation == 5)
        Read.generic_signature = "<U:Ljava/lang/Object;>()I";
      else if (Mutation == 6)
        Read.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 7)
        Read.access.insert("synthetic");
      else if (Mutation == 8)
        Read.access.insert("bridge");
      else if (Mutation == 9)
        Read.access.insert("varargs");
      else if (Mutation == 10)
        Call.instructions[3].opcode = "invoke-static/range";
      else if (Mutation == 11) {
        Call.instructions[4].registers = {0};
        Call.instructions[5].registers[1] = 0;
      } else if (Mutation == 12) {
        Call.instructions[4].registers = {2};
        Call.instructions[5].registers[1] = 2;
      } else if (Mutation == 13)
        Call.instructions[3].registers = {2};
      else if (Mutation == 14)
        Call.instructions[5].registers[1] = 1;
      else if (Mutation == 15)
        Call.instructions.erase(Call.instructions.begin() + 4);
      else if (Mutation == 16) {
        Call.instructions[4].registers = {1};
        Call.instructions[5].registers[1] = 1;
      } else if (Mutation == 17)
        Input[0].methods[1].generic_signature =
            "(Lfixture/Base<Lfixture/Slot<+TE;>;>;Lfixture/Context;)V";
      else if (Mutation == 18)
        Input[0].methods[1].generic_signature =
            "(Lfixture/Base<Lfixture/Slot<TMissing;>;>;Lfixture/Context;)V";
      else
        Input[1].methods[0].access = {"private", "constructor"};
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}

TEST(MobileDalvikComputedAllocation, ConditionalArgumentsCommitInstanceStores) {
  auto owner = klass("Lfixture/Counter;");
  auto result = klass("Lfixture/Result;");
  auto object = klass("Ljava/lang/Object;");
  object.external = true;
  object.superclass.reset();
  const MethodRef objectInit{object.name, "<init>", {}, "V"};
  object.methods.push_back(method(objectInit, {"public", "constructor"}, 0, {}));
  const FieldRef count{owner.name, "count", "I"};
  owner.fields.push_back({count, {"public"}, {}});
  const MethodRef init{result.name, "<init>", {"I"}, "V"};
  result.methods.push_back(method(init, {"public", "constructor"}, 2,
                                  {op(0, "invoke-direct", {0}, objectInit),
                                   op(1, "return-void")}));
  auto increment = op(2, "add-int/lit8", {2, 1});
  increment.literal = int64_t(1);
  auto branch = op(4, "if-gez", {1});
  branch.target = 6;
  owner.methods.push_back(method(
      {owner.name, "advance", {}, result.name}, {"public"}, 4,
      {op(0, "new-instance", {0}, result.name), op(1, "iget", {1, 3}, count),
       increment, op(3, "iput", {2, 3}, count), branch,
       op(5, "move", {1, 2}), op(6, "invoke-direct", {0, 1}, init),
       op(7, "return-object", {0})}));
  for (bool external : {false, true}) {
    if (external) {
      result.external = true;
      result.methods.front().instructions.clear();
      result.methods.front().code_end = result.methods.front().registers = 0;
    }
    for (unsigned mutation = 0; mutation < 4; ++mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{external, mutation}));
      auto caller = owner;
      auto &store = caller.methods.front().instructions[3];
      if (mutation == 1)
        store.registers[1] = 0; // The field receiver is still uninitialized.
      if (mutation == 2)
        store.registers[0] = 0;
      if (mutation == 3)
        store.opcode = "iput-wide";
      Budget b;
      auto classes = linkClasses({caller, result, object}, b);
      if (mutation) {
        EXPECT_THROW(recoverJava(classes, b), std::runtime_error);
        continue;
      }
      auto report = recoverJava(classes, b);
      EXPECT_EQ(report.getInteger("unrecovered_method_count"), 0);
      EXPECT_EQ(report.getInteger("minimum_java_release"), 14);
      bool retained = false;
      for (const auto &unit : *report.getArray("source_units")) {
        const auto code = unit.getAsObject()->getString("source");
        retained |= code && code->contains("new fixture.Result(switch (0)") &&
                    code->contains(".count = v2;");
      }
      EXPECT_TRUE(retained);
    }
  }
}
