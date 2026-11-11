#include "MobileDalvik.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <numeric>
#include <tuple>
using namespace neverd::mobile;
using namespace neverd::mobile::dalvik;
namespace {
Instruction op(unsigned PC, std::string Kind, std::vector<unsigned> Words = {},
               Reference Ref = {}) {
  Instruction I;
  I.pc = PC;
  I.opcode = std::move(Kind);
  I.registers = std::move(Words);
  I.reference = std::move(Ref);
  return I;
}
Method method(MethodRef Ref, Access Flags, unsigned Registers,
              std::vector<Instruction> Code = {}) {
  Method M;
  M.reference = std::move(Ref);
  M.access = std::move(Flags);
  M.registers = Registers;
  M.instructions = std::move(Code);
  M.code_end = M.instructions.empty() ? 0 : M.instructions.back().pc + 1;
  return M;
}
Class klass(std::string Name, std::vector<Method> Methods = {},
            bool External = false) {
  Class C;
  C.name = std::move(Name);
  C.access = {"public", "final"};
  C.superclass = "Ljava/lang/Object;";
  C.source_id = "owned-fixture";
  C.external = External;
  C.methods = std::move(Methods);
  return C;
}
std::vector<Class> fixture(bool Range, bool Frame, bool External,
                           bool LongTail) {
  MethodRef Init{
      "Lfixture/Target;", "<init>",
      std::vector<std::string>(LongTail ? 20 : 1, "Ljava/lang/Object;"), "V"};
  MethodRef Echo{"Lfixture/Provider;",
                 "echo",
                 {"Ljava/lang/Object;"},
                 "Ljava/lang/Object;"};
  MethodRef Super{"Ljava/lang/Object;", "<init>", {}, "V"};
  auto Ctor =
      method(Init, {"public", "constructor"}, Init.parameters.size() + 1,
             {op(0, "invoke-direct", {0}, Super), op(1, "return-void")});
  if (External) {
    Ctor.instructions.clear();
    Ctor.registers = 0;
    Ctor.code_end = 0;
  }
  auto Target = klass(Init.owner, {Ctor}, External);
  auto Object =
      klass(Super.owner, {method(Super, {"public", "constructor"}, 0)}, true);
  Object.superclass.reset();
  Object.access = {"public"};
  std::vector<Instruction> Code;
  unsigned Registers;
  std::vector<std::string> Params;
  if (!LongTail) {
    Registers = 9;
    Params = {"I", "I", "Ljava/lang/Object;"};
    Code = {
        op(0, "new-instance", {0}, Target.name),
        op(1, "if-eqz", {6}),
        op(2, "move-object", {2, 0}),
        op(3, "const/4", {0}),
        op(4, "goto"),
        op(5, "if-ltz", {7}),
        op(6, "move-object", {2, 0}),
        op(7, "const/4", {0}),
        op(8, "goto"),
        op(9, "move-object", {2, 0}),
        op(10, "const/4", {0}),
        op(11, "invoke-static", {8}, Echo),
        op(12, "move-result-object", {0}),
        op(13, "move-object", {1, 0}),
        op(14, "move-object", {0, 2}),
        op(15, Range ? "invoke-direct/range" : "invoke-direct", {0, 1}, Init),
        op(16, "return-object", {2})};
    Code[1].target = 5;
    Code[4].target = 11;
    Code[5].target = 9;
    Code[8].target = 11;
    for (unsigned At : {3, 7, 10})
      Code[At].literal = int64_t(0);
  } else {
    Registers = 30;
    Params = {"I", "Ljava/lang/Object;"};
    Code = {op(0, "new-instance", {24}, Target.name),
            op(1, "if-eqz", {28}),
            op(2, "const/4", {21}),
            op(3, "goto"),
            op(4, "const/4", {21}),
            op(5, "nop"),
            op(6, "move-object", {0, 24})};
    Code[1].target = 4;
    Code[3].target = 6;
    Code[2].literal = int64_t(0);
    Code[4].literal = int64_t(1);
    for (unsigned I = 1; I <= 20; ++I)
      Code.push_back(op(Code.size(), "move-object", {I, 29}));
    std::vector<unsigned> Words(21);
    std::iota(Words.begin(), Words.end(), 0);
    Code.push_back(op(Code.size(), "invoke-direct/range", Words, Init));
    Code.push_back(op(Code.size(), "return-object", {24}));
  }
  auto Make = method({"Lfixture/Core;", "make", Params, Target.name},
                     {"public", "static"}, Registers, Code);
  if (Frame) {
    Make.registers += 3;
    for (auto &I : Make.instructions)
      for (auto &R : I.registers)
        R += 3;
  }
  return {klass("Lfixture/Core;", {Make}), Target,
          klass(Echo.owner, {method(Echo, {"public", "static"}, 0)}, true),
          Object};
}
ClassMap classes(std::vector<Class> Input, unsigned Route, Budget &B) {
  if (Route == 2)
    std::reverse(Input.begin(), Input.end());
  if (Route)
    return linkClasses(std::move(Input), B);
  ClassMap Result;
  for (auto &C : Input)
    Result.emplace(C.name, C);
  return Result;
}
std::vector<Class> reusedBooleanReceiverFixture(bool Range, bool Frame,
                                                bool External,
                                                unsigned Distance) {
  auto Input = fixture(Range, Frame, External, false);
  auto &M = Input[0].methods[0];
  auto &Code = M.instructions;
  const unsigned Offset = Frame ? 3 : 0;
  for (unsigned At : {3u, 7u, 10u})
    Code[At] = op(At, "instance-of", {Offset, Offset + 8},
                  std::string("Ljava/lang/Object;"));
  const unsigned Padding = Distance - 15;
  std::vector<Instruction> Padded;
  for (unsigned I = 0; I < Code.size(); ++I) {
    if (I == 14)
      for (unsigned N = 0; N < Padding; ++N)
        Padded.push_back(op(14 + N, "nop"));
    auto Copy = Code[I];
    if (Copy.pc >= 14)
      Copy.pc += Padding;
    if (Copy.target && *Copy.target >= 14)
      *Copy.target += Padding;
    Padded.push_back(std::move(Copy));
  }
  Code = std::move(Padded);
  M.code_end = Code.back().pc + 1;
  return Input;
}
std::vector<Class> reusedFieldReceiverFixture(const std::string &Type,
                                              bool Static, bool Frame,
                                              bool External,
                                              unsigned Distance) {
  auto Input = reusedBooleanReceiverFixture(true, Frame, External, Distance);
  auto &M = Input[0].methods[0];
  M.reference.parameters.back() = Input[2].name;
  Field Slot;
  Slot.reference = {Input[2].name, "slot", Type};
  Slot.access = {"public", "volatile"};
  if (Static)
    Slot.access.insert("static");
  Input[2].fields.push_back(Slot);
  auto Opcode = Static ? std::string("sget") : std::string("iget");
  if (Type.starts_with('L') || Type.starts_with('['))
    Opcode += "-object";
  else if (Type == "J" || Type == "D")
    Opcode += "-wide";
  else if (Type == "Z")
    Opcode += "-boolean";
  else if (Type == "B")
    Opcode += "-byte";
  else if (Type == "C")
    Opcode += "-char";
  else if (Type == "S")
    Opcode += "-short";
  const unsigned Offset = Frame ? 3 : 0;
  for (unsigned At : {3u, 7u, 10u}) {
    std::vector<unsigned> Words{Offset};
    if (!Static)
      Words.push_back(Offset + 8);
    M.instructions[At] = op(At, Opcode, Words, Slot.reference);
  }
  return Input;
}
} // namespace
TEST(MobileDalvikConditionalReceiver,
     BranchAliasesAndLongFramesKeepOneAllocation) {
  for (bool Range : {false, true})
    for (bool Frame : {false, true})
      for (bool External : {false, true})
        for (bool LongTail : {false, true})
          for (unsigned Route = 0; Route < 3; ++Route) {
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{Range, Frame, External, LongTail, Route}));
            Budget B;
            const auto Report = recoverJava(
                classes(fixture(Range, Frame, External, LongTail), Route, B),
                B);
            EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
            EXPECT_EQ(Report.getInteger("minimum_java_release"), 14);
            EXPECT_FALSE(Report.getArray("generic_signature_projections"));
          }
}
TEST(MobileDalvikConditionalReceiver,
     LostIdentityAndUninitializedObservationsFail) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = fixture(false, false, false, false);
      auto &Code = Input[0].methods[0].instructions;
      if (Mutation == 0)
        Code[6].registers[1] = 8;
      else if (Mutation == 1) {
        Code[9] = op(9, "const/4", {2});
        Code[9].literal = int64_t(0);
      } else if (Mutation == 2)
        Code[14].registers[1] = 8;
      else if (Mutation == 3)
        Code[11].registers = {2};
      else if (Mutation == 4)
        Code[1].registers = {0};
      else if (Mutation == 5)
        Code[2].opcode = "move";
      else if (Mutation == 6) {
        Code[13] = op(13, "const-wide/16", {1});
        Code[13].literal = int64_t(0);
      } else if (Mutation == 7)
        Code[2].reference = std::string("Lfixture/Target;");
      else if (Mutation == 8) {
        auto Ref = std::get<MethodRef>(Code[15].reference);
        Ref.owner = "Lmissing/Target;";
        Code[15].reference = Ref;
      } else if (Mutation == 9)
        Code[15].registers = {1, 8};
      else if (Mutation == 10)
        Code[15].registers = {0, 2};
      else if (Mutation == 11)
        Code[4].target = 1;
      else if (Mutation == 12)
        Code[8].target = 17;
      else if (Mutation == 13)
        Code[14] = op(14, "move-object", {0, 1});
      else if (Mutation == 14)
        Code[10].registers = {2};
      else if (Mutation == 15)
        Code[9].registers = {2, 1};
      else if (Mutation == 16)
        Input[1].name = "Lmissing/Target;";
      else if (Mutation == 17)
        Input[1].methods[0].access = {"private", "constructor"};
      else if (Mutation == 18)
        Code[15].registers.pop_back();
      else
        Code[13] =
            op(13, "iput-object", {2, 8},
               FieldRef{"Lfixture/Target;", "value", "Ljava/lang/Object;"});
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}

TEST(MobileDalvikConditionalReceiver,
     BooleanDestinationReuseAndLongRegionsKeepDefiniteOrigins) {
  for (unsigned Distance : {15u, 256u, 257u, 512u})
    for (bool Range : {false, true})
      for (bool Frame : {false, true})
        for (bool External : {false, true})
          for (unsigned Route = 0; Route < 3; ++Route) {
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{Distance, Range, Frame, External, Route}));
            Budget B;
            const auto Report =
                recoverJava(classes(reusedBooleanReceiverFixture(
                                        Range, Frame, External, Distance),
                                    Route, B),
                            B);
            EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
            EXPECT_EQ(Report.getInteger("minimum_java_release"), 14);
            EXPECT_EQ(Report.getInteger("projected_method_count").value_or(0),
                      0);
            EXPECT_FALSE(Report.getArray("generic_signature_projections"));
          }
}

TEST(MobileDalvikConditionalReceiver,
     BooleanSourcesMalformedWritesAndExcessiveRegionsFail) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = reusedBooleanReceiverFixture(true, false, false,
                                                Mutation == 18   ? 513
                                                : Mutation == 19 ? 768
                                                                 : 15);
      auto &Code = Input[0].methods[0].instructions;
      if (Mutation == 0)
        Code[3].registers[1] = 2;
      else if (Mutation == 1)
        Code[3].registers[1] = 0;
      else if (Mutation == 2)
        Code[3].registers[1] = 9;
      else if (Mutation == 3)
        Code[3].registers.pop_back();
      else if (Mutation == 4)
        Code[3].registers.push_back(8);
      else if (Mutation == 5)
        Code[3].reference = {};
      else if (Mutation == 6)
        Code[3].reference = std::string("I");
      else if (Mutation == 7)
        Code[3].literal = int64_t(0);
      else if (Mutation == 8)
        Code[3].target = 11;
      else if (Mutation == 9)
        Code[3].data = {0};
      else if (Mutation == 10)
        Code[3].keys = {0};
      else if (Mutation == 11)
        Code[3].targets = {11};
      else if (Mutation == 12)
        Code[3].element_width = 1;
      else if (Mutation == 13)
        Code[3].opcode = "instance-of/vendor";
      else if (Mutation == 14)
        Code[2].registers[1] = 8;
      else if (Mutation == 15)
        Code[10].registers[0] = 2;
      else if (Mutation == 16)
        Code[14].registers[1] = 1;
      else if (Mutation == 17)
        Code[11].registers = {2};
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}

TEST(MobileDalvikConditionalReceiver,
     FieldDestinationsKeepEverySurvivingAllocationAlias) {
  for (const std::string Type :
       {"Z", "B", "C", "S", "I", "J", "F", "D", "Ljava/lang/Object;", "[J",
        "[Ljava/lang/Object;"})
    for (unsigned Distance : {15u, 512u})
      for (bool Static : {false, true})
        for (bool Frame : {false, true})
          for (bool External : {false, true})
            for (unsigned Route = 0; Route < 3; ++Route) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{Type, Distance, Static, Frame, External, Route}));
              Budget B;
              const auto Report = recoverJava(
                  classes(reusedFieldReceiverFixture(Type, Static, Frame,
                                                     External, Distance),
                          Route, B),
                  B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(Report.getInteger("minimum_java_release"), 14);
              EXPECT_EQ(Report.getInteger("projected_method_count").value_or(0),
                        0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikConditionalReceiver,
     FieldSourcesBrokenPairsAndMalformedReadsFail) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 25; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      const bool Static = Mutation != 14 && Mutation != 15 && Mutation != 24;
      const std::string Type = Mutation == 18 || Mutation == 22 ? "J"
                               : Mutation == 19 ? "D"
                                                : "Ljava/lang/Object;";
      auto Input = reusedFieldReceiverFixture(Type, Static, false, false, 15);
      auto &Code = Input[0].methods[0].instructions;
      if (Mutation == 0)
        Code[3].reference = {};
      else if (Mutation == 1)
        Code[3].reference = Input[2].methods[0].reference;
      else if (Mutation == 2)
        Code[3].registers.clear();
      else if (Mutation == 3)
        Code[3].registers.push_back(8);
      else if (Mutation == 4)
        Code[3].literal = int64_t(0);
      else if (Mutation == 5)
        Code[3].target = 11;
      else if (Mutation == 6)
        Code[3].data = {0};
      else if (Mutation == 7)
        Code[3].keys = {0};
      else if (Mutation == 8)
        Code[3].targets = {11};
      else if (Mutation == 9)
        Code[3].element_width = 1;
      else if (Mutation == 10)
        Code[3].opcode = "sget-object/vendor";
      else if (Mutation == 11)
        std::get<FieldRef>(Code[3].reference).type = "I";
      else if (Mutation == 12)
        Input[2].fields[0].access.erase("static");
      else if (Mutation == 13)
        Code[2].registers[1] = 8;
      else if (Mutation == 14)
        Code[3].registers[1] = 0;
      else if (Mutation == 15)
        Code[3].registers[1] = 2;
      else if (Mutation == 16)
        Code[14].registers[1] = 1;
      else if (Mutation == 17)
        Code[11].registers = {2};
      else if (Mutation == 18)
        Code[3].registers[0] = 1;
      else if (Mutation == 19)
        Code[7].registers[0] = 1;
      else if (Mutation == 20)
        Code[3].opcode = "sput-object";
      else if (Mutation == 21)
        Code[3].opcode = "sget-wide";
      else if (Mutation == 22)
        Code[3].opcode = "sget-object";
      else if (Mutation == 23)
        std::get<FieldRef>(Code[3].reference).name = "missing";
      else
        Input[2].fields[0].access.insert("static");
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}
