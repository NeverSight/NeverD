#include "MobileDalvik.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <tuple>
using namespace neverd::mobile;
using namespace neverd::mobile::dalvik;
namespace {
Instruction op(unsigned PC, std::string Name, std::vector<unsigned> Words = {},
               Reference Ref = {}) {
  Instruction I;
  I.pc = PC;
  I.opcode = std::move(Name);
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
  C.methods = std::move(Methods);
  C.external = External;
  return C;
}
std::vector<Class> fixture(bool Range, bool External, bool Frame, bool Generic,
                           bool Names, bool Loop) {
  const std::string Core = "Lfixture/Core;", Stage = "Lfixture/Stage;",
                    Object = "Ljava/lang/Object;";
  FieldRef Prefix{Core, Names ? "__neverdFinalGuardValue9" : "prefix", "I"},
      Choice{Core, "choice", "Z"}, Value{Core, "value", Object},
      Wide{Core, "wide", "J"}, Last{Core, "last", Object};
  std::vector<Method> Methods{
      method({Stage, "start", {Object, "J"}, "I"}, {"public", "static"}, 4,
             {op(0, "const/4", {0}), op(1, "return", {0})}),
      method({Stage, "choose", {"I"}, "Z"}, {"public", "static"}, 1,
             {op(0, "return", {0})}),
      method({Stage, "value", {Object, "J"}, Object}, {"public", "static"}, 3,
             {op(0, "return-object", {0})}),
      method({Stage, "cleanup", {Object}, "V"}, {"public", "static"}, 1,
             {op(0, "return-void")}),
      method({Stage, "finish", {"J"}, "J"}, {"public", "static"}, 2,
             {op(0, "return-wide", {0})}),
      method({Stage, "end", {Object}, Object}, {"public", "static"}, 1,
             {op(0, "return-object", {0})})};
  Methods[0].instructions[0].literal = int64_t(7);
  const std::string Call = Range ? "invoke-static/range" : "invoke-static";
  auto M = method(
      {Core, "<init>", {Object, "J", "I"}, "V"}, {"public", "constructor"}, 17,
      {op(0, "invoke-direct", {12}, MethodRef{Object, "<init>", {}, "V"}),
       op(1, "move-object", {5, 12}),
       op(2, Call, {13, 14, 15}, Methods[0].reference),
       op(3, "move-result", {0}),
       op(4, "iput", {0, 5}, Prefix),
       op(5, Call, {16}, Methods[1].reference),
       op(6, "move-result", {0}),
       op(7, "if-eqz", {0}),
       op(8, "const/4", {0}),
       op(9, "iput-boolean", {0, 5}, Choice),
       op(10, Call, {13, 14, 15}, Methods[2].reference),
       op(11, "move-result-object", {1}),
       op(12, "iput-object", {1, 5}, Value),
       op(13, "goto"),
       op(14, "const/4", {0}),
       op(15, "iput-boolean", {0, 5}, Choice),
       op(16, Call, {13, 14, 15}, Methods[2].reference),
       op(17, "move-result-object", {1}),
       op(18, "iput-object", {1, 5}, Value),
       op(19, "goto"),
       op(20, Call, {13}, Methods[3].reference),
       op(21, Call, {14, 15}, Methods[4].reference),
       op(22, "move-result-wide", {2}),
       op(23, "iput-wide", {2, 5}, Wide),
       op(24, Call, {13}, Methods[5].reference),
       op(25, "move-result-object", {1}),
       op(26, "iput-object", {1, 5}, Last),
       op(27, "return-void"),
       op(28, "move-exception", {4}),
       op(29, "move-object", {6, 13}),
       op(30, "goto"),
       op(31, "move-exception", {4}),
       op(32, "move-object", {7, 13}),
       op(33, "goto"),
       op(34, Call, {13}, Methods[3].reference),
       op(35, "throw", {4})});
  auto &C = M.instructions;
  C[7].target = 14;
  C[8].literal = int64_t(1);
  C[14].literal = int64_t(0);
  C[13].target = C[19].target = 20;
  C[30].target = C[33].target = 34;
  M.tries = {{5, 14, {{std::nullopt, 28}}}, {14, 20, {{std::nullopt, 31}}}};
  if (Loop) {
    for (auto &I : C) {
      if (I.pc >= 5)
        I.pc += 3;
      if (I.target && *I.target >= 5)
        *I.target += 3;
    }
    auto Start = op(5, "const/4", {8});
    Start.literal = int64_t(1);
    auto Decrement = op(6, "add-int/lit8", {8, 8});
    Decrement.literal = int64_t(-1);
    auto Guard = op(7, "if-gtz", {8});
    Guard.target = 6;
    C.insert(C.begin() + 5, {Start, Decrement, Guard});
    for (auto &T : M.tries) {
      T.start += 3;
      T.end += 3;
      for (auto &H : T.handlers)
        H.target += 3;
    }
    M.code_end += 3;
  }
  if (Frame) {
    M.registers += 3;
    for (auto &I : C)
      for (auto &R : I.registers)
        R += 3;
  }
  auto Owner = klass(Core, {M});
  for (const auto &F : {Prefix, Choice, Value, Wide, Last})
    Owner.fields.push_back({F, {"public", "final"}});
  if (Generic) {
    Owner.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
    Owner.fields[2].generic_signature = "TT;";
    Owner.fields[4].generic_signature = "TT;";
    Owner.methods[0].generic_signature = "(TT;JI)V";
  }
  auto Provider = klass(Stage, Methods, External);
  if (External)
    for (auto &M : Provider.methods) {
      M.instructions.clear();
      M.registers = M.code_end = 0;
    }
  auto Root =
      klass(Object,
            {method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0)},
            true);
  Root.access = {"public"};
  Root.superclass.reset();
  return {Owner, Provider, Root};
}

std::vector<Class> staticCleanupFixture(bool Range, bool External, bool Frame) {
  const std::string Core = "Lfixture/Core;", Stage = "Lfixture/Stage;",
                    Object = "Ljava/lang/Object;";
  MethodRef Cleanup{Stage, "cleanup", {Object}, "V"};
  MethodRef Init{Object, "<init>", {}, "V"},
      Inside{Stage, "inside", {Object}, "V"},
      Finish{Stage, "finish", {Object}, "I"};
  FieldRef Lock{Core, "lock", Object}, Value{Core, "value", "I"};
  auto M = method(
      {Core, "<clinit>", {}, "V"}, {"static", "constructor"}, 3,
      {op(0, "new-instance", {0}, Object), op(1, "invoke-direct", {0}, Init),
       op(2, "sput-object", {0}, Lock), op(3, "nop"),
       op(4, Range ? "invoke-static/range" : "invoke-static", {0}, Inside),
       op(5, Range ? "invoke-static/range" : "invoke-static", {0}, Cleanup),
       op(6, Range ? "invoke-static/range" : "invoke-static", {0}, Finish),
       op(7, "move-result", {1}), op(8, "sput", {1}, Value),
       op(9, "return-void"), op(10, "move-exception", {2}),
       op(11, Range ? "invoke-static/range" : "invoke-static", {0}, Cleanup),
       op(12, "throw", {2})});
  M.tries = {{4, 5, {{std::nullopt, 10}}}};
  if (Frame) {
    M.registers += 4;
    for (auto &I : M.instructions)
      for (auto &R : I.registers)
        R += 4;
  }
  auto Owner = klass(Core, {M});
  Owner.fields = {{Lock, {"public", "static", "final"}, {}},
                  {Value, {"public", "static", "final"}, int64_t(0)}};
  auto Last = method(Finish, {"public", "static"}, 2,
                     {op(0, "const/16", {0}), op(1, "return", {0})});
  Last.instructions[0].literal = int64_t(17);
  auto Provider = klass(
      Stage,
      {method(Inside, {"public", "static"}, 1, {op(0, "return-void")}),
       method(Cleanup, {"public", "static"}, 1, {op(0, "return-void")}), Last},
      External);
  if (External)
    for (auto &F : Provider.methods) {
      F.instructions.clear();
      F.registers = F.code_end = 0;
    }
  auto Root = klass(Object, {method(Init, {"public", "constructor"}, 0)}, true);
  Root.access = {"public"};
  Root.superclass.reset();
  return {Owner, Provider, Root};
}

std::vector<Class> staticMonitorFixture(bool Range, bool External, bool Frame,
                                        bool InnerStore) {
  auto Input = staticCleanupFixture(Range, External, false);
  auto &M = Input[0].methods[0];
  const auto &Lock = Input[0].fields[0].reference;
  const auto &Value = Input[0].fields[1].reference;
  const auto Inside = Input[1].methods[0].reference;
  if (InnerStore) {
    M.instructions = {
        op(0, "new-instance", {0}, std::string("Ljava/lang/Object;")),
        op(1, "invoke-direct", {0},
           MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
        op(2, "sput-object", {0}, Lock),
        op(3, "const/16", {1}),
        op(4, "monitor-enter", {0}),
        op(5, Range ? "invoke-static/range" : "invoke-static", {0}, Inside),
        op(6, "sput", {1}, Value),
        op(7, "monitor-exit", {0}),
        op(8, "return-void"),
        op(9, "move-exception", {2}),
        op(10, "monitor-exit", {0}),
        op(11, "throw", {2})};
    M.instructions[3].literal = int64_t(17);
    M.code_end = 12;
    M.tries = {{5, 7, {{std::nullopt, 9}}}};
  } else {
    M.instructions[3] = op(3, "monitor-enter", {0});
    M.instructions[5] = op(5, "monitor-exit", {0});
    M.instructions[11] = op(11, "monitor-exit", {0});
  }
  if (Frame) {
    M.registers += 4;
    for (auto &I : M.instructions)
      for (auto &R : I.registers)
        R += 4;
  }
  return Input;
}

ClassMap classes(std::vector<Class> Input, unsigned Route, Budget &B) {
  if (Route == 2)
    std::reverse(Input.begin(), Input.end());
  if (Route)
    return linkClasses(std::move(Input), B);
  ClassMap Out;
  for (auto &C : Input)
    Out.emplace(C.name, std::move(C));
  return Out;
}
} // namespace

TEST(MobileDalvikFinalStages, ProtectedAlternativeStoresKeepCommonEventOrder) {
  for (bool Range : {false, true})
    for (bool External : {false, true})
      for (bool Frame : {false, true})
        for (bool Generic : {false, true})
          for (bool Names : {false, true})
            for (bool Loop : {false, true})
              for (unsigned Route = 0; Route < 3; ++Route) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    Range, External, Frame, Generic, Names, Loop, Route}));
                Budget B;
                auto R = recoverJava(classes(fixture(Range, External, Frame,
                                                     Generic, Names, Loop),
                                             Route, B),
                                     B);
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_EQ(R.getInteger("projected_method_count").value_or(0),
                          0);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
              }
}
TEST(MobileDalvikFinalStages, MissingRepeatedOrForeignWritesAndOpenEdgesFail) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = fixture(false, false, false, false, false, false);
      auto &M = Input[0].methods[0];
      auto &C = M.instructions;
      switch (Mutation) {
      case 0:
        C[15] = op(15, "nop");
        break;
      case 1:
        C[18].reference = Input[0].fields[1].reference;
        C[18].opcode = "iput-boolean";
        C[18].registers[0] = 0;
        break;
      case 2:
        C[13].target = 27;
        break;
      case 3:
        C[30].target = 7;
        break;
      case 4:
        C[35] = op(35, "return-void");
        break;
      case 5:
        C[23].registers[0] = 3;
        break;
      case 6:
        C[12].registers[1] = 13;
        break;
      case 7:
        C[19].target = 999;
        break;
      case 8:
        C[5].opcode = "invoke-virtual";
        break;
      case 9:
        C[12].opcode = "iput";
        break;
      case 10:
        C[13].target = 28;
        break;
      case 11:
        M.tries[0].handlers[0].target = 6;
        break;
      case 12:
        M.tries[0].handlers[0].target = 35;
        break;
      case 13:
        C[9].literal = int64_t(1);
        break;
      case 14:
        C[19].target = 14;
        break;
      case 15:
        std::get<FieldRef>(C[12].reference).owner = "Lfixture/Foreign;";
        break;
      case 16:
        C[0] = op(0, "nop");
        break;
      case 17:
        M.code_end = 15;
        break;
      case 18:
        C[26].reference = Input[0].fields[0].reference;
        C[26].opcode = "iput";
        C[26].registers[0] = 0;
        break;
      case 19:
        M.tries[0].start = 0;
        break;
      }
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}

TEST(MobileDalvikFinalStages, RethrowPaddingKeepsStaticAndInstanceFinalStages) {
  for (bool Static : {true, false})
    for (bool Range : {false, true})
      for (bool External : {false, true})
        for (bool Frame : {false, true})
          for (unsigned Padding = 0; Padding < 4; ++Padding)
            for (unsigned Route = 0; Route < 3; ++Route) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{Static, Range, External, Frame, Padding, Route}));
              auto Input =
                  Static ? staticCleanupFixture(Range, External, Frame)
                         : fixture(Range, External, Frame, true, false, true);
              auto &M = Input[0].methods[0];
              for (unsigned I = 0; I < Padding; ++I)
                M.instructions.push_back(op(M.code_end++, "nop"));
              Budget B;
              llvm::json::Object R;
              ASSERT_NO_THROW(
                  R = recoverJava(classes(std::move(Input), Route, B), B));
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
            }
}
TEST(MobileDalvikFinalStages,
     RethrowPaddingRejectsDecodedOperandsAndIncomingEdges) {
  for (unsigned Mutation = 0; Mutation < 18; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input = staticCleanupFixture(false, true, false);
      auto &M = Input[0].methods[0];
      auto &C = M.instructions;
      C.push_back(op(M.code_end++, "nop"));
      switch (Mutation) {
      case 0:
        C.back().registers = {0};
        break;
      case 1:
        C.back().literal = int64_t(0);
        break;
      case 2:
        C.back().reference = std::string("Ljava/lang/Object;");
        break;
      case 3:
        C.back().target = 0;
        break;
      case 4:
        C.back().targets = {0};
        break;
      case 5:
        C.back().keys = {0};
        break;
      case 6:
        C.back().data = {1};
        break;
      case 7:
        C.back().element_width = 1;
        break;
      case 8:
        C.back().opcode = "nop/unknown";
        break;
      case 9:
        C.back().pc = C[C.size() - 2].pc;
        break;
      case 10:
        M.code_end = C.back().pc;
        break;
      case 11: {
        for (auto &I : C) {
          if (I.pc >= 8)
            ++I.pc;
          if (I.target && *I.target >= 8)
            ++*I.target;
        }
        for (auto &T : M.tries)
          for (auto &H : T.handlers)
            if (H.target >= 8)
              ++H.target;
        auto Guard = op(8, "if-eqz", {1});
        Guard.target = C.back().pc;
        C.insert(C.begin() + 8, Guard);
        ++M.code_end;
        break;
      }
      case 12:
        M.tries[0].handlers[0].target = C.back().pc;
        break;
      case 13:
        C[12] = op(12, "return-void");
        break;
      case 14:
        C[12] = op(12, "nop");
        break;
      case 15:
        C[11].opcode = "invoke-virtual";
        break;
      case 16:
        C[12].registers.clear();
        break;
      case 17:
        C[8].reference = FieldRef{"Lfixture/Foreign;", "value", "I"};
        break;
      }
      Budget B;
      EXPECT_THROW(recoverJava(classes(std::move(Input), Route, B), B),
                   std::runtime_error);
    }
}

TEST(MobileDalvikFinalStages, StaticMonitorsStayWithinOneFinalAssignmentStage) {
  for (bool Range : {false, true})
    for (bool External : {false, true})
      for (bool Frame : {false, true})
        for (unsigned Padding = 0; Padding < 3; ++Padding)
          for (unsigned Route = 0; Route < 3; ++Route) {
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{Range, External, Frame, Padding, Route}));
            auto Input = staticMonitorFixture(Range, External, Frame, false);
            auto &M = Input[0].methods[0];
            for (unsigned I = 0; I < Padding; ++I)
              M.instructions.push_back(op(M.code_end++, "nop"));
            const auto OriginalInstructions = M.instructions.size();
            Budget B;
            llvm::json::Object R;
            ASSERT_NO_THROW(
                R = recoverJava(classes(std::move(Input), Route, B), B));
            EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
            EXPECT_FALSE(R.getArray("generic_signature_projections"));
            unsigned Initializers = 0;
            for (const auto &Row : *R.getArray("methods")) {
              const auto &Method = *Row.getAsObject();
              if (Method.getString("identity") ==
                  "Lfixture/Core;-><clinit>()V") {
                ++Initializers;
                EXPECT_EQ(Method.getInteger("instruction_count"),
                          OriginalInstructions);
              }
            }
            EXPECT_EQ(Initializers, 1u);
          }
}

TEST(MobileDalvikFinalStages, StaticMonitorsRejectFinalStoresWhileHeld) {
  for (bool Range : {false, true})
    for (bool External : {false, true})
      for (bool Frame : {false, true})
        for (unsigned Route = 0; Route < 3; ++Route) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{Range, External, Frame, Route}));
          Budget B;
          EXPECT_THROW(recoverJava(classes(staticMonitorFixture(Range, External,
                                                                Frame, true),
                                           Route, B),
                                   B),
                       std::runtime_error);
        }
}

namespace {
std::vector<Class> loopingFinalBranchClasses(bool Scalar, bool External,
                                             bool Range, bool Varargs) {
  const std::string Core = "Lfixture/Core;", Stage = "Lfixture/Stage;";
  const std::string LeftType = Scalar ? "J" : "[Ljava/lang/Object;";
  const std::string RightType = Scalar ? "I" : "Ljava/lang/String;";
  FieldRef Left{Core, "left", LeftType}, Right{Core, "right", RightType};
  auto Value = method({Stage, "value", {}, RightType}, {"public", "static"}, 1,
                      {op(0, Scalar ? "const/4" : "const-string", {0}),
                       op(1, Scalar ? "return" : "return-object", {0})});
  if (Scalar)
    Value.instructions[0].literal = int64_t(7);
  else
    Value.instructions[0].literal = std::string("owned value");
  auto Provider = klass(Stage, {Value}, External);
  if (External) {
    Provider.methods[0].instructions.clear();
    Provider.methods[0].registers = Provider.methods[0].code_end = 0;
  }
  auto M = method({Core, "<init>", {"[Ljava/lang/Object;"}, "V"},
                  {"public", "constructor"}, 10);
  if (Varargs)
    M.access.insert("varargs");
  auto emit = [&](Instruction I) {
    I.pc = M.instructions.size();
    M.instructions.push_back(std::move(I));
    return M.instructions.size() - 1;
  };
  emit(op(0, "invoke-direct", {8},
          MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}));
  emit(op(0, "array-length", {1, 9}));
  const auto Branch = emit(op(0, "if-nez", {1}));
  auto At = emit(op(0, Scalar ? "const-wide/16" : "const/4", {0}));
  M.instructions[At].literal = int64_t(Scalar ? 3 : 0);
  emit(op(0, Scalar ? "iput-wide" : "iput-object", {0, 8}, Left));
  At = emit(op(0, Scalar ? "const/4" : "const-string", {0}));
  if (Scalar)
    M.instructions[At].literal = int64_t(5);
  else
    M.instructions[At].literal = std::string("owned empty");
  emit(op(0, Scalar ? "iput" : "iput-object", {0, 8}, Right));
  const auto Jump = emit(op(0, "goto"));
  M.instructions[Branch].target = M.instructions.size();
  At = emit(op(0, "const/4", {2}));
  M.instructions[At].literal = int64_t(0);
  const auto Loop = emit(op(0, "if-ge", {2, 1}));
  At = emit(op(0, "add-int/lit8", {2, 2}));
  M.instructions[At].literal = int64_t(1);
  At = emit(op(0, "goto"));
  M.instructions[At].target = Loop;
  M.instructions[Loop].target = M.instructions.size();
  if (Scalar) {
    At = emit(op(0, "const-wide/16", {0}));
    M.instructions[At].literal = int64_t(11);
  }
  emit(
      op(0, Scalar ? "iput-wide" : "iput-object", {Scalar ? 0u : 9u, 8}, Left));
  emit(op(0, Range ? "invoke-static/range" : "invoke-static", {},
          Value.reference));
  emit(op(0, Scalar ? "move-result" : "move-result-object", {0}));
  emit(op(0, Scalar ? "iput" : "iput-object", {0, 8}, Right));
  M.instructions[Jump].target = M.instructions.size();
  emit(op(0, "return-void"));
  M.code_end = M.instructions.size();
  auto Owner = klass(Core, {M});
  Owner.fields = {{Left, {"public", "final"}, {}},
                  {Right, {"public", "final"}, {}}};
  return {Owner, Provider};
}
} // namespace

TEST(MobileDalvikFinalStages, UnprotectedLoopBranchesKeepOrderedFieldEvents) {
  for (bool Scalar : {false, true})
    for (bool External : {false, true})
      for (bool Range : {false, true})
        for (bool Varargs : {false, true})
          for (unsigned Route : {0u, 1u, 2u}) {
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{Scalar, External, Range, Varargs, Route}));
            auto Input =
                loopingFinalBranchClasses(Scalar, External, Range, Varargs);
            if (Route == 2)
              std::reverse(Input.begin(), Input.end());
            Budget B;
            ClassMap Classes;
            for (const auto &C : Input)
              Classes.emplace(C.name, C);
            const auto R =
                recoverJava(Route ? linkClasses(Input, B) : Classes, B);
            EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
            EXPECT_EQ(R.getInteger("projected_method_count").value_or(0), 0);
            bool Found = false;
            for (const auto &Unit : *R.getArray("source_units"))
              if (Unit.getAsObject()->getString("class") == "Lfixture/Core;") {
                Found = true;
                const auto Text =
                    Unit.getAsObject()->getString("source")->str();
                EXPECT_NE(Text.find("__neverdRegionOrdered"),
                          std::string::npos);
                EXPECT_NE(Text.find("this.left ="), std::string::npos);
                EXPECT_NE(Text.find("this.right ="), std::string::npos);
              }
            EXPECT_TRUE(Found);
          }
}

TEST(MobileDalvikFinalStages, UnprotectedLoopBranchesRejectChangedFieldOrder) {
  for (unsigned Mutation = 0; Mutation < 12; ++Mutation)
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input = loopingFinalBranchClasses(false, false, false, false);
      auto &M = Input[0].methods[0];
      auto &C = M.instructions;
      switch (Mutation) {
      case 0:
        C[6] = op(6, "nop");
        break;
      case 1:
        C[4] = op(4, "nop");
        break;
      case 2:
        C[7].target = 4;
        break;
      case 3:
        C[11].target = 4;
        break;
      case 4:
        C[4].registers[1] = 9;
        break;
      case 5:
        C[4].opcode = "iput";
        break;
      case 6:
        std::get<FieldRef>(C[12].reference) = Input[0].fields[1].reference;
        break;
      case 7:
        C[13] = op(13, "return-void");
        break;
      case 8:
        C[6].reference = Input[0].fields[0].reference;
        break;
      case 9:
        C[11].target = 1000;
        break;
      case 10:
        C[4].reference =
            FieldRef{"Lfixture/Foreign;", "left", "[Ljava/lang/Object;"};
        break;
      case 11:
        C[4].literal = int64_t(1);
        break;
      }
      if (Route == 2)
        std::reverse(Input.begin(), Input.end());
      Budget B;
      ClassMap Classes;
      for (const auto &Type : Input)
        Classes.emplace(Type.name, Type);
      EXPECT_THROW(recoverJava(Route ? linkClasses(Input, B) : Classes, B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> prefixLoopFinalClasses(bool Scalar, bool External,
                                          bool Range, bool Varargs,
                                          bool Prefix) {
  auto Input = loopingFinalBranchClasses(Scalar, External, Range, Varargs);
  auto &Owner = Input[0];
  auto &M = Owner.methods[0];
  M.instructions.clear();
  const auto Left = Owner.fields[0].reference,
             Right = Owner.fields[1].reference;
  auto emit = [&](Instruction I) {
    I.pc = M.instructions.size();
    M.instructions.push_back(std::move(I));
    return M.instructions.size() - 1;
  };
  emit(op(0, "invoke-direct", {8},
          MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}));
  emit(op(0, "array-length", {1, 9}));
  if (Prefix) {
    FieldRef First{Owner.name, "prefix", "I"};
    Owner.fields.push_back({First, {"public", "final"}, {}});
    emit(op(0, "iput", {1, 8}, First));
  }
  auto At = emit(op(0, "const/4", {2}));
  M.instructions[At].literal = int64_t(0);
  const auto Loop = emit(op(0, "if-ge", {2, 1}));
  At = emit(op(0, "add-int/lit8", {2, 2}));
  M.instructions[At].literal = int64_t(1);
  At = emit(op(0, "goto"));
  M.instructions[At].target = Loop;
  M.instructions[Loop].target = M.instructions.size();
  const auto Branch = emit(op(0, "if-nez", {1}));
  At = emit(op(0, Scalar ? "const-wide/16" : "const/4", {0}));
  M.instructions[At].literal = int64_t(Scalar ? 3 : 0);
  emit(op(0, Scalar ? "iput-wide" : "iput-object", {0, 8}, Left));
  At = emit(op(0, Scalar ? "const/4" : "const-string", {0}));
  if (Scalar)
    M.instructions[At].literal = int64_t(5);
  else
    M.instructions[At].literal = std::string("owned empty");
  emit(op(0, Scalar ? "iput" : "iput-object", {0, 8}, Right));
  const auto Jump = emit(op(0, "goto"));
  M.instructions[Branch].target = M.instructions.size();
  if (Scalar) {
    At = emit(op(0, "const-wide/16", {0}));
    M.instructions[At].literal = int64_t(11);
  }
  emit(
      op(0, Scalar ? "iput-wide" : "iput-object", {Scalar ? 0u : 9u, 8}, Left));
  emit(op(0, Range ? "invoke-static/range" : "invoke-static", {},
          Input[1].methods[0].reference));
  emit(op(0, Scalar ? "move-result" : "move-result-object", {0}));
  emit(op(0, Scalar ? "iput" : "iput-object", {0, 8}, Right));
  M.instructions[Jump].target = M.instructions.size();
  emit(op(0, "return-void"));
  M.code_end = M.instructions.size();
  return Input;
}
} // namespace

TEST(MobileDalvikFinalStages, PrefixLoopsKeepAlternativeFinalEventOrder) {
  for (bool Scalar : {false, true})
    for (bool External : {false, true})
      for (bool Range : {false, true})
        for (bool Varargs : {false, true})
          for (bool Prefix : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{Scalar, External, Range, Varargs, Prefix, Route}));
              auto Input = prefixLoopFinalClasses(Scalar, External, Range,
                                                  Varargs, Prefix);
              const auto Original = Input[0].methods[0];
              Budget B;
              const auto Linked = classes(Input, Route, B);
              const auto R = recoverJava(Linked, B);
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(R.getInteger("projected_method_count").value_or(0), 0);
              EXPECT_FALSE(R.getArray("field_source_bindings"));
              EXPECT_FALSE(R.getBoolean("requires_java_rebuild_postprocessing")
                               .value_or(false));
              const auto &After =
                  Linked.at(Original.reference.owner).methods[0];
              EXPECT_EQ(After.access, Original.access);
              EXPECT_EQ(After.generic_signature, Original.generic_signature);
              bool Found = false;
              for (const auto &Unit : *R.getArray("source_units"))
                if (Unit.getAsObject()->getString("class") ==
                    "Lfixture/Core;") {
                  Found = true;
                  const auto Text =
                      Unit.getAsObject()->getString("source")->str();
                  EXPECT_NE(Text.find("__neverdRegionOrdered"),
                            std::string::npos);
                  EXPECT_NE(Text.find("this.left ="), std::string::npos);
                  EXPECT_NE(Text.find("this.right ="), std::string::npos);
                }
              EXPECT_TRUE(Found);
            }
}

TEST(MobileDalvikFinalStages,
     PrefixLoopsRejectSkippedRepeatedAndForeignStores) {
  for (unsigned Mutation = 0; Mutation < 14; ++Mutation)
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input = prefixLoopFinalClasses(false, false, false, false, true);
      auto &M = Input[0].methods[0];
      auto &C = M.instructions;
      switch (Mutation) {
      case 0:
        C[9] = op(9, "nop");
        break;
      case 1:
        C[11] = op(11, "nop");
        break;
      case 2:
        C[12].target = 9;
        break;
      case 3:
        C[4].target = 17;
        break;
      case 4:
        C[9].registers[1] = 9;
        break;
      case 5:
        C[9].opcode = "iput";
        break;
      case 6:
        C[11].reference = Input[0].fields[0].reference;
        break;
      case 7:
        C[14] = op(14, "return-void");
        break;
      case 8:
        C[16].reference = Input[0].fields[0].reference;
        break;
      case 9:
        C[6].target = 1000;
        break;
      case 10:
        C[9].reference =
            FieldRef{"Lfixture/Foreign;", "left", "[Ljava/lang/Object;"};
        break;
      case 11:
        C[9].literal = int64_t(1);
        break;
      case 12:
        C[6].target = 2;
        break;
      case 13:
        C[17] = op(17, "goto");
        C[17].target = 4;
        break;
      }
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}
