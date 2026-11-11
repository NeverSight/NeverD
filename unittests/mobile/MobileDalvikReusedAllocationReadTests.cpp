#include "MobileDalvik.h"
#include "MobileDalvikSignature.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <array>
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
std::vector<Class> fixture(unsigned Kind, bool TargetSDK, bool FieldsSDK,
                           bool Head, bool Jumbo) {
  const std::string Object = "Ljava/lang/Object;",
                    String = "Ljava/lang/String;";
  auto Storage = klass("Lfixture/Storage;", FieldsSDK);
  Field First;
  First.reference = {Storage.name, "first", String};
  First.access = {"public", "volatile"};
  Field Second;
  Second.reference = {Storage.name, "second", String};
  Second.access = {"public", "volatile"};
  if (Kind != 2) {
    First.access.insert("static");
    Second.access.insert("static");
  }
  Storage.fields = {First, Second};
  auto Value = klass("Lfixture/Value;", TargetSDK);
  MethodRef Init{Value.name, "<init>", {String, String, String}, "V"};
  MethodRef RootInit{Object, "<init>", {}, "V"};
  Value.methods.push_back(method(
      Init, {"public"}, TargetSDK ? 0 : 4,
      TargetSDK
          ? std::vector<Instruction>{}
          : std::vector<Instruction>{op(0, "invoke-direct", {0}, RootInit),
                                     op(3, "return-void")}));
  auto Provider = klass("Lfixture/Provider;");
  std::vector<Instruction> Code{op(0, "new-instance", {0}, Value.name)};
  if (Kind == 0) {
    auto A = op(2, Jumbo ? "const-string/jumbo" : "const-string", {1});
    A.literal = std::string("owned-first");
    auto B = op(4, "const-string", {2});
    B.literal = std::string("owned-shared");
    Code.push_back(A);
    Code.push_back(B);
  } else {
    Code.push_back(
        op(2, Kind == 2 ? "iget-object" : "sget-object",
           Kind == 2 ? std::vector<unsigned>{1, 3} : std::vector<unsigned>{1},
           First.reference));
    Code.push_back(
        op(4, Kind == 2 ? "iget-object" : "sget-object",
           Kind == 2 ? std::vector<unsigned>{2, 3} : std::vector<unsigned>{2},
           Second.reference));
  }
  Code.push_back(op(6, "invoke-direct",
                    Head ? std::vector<unsigned>{0, 1, 2, 1}
                         : std::vector<unsigned>{0, 1, 2, 2},
                    Init));
  Code.push_back(op(9, "return-object", {0}));
  Provider.methods.push_back(
      method({Provider.name, "make",
              Kind == 2 ? std::vector<std::string>{Storage.name}
                        : std::vector<std::string>{},
              Value.name},
             {"public", "static"}, Kind == 2 ? 4 : 3, Code));
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(method(RootInit, {"public"}, 0, {}));
  return {Provider, Value, Storage, klass(String, true), Root};
}
} // namespace

namespace {
std::vector<Class> reorderedStringClasses(std::array<unsigned, 3> Order,
                                          bool SDK, bool Jumbo, bool Range,
                                          unsigned Continuation) {
  auto Input = fixture(0, SDK, false, false, Jumbo);
  auto &Provider = Input[0];
  auto &Caller = Provider.methods[0];
  const auto Init = Input[1].methods[0].reference;
  std::vector<Instruction> Code{op(0, "new-instance", {0}, Input[1].name)};
  unsigned PC = 2;
  for (auto Register : Order) {
    auto String =
        op(PC, Jumbo ? "const-string/jumbo" : "const-string", {Register});
    String.literal = "owned-argument-" + std::to_string(Register);
    Code.push_back(std::move(String));
    PC += Jumbo ? 3 : 2;
  }
  Code.push_back(op(PC, Range ? "invoke-direct/range" : "invoke-direct",
                    {0, 1, 2, 3}, Init));
  PC += 3;
  if (Continuation == 2) {
    Field Stored;
    Stored.reference = {Provider.name, "stored", Input[1].name};
    Stored.access = {"public", "static", "final"};
    Provider.fields.push_back(Stored);
    Code.push_back(op(PC, "sput-object", {0}, Stored.reference));
    Code.push_back(op(PC + 2, "return-void"));
    Caller.reference.name = "<clinit>";
    Caller.reference.returns = "V";
    Caller.access = {"static", "constructor"};
  } else {
    Code.push_back(op(PC, "return-object", {Continuation == 1 ? 1u : 0u}));
    if (Continuation == 1)
      Caller.reference.returns = "Ljava/lang/String;";
  }
  Caller.registers = 4;
  Caller.instructions = std::move(Code);
  Caller.code_end = Caller.instructions.back().pc + 1;
  return Input;
}
} // namespace

TEST(MobileDalvikReusedAllocationRead, InstanceFieldCanReplaceItsOwnReceiver) {
  auto input = fixture(2, true, true, false, false);
  auto &make = input[0].methods.front();
  auto &init = input[1].methods.front();
  init.reference.parameters.resize(1);
  // The only source reference to Storage lives in the destination word.
  // Argument assignment must read it before storing the String result.
  make.instructions.erase(make.instructions.begin() + 2);
  make.instructions[1].registers = {3, 3};
  make.instructions[2].registers = {0, 3};
  make.instructions[2].reference = init.reference;
  Budget budget;
  auto report = recoverJava(linkClasses(input, budget), budget);
  EXPECT_EQ(report.getInteger("unrecovered_method_count"), 0);
  bool assignment = false;
  for (const auto &unit : *report.getArray("source_units"))
    if (unit.getAsObject()->getString("class") == make.reference.owner) {
      auto text = unit.getAsObject()->getString("source")->str();
      assignment = text.find("(o3 = ((fixture.Storage) o3).first)") !=
                   std::string::npos;
    }
  EXPECT_TRUE(assignment);
}

TEST(MobileDalvikReusedAllocationRead,
     ReorderedStringsKeepProducerOrderAndConstructorArgumentSlots) {
  std::array<unsigned, 3> Order{1, 2, 3};
  do {
    for (bool SDK : {false, true})
      for (bool Jumbo : {false, true})
        for (bool Range : {false, true})
          for (unsigned Continuation : {0u, 1u, 2u})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{Order, SDK, Jumbo, Range, Continuation, Route}));
              auto Input = reorderedStringClasses(Order, SDK, Jumbo, Range,
                                                  Continuation);
              if (Route == 2)
                std::reverse(Input.begin(), Input.end());
              Budget B;
              ClassMap Classes;
              if (Route)
                Classes = linkClasses(Input, B);
              else
                for (const auto &C : Input)
                  Classes.emplace(C.name, C);
              const auto Report = recoverJava(Classes, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(Report.getInteger("projected_method_count").value_or(0),
                        0);
              const auto *Units = Report.getArray("source_units");
              ASSERT_NE(Units, nullptr);
              unsigned Matched = 0;
              for (const auto &Unit : *Units) {
                const auto *Object = Unit.getAsObject();
                ASSERT_NE(Object, nullptr);
                const auto Source = Object->getString("source");
                ASSERT_TRUE(Source);
                const auto Text = Source->str();
                auto Previous = Text.find("new fixture.Value(");
                if (Previous == std::string::npos)
                  continue;
                ++Matched;
                for (auto Register : Order) {
                  const auto Literal =
                      "\"owned-argument-" + std::to_string(Register) + "\"";
                  const auto At = Text.find(Literal, Previous);
                  ASSERT_NE(At, std::string::npos);
                  EXPECT_LT(Previous, At);
                  EXPECT_EQ(Text.find(Literal, At + 1), std::string::npos);
                  Previous = At;
                }
              }
              EXPECT_EQ(Matched, 1u);
            }
  } while (std::next_permutation(Order.begin(), Order.end()));
}

TEST(MobileDalvikReusedAllocationRead,
     ReorderedStringsRejectMalformedValuesAccessAndEntries) {
  for (unsigned Mutation = 0; Mutation < 14; ++Mutation)
    for (bool SDK : {false, true}) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, SDK}));
      auto Input = reorderedStringClasses({2, 3, 1}, SDK, false, false, 0);
      auto &Caller = Input[0].methods[0];
      auto &Code = Caller.instructions;
      if (Mutation == 0)
        Code[1].literal = int64_t(1);
      else if (Mutation == 1)
        Code[2].registers.clear();
      else if (Mutation == 2)
        Code[3].registers[0] = 0;
      else if (Mutation == 3)
        Code[4].registers.pop_back();
      else if (Mutation == 4)
        Code[4].registers.push_back(1);
      else if (Mutation == 5)
        std::get<MethodRef>(Code[4].reference).owner = "Ljava/lang/Object;";
      else if (Mutation == 6)
        Code[4].registers[0] = 2;
      else if (Mutation == 7)
        Code[4].opcode = "invoke-virtual";
      else if (Mutation == 8) {
        Input[1].methods[0].access.erase("public");
        Input[1].methods[0].access.insert("private");
      } else if (Mutation == 9)
        Input[1].access.insert("abstract");
      else if (Mutation == 10)
        Code[2].registers[0] = Caller.registers;
      else if (Mutation == 11)
        Code[2].target = Code[4].pc;
      else if (Mutation == 12)
        std::get<MethodRef>(Code[4].reference).parameters[1] = "I";
      else {
        for (auto &I : Code)
          I.pc += 2;
        auto Branch = op(0, "goto");
        Branch.target = Code[2].pc;
        Code.insert(Code.begin(), Branch);
        Caller.code_end += 2;
      }
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}

TEST(MobileDalvikReusedAllocationRead,
     CacheSharedStringsAndVolatileReadsInOriginalAllocationOrder) {
  for (unsigned Kind : {0u, 1u, 2u})
    for (bool TargetSDK : {false, true})
      for (bool FieldsSDK : {false, true})
        for (bool Head : {false, true})
          for (bool Jumbo : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{Kind, TargetSDK, FieldsSDK, Head, Jumbo, Route}));
              auto Input = fixture(Kind, TargetSDK, FieldsSDK, Head, Jumbo);
              const auto Original = Input;
              if (Route == 2)
                std::reverse(Input.begin(), Input.end());
              Budget B;
              ClassMap Classes;
              if (Route)
                Classes = linkClasses(Input, B);
              else
                for (const auto &C : Input)
                  Classes.emplace(C.name, C);
              const auto Report = recoverJava(Classes, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
              for (const auto &C : Original) {
                const auto &After = Classes.at(C.name);
                EXPECT_EQ(After.access, C.access);
                EXPECT_EQ(After.generic_signature, C.generic_signature);
                for (size_t I = 0; I < C.fields.size(); ++I) {
                  EXPECT_EQ(After.fields[I].reference, C.fields[I].reference);
                  EXPECT_EQ(After.fields[I].access, C.fields[I].access);
                }
                for (size_t I = 0; I < C.methods.size(); ++I) {
                  EXPECT_EQ(After.methods[I].reference, C.methods[I].reference);
                  EXPECT_EQ(After.methods[I].access, C.methods[I].access);
                  EXPECT_EQ(After.methods[I].generic_signature,
                            C.methods[I].generic_signature);
                }
              }
            }
  for (unsigned Kind : {1u, 2u}) {
    auto Input = fixture(Kind, false, false, true, false);
    auto &Provider = Input[0];
    Provider.fields = Input[2].fields;
    for (auto &Field : Provider.fields) {
      Field.reference.owner = Provider.name;
      Field.access.erase("public");
      Field.access.insert("private");
    }
    auto &Caller = Provider.methods[0];
    if (Kind == 2)
      Caller.reference.parameters[0] = Provider.name;
    for (unsigned I : {1u, 2u})
      std::get<FieldRef>(Caller.instructions[I].reference).owner =
          Provider.name;
    Budget B;
    const auto Report = recoverJava(linkClasses(Input, B), B);
    EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
    EXPECT_FALSE(Report.getArray("class_source_bindings"));
    EXPECT_FALSE(Report.getArray("generic_signature_projections"));
  }
}

TEST(MobileDalvikReusedAllocationRead,
     RejectInvalidProducersAccessReceiversAndConstructorFrames) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 18; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(1, SDK, SDK, true, false);
      auto &Caller = Input[0].methods[0];
      auto &Code = Caller.instructions;
      if (Mutation == 0) {
        Input = fixture(0, SDK, SDK, true, false);
        Input[0].methods[0].instructions[1].literal = int64_t(1);
      } else if (Mutation == 1)
        Code[1].reference = std::monostate{};
      else if (Mutation == 2)
        Input[2].fields.clear();
      else if (Mutation == 3)
        Input[2].fields[0].access.erase("static");
      else if (Mutation == 4) {
        Input[2].fields[0].access.erase("public");
        Input[2].fields[0].access.insert("private");
      } else if (Mutation == 5)
        std::get<MethodRef>(Code[3].reference).owner = "Ljava/lang/Object;";
      else if (Mutation == 6)
        Code[3].registers[0] = 1;
      else if (Mutation == 7)
        std::get<MethodRef>(Code[3].reference).returns = "Ljava/lang/String;";
      else if (Mutation == 8)
        Code[3].registers.pop_back();
      else if (Mutation == 9)
        Code[3].registers.push_back(2);
      else if (Mutation == 10)
        Code[1].registers[0] = 0;
      else if (Mutation == 11)
        Code[1].target = 6;
      else if (Mutation == 12)
        Input[1].access.insert("abstract");
      else if (Mutation == 13) {
        Input[1].methods[0].access.erase("public");
        Input[1].methods[0].access.insert("private");
      } else if (Mutation == 14) {
        Input = fixture(2, SDK, SDK, true, false);
        Input[0].methods[0].instructions[1].registers[1] = 0;
      } else if (Mutation == 15)
        Code[1].opcode = "sget-wide";
      else if (Mutation == 16)
        Code[3].opcode = "invoke-virtual";
      else
        Code[3].registers.back() = Caller.registers;
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}

namespace {
std::vector<Class> deferredFieldStringClasses(unsigned Kind, bool TargetSDK,
                                              bool ProducerSDK, bool Leading,
                                              bool Live, bool Jumbo,
                                              bool Range) {
  auto Input = fixture(Kind == 1 ? 2 : 1, TargetSDK, ProducerSDK, true, false);
  auto &Provider = Input[0];
  auto &Value = Input[1];
  auto &Storage = Input[2];
  const std::string String = "Ljava/lang/String;";
  auto &Init = Value.methods[0];
  Init.reference.parameters = {String, String};
  if (!TargetSDK)
    Init.registers = 3;
  const unsigned FieldRegister = Leading ? 2 : 1;
  const unsigned StringRegister = Leading ? 1 : 2;
  std::vector<Instruction> Code{op(0, "new-instance", {0}, Value.name)};
  unsigned PC = 2;
  if (Kind == 2) {
    auto Read = method({Storage.name, "read", {}, String}, {"public", "static"},
                       ProducerSDK ? 0 : 1, {});
    if (!ProducerSDK) {
      auto Zero = op(0, "const/4", {0});
      Zero.literal = int64_t(0);
      Read.instructions = {Zero, op(1, "return-object", {0})};
      Read.code_end = 2;
    }
    Storage.methods.push_back(Read);
    Code.push_back(op(PC, "invoke-static", {}, Read.reference));
    PC += 3;
    Code.push_back(op(PC++, "move-result-object", {FieldRegister}));
  } else {
    Code.push_back(op(PC, Kind == 1 ? "iget-object" : "sget-object",
                      Kind == 1 ? std::vector<unsigned>{FieldRegister, 3}
                                : std::vector<unsigned>{FieldRegister},
                      Storage.fields[0].reference));
    PC += 2;
  }
  auto Text =
      op(PC, Jumbo ? "const-string/jumbo" : "const-string", {StringRegister});
  Text.literal = std::string("owned trailing producer");
  Code.push_back(Text);
  PC += Jumbo ? 3 : 2;
  Code.push_back(op(PC, Range ? "invoke-direct/range" : "invoke-direct",
                    {0, 1, 2}, Init.reference));
  PC += 3;
  Code.push_back(op(PC, "return-object", {Live ? FieldRegister : 0}));
  Provider.methods[0] =
      method({Provider.name, "make",
              Kind == 1 ? std::vector<std::string>{Storage.name}
                        : std::vector<std::string>{},
              Live ? String : Value.name},
             {"public", "static"}, Kind == 1 ? 4 : 3, Code);
  return Input;
}
} // namespace

TEST(MobileDalvikReusedAllocationRead,
     LateStringsRetainEarlierFieldAndCallEffectsInConstructorArguments) {
  for (unsigned Kind : {0u, 1u, 2u})
    for (bool TargetSDK : {false, true})
      for (bool ProducerSDK : {false, true})
        for (bool Leading : {false, true})
          for (bool Live : {false, true})
            for (bool Jumbo : {false, true})
              for (bool Range : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{Kind, TargetSDK, ProducerSDK, Leading, Live,
                                 Jumbo, Range, Route}));
                  auto Input =
                      deferredFieldStringClasses(Kind, TargetSDK, ProducerSDK,
                                                 Leading, Live, Jumbo, Range);
                  if (Route == 2)
                    std::reverse(Input.begin(), Input.end());
                  Budget B;
                  ClassMap Classes;
                  if (Route)
                    Classes = linkClasses(Input, B);
                  else
                    for (const auto &C : Input)
                      Classes.emplace(C.name, C);
                  auto Report = recoverJava(Classes, B);
                  EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                  EXPECT_EQ(
                      Report.getInteger("projected_method_count").value_or(0),
                      0);
                  EXPECT_EQ(
                      Report.getInteger("minimum_java_release").value_or(8), 8);
                  const auto *Units = Report.getArray("source_units");
                  ASSERT_NE(Units, nullptr);
                  unsigned Matched = 0;
                  for (const auto &Unit : *Units) {
                    const auto *Object = Unit.getAsObject();
                    if (!Object)
                      continue;
                    const auto Source = Object->getString("source");
                    if (!Source)
                      continue;
                    const auto Text = Source->str();
                    const auto New = Text.find("new fixture.Value(");
                    if (New == std::string::npos)
                      continue;
                    ++Matched;
                    const auto Producer = Kind == 2 ? ".read(" : ".first";
                    const auto Read = Text.find(Producer, New);
                    const auto Literal =
                        Text.find("\"owned trailing producer\"", New);
                    ASSERT_NE(Read, std::string::npos);
                    ASSERT_NE(Literal, std::string::npos);
                    EXPECT_LT(New, Read);
                    EXPECT_LT(Read, Literal);
                    EXPECT_EQ(Text.find(Producer, Read + 1), std::string::npos);
                  }
                  EXPECT_EQ(Matched, 1u);
                }
}

TEST(MobileDalvikReusedAllocationRead,
     LateStringsRejectBrokenProducersFramesAccessAndRegionBoundaries) {
  for (unsigned Mutation = 0; Mutation < 18; ++Mutation)
    for (bool SDK : {false, true})
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{Mutation, SDK, Route}));
        auto Input =
            deferredFieldStringClasses(0, SDK, SDK, true, true, false, false);
        auto &M = Input[0].methods[0];
        auto &Code = M.instructions;
        if (Mutation == 0)
          Code[2].literal = int64_t(1);
        else if (Mutation == 1)
          Code[2].registers.clear();
        else if (Mutation == 2)
          Code[2].registers[0] = 0;
        else if (Mutation == 3)
          Input[2].fields[0].access.erase("static");
        else if (Mutation == 4) {
          Input[2].fields[0].access.erase("public");
          Input[2].fields[0].access.insert("private");
        } else if (Mutation == 5)
          std::get<FieldRef>(Code[1].reference).type = "Ljava/lang/Object;";
        else if (Mutation == 6)
          Code[3].registers[0] = 2;
        else if (Mutation == 7)
          Code[3].registers.pop_back();
        else if (Mutation == 8)
          Code[3].registers.push_back(2);
        else if (Mutation == 9)
          std::get<MethodRef>(Code[3].reference).parameters[0] =
              "Ljava/lang/Object;";
        else if (Mutation == 10)
          Code[2].target = Code[3].pc;
        else if (Mutation == 11)
          Input[1].access.insert("abstract");
        else if (Mutation == 12) {
          Input[1].methods[0].access.erase("public");
          Input[1].methods[0].access.insert("private");
        } else if (Mutation == 13)
          Code[1].registers[0] = 0;
        else if (Mutation == 14)
          Code[3].opcode = "invoke-virtual";
        else if (Mutation == 15)
          Code[2].registers[0] = M.registers;
        else if (Mutation == 16)
          Code[1].opcode = "sget-wide";
        else {
          for (auto &I : Code)
            I.pc += 2;
          auto Branch = op(0, "goto");
          Branch.target = Code[2].pc;
          Code.insert(Code.begin(), Branch);
          M.code_end += 2;
        }
        if (Route == 2)
          std::reverse(Input.begin(), Input.end());
        Budget B;
        EXPECT_THROW(
            {
              ClassMap Classes;
              if (Route)
                Classes = linkClasses(Input, B);
              else
                for (const auto &C : Input)
                  Classes.emplace(C.name, C);
              recoverJava(Classes, B);
            },
            std::runtime_error);
      }
}

namespace {
std::vector<Class> castedStringClasses(const std::string &Target, bool SDK,
                                       bool Jumbo, bool Copy, bool Live,
                                       bool Range) {
  auto Input = fixture(0, SDK, false, false, Jumbo);
  auto &Init = Input[1].methods[0];
  Init.reference.parameters = {Target};
  if (!SDK)
    Init.registers = 2;
  auto &Caller = Input[0].methods[0];
  const unsigned Receiver = Copy && Range ? 1 : 0;
  const unsigned Source = Copy && Range ? 0 : 1;
  auto Text = op(2, Jumbo ? "const-string/jumbo" : "const-string", {Source});
  Text.literal = std::string("owned casted string");
  std::vector<Instruction> Code{
      op(0, "new-instance", {Receiver}, Input[1].name), Text};
  unsigned PC = Jumbo ? 5 : 4;
  const unsigned Value = Copy ? 2 : 1;
  if (Copy) {
    Code.push_back(op(PC, "move-object/from16", {2, Source}));
    PC += 2;
  }
  Code.push_back(op(PC, "check-cast", {Value}, Target));
  PC += 2;
  Code.push_back(op(PC, Range ? "invoke-direct/range" : "invoke-direct",
                    {Receiver, Value}, Init.reference));
  PC += 3;
  Code.push_back(op(PC, "return-object", {Live ? Value : Receiver}));
  Caller.registers = 3;
  Caller.reference.returns = Live ? Target : Input[1].name;
  Caller.instructions = std::move(Code);
  Caller.code_end = PC + 1;
  const std::string Sequence = "Ljava/lang/CharSequence;";
  auto Declaration = klass(Sequence, true);
  Declaration.access = {"public", "interface", "abstract"};
  Input[3].interfaces.push_back(Sequence);
  Input.push_back(Declaration);
  return Input;
}
} // namespace

TEST(MobileDalvikReusedAllocationRead,
     StringsRetainFollowingCastsAndReferenceCopies) {
  for (const auto &Target :
       {"Ljava/lang/String;", "Ljava/lang/CharSequence;", "Ljava/lang/Object;"})
    for (bool SDK : {false, true})
      for (bool Jumbo : {false, true})
        for (bool Copy : {false, true})
          for (bool Live : {false, true})
            for (bool Range : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{Target, SDK, Jumbo, Copy, Live, Range, Route}));
                auto Input =
                    castedStringClasses(Target, SDK, Jumbo, Copy, Live, Range);
                if (Route == 2)
                  std::reverse(Input.begin(), Input.end());
                Budget B;
                ClassMap Classes;
                if (Route)
                  Classes = linkClasses(Input, B);
                else
                  for (const auto &C : Input)
                    Classes.emplace(C.name, C);
                const auto Report = recoverJava(Classes, B);
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                EXPECT_EQ(
                    Report.getInteger("projected_method_count").value_or(0), 0);
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
              }
}

TEST(MobileDalvikReusedAllocationRead,
     StringCastsRejectMalformedOperandsAndUninitializedUses) {
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation)
    for (bool SDK : {false, true}) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, SDK}));
      auto Input = castedStringClasses("Ljava/lang/CharSequence;", SDK, false,
                                       false, false, false);
      auto &Caller = Input[0].methods[0];
      auto &Code = Caller.instructions;
      if (Mutation == 0)
        Code[2].registers = {0};
      else if (Mutation == 1)
        Code[2].registers.clear();
      else if (Mutation == 2)
        Code[2].registers.push_back(2);
      else if (Mutation == 3)
        Code[2].reference = std::monostate{};
      else if (Mutation == 4)
        Code[2].reference = std::string("I");
      else if (Mutation == 5)
        Code[2].reference = std::string("Lfixture/Missing;");
      else if (Mutation == 6)
        Code[2].target = Code[3].pc;
      else if (Mutation == 7)
        Code[2].registers[0] = Caller.registers;
      else if (Mutation == 8)
        Code[3].registers[1] = 0;
      else if (Mutation == 9)
        Code[1].literal = int64_t(0);
      else if (Mutation == 10)
        Code[1].registers = {0};
      else {
        for (auto &I : Code)
          I.pc += 2;
        auto Branch = op(0, "goto");
        Branch.target = Code[2].pc;
        Code.insert(Code.begin(), Branch);
        Caller.code_end += 2;
      }
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
