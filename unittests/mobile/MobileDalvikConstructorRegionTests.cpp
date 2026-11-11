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
std::vector<Class> fixture(bool Nested, bool Frame, bool External,
                           unsigned Reuse = 0) {
  const std::string Core = "Lfixture/Core;", A = "Lfixture/First;",
                    B = "Lfixture/Second;", Context = "Lfixture/Context;",
                    Object = "Ljava/lang/Object;";
  MethodRef Super{Object, "<init>", {}, "V"};
  MethodRef Base{Core, "<init>", {Object, B, Context}, "V"};
  MethodRef AI{A, "<init>", {"J", Object}, "V"},
      BI{B, "<init>", {"I", Object}, "V"};
  MethodRef Left{Context, "left", {Object}, Object},
      Right{Context, "right", {Object}, Object};
  auto ACtor =
      method(AI, {"public", "constructor"}, 4,
             {op(0, "invoke-direct", {0}, Super), op(1, "return-void")});
  auto BCtor =
      method(BI, {"public", "constructor"}, 3,
             {op(0, "invoke-direct", {0}, Super), op(1, "return-void")});
  if (External)
    for (auto *M : {&ACtor, &BCtor}) {
      M->instructions.clear();
      M->registers = 0;
      M->code_end = 0;
    }
  auto Primary =
      method(Base, {"private", "constructor"}, 4,
             {op(0, "invoke-direct", {0}, Super),
              op(1, "iput-object", {1, 0}, FieldRef{Core, "first", Object}),
              op(2, "iput-object", {2, 0}, FieldRef{Core, "second", B}),
              op(3, "iput-object", {3, 0}, FieldRef{Core, "context", Context}),
              op(4, "return-void")});
  std::vector<Instruction> C;
  MethodRef Entry{Core, "<init>",
                  Nested ? std::vector<std::string>{Object, Context, "J"}
                         : std::vector<std::string>{"J", "I", Context, Object},
                  "V"};
  if (!Nested) {
    C = {op(0, "new-instance", {0}, A),
         op(1, "move-wide/from16", {1, 21}),
         op(2, "if-eqz", {24}),
         op(3, "invoke-interface/range", {24, 25}, Left),
         op(4, "move-result-object", {3}),
         op(5, "goto"),
         op(6, "const/4", {3}),
         op(7, "invoke-direct/range", {0, 1, 2, 3}, AI),
         op(8, "new-instance", {4}, B),
         op(9, "move/from16", {5, 23}),
         op(10, "if-eqz", {24}),
         op(11, "invoke-interface/range", {24, 25}, Right),
         op(12, "move-result-object", {6}),
         op(13, "goto"),
         op(14, "const/4", {6}),
         op(15, "invoke-direct/range", {4, 5, 6}, BI),
         op(16, "move-object/from16", {10, 20}),
         op(17, "move-object", {11, 0}),
         op(18, "move-object", {12, 4}),
         op(19, "move-object/from16", {13, 24}),
         op(20, "invoke-direct/range", {10, 11, 12, 13}, Base),
         op(21, "return-void")};
    C[2].target = 6;
    C[5].target = 7;
    C[10].target = 14;
    C[13].target = 15;
    C[6].literal = int64_t(0);
    C[14].literal = int64_t(0);
  } else {
    C = {op(0, "new-instance", {0}, Core),
         op(1, "const/4", {1}),
         op(2, "const/4", {2}),
         op(3, "move-object/from16", {3, 22}),
         op(4, "invoke-direct/range", {0, 1, 2, 3}, Base),
         op(5, "new-instance", {4}, B),
         op(6, "long-to-int", {5, 23}),
         op(7, "if-eqz", {22}),
         op(8, "invoke-interface/range", {22, 23}, Right),
         op(9, "move-result-object", {6}),
         op(10, "goto"),
         op(11, "const/4", {6}),
         op(12, "invoke-direct/range", {4, 5, 6}, BI),
         op(13, "move-object/from16", {10, 20}),
         op(14, "move-object", {11, 0}),
         op(15, "move-object", {12, 4}),
         op(16, "move-object/from16", {13, 22}),
         op(17, "invoke-direct/range", {10, 11, 12, 13}, Base),
         op(18, "return-void")};
    C[1].literal = int64_t(0);
    C[2].literal = int64_t(0);
    C[7].target = 11;
    C[10].target = 12;
    C[11].literal = int64_t(0);
    for (auto &I : C) {
      if (I.pc >= 7)
        ++I.pc;
      if (I.target && *I.target >= 7)
        ++*I.target;
    }
    C.insert(C.begin() + 7, op(7, "move-object/from16", {23, 21}));
  }
  if (Reuse && !Nested) {
    const unsigned Destination = Reuse == 1 ? 21 : 22;
    C[8].registers = {Destination};
    C[9].registers[0] = Destination + 1;
    C[12].registers[0] = Destination + 2;
    C[14].registers[0] = Destination + 2;
    C[15].registers = {Destination, Destination + 1, Destination + 2};
    C[18].registers[1] = Destination;
    if (Reuse == 2) {
      C[10].registers = {7};
      C[11].registers = {7, 8};
      C[19].registers[1] = 7;
      for (auto &I : C) {
        if (I.pc >= 8)
          I.pc += 2;
        if (I.target && *I.target >= 8)
          *I.target += 2;
      }
      C.insert(C.begin() + 8, {op(8, "move-object/from16", {7, 24}),
                               op(9, "move-object/from16", {8, 25})});
    }
  }
  auto Caller = method(Entry, {"public", "constructor"}, Nested ? 25 : 26, C);
  if (Frame) {
    Caller.registers += 3;
    for (auto &I : Caller.instructions)
      for (auto &R : I.registers)
        R += 3;
  }
  auto Owner = klass(Core, {Primary, Caller});
  Owner.fields = {
      Field{FieldRef{Core, "first", Object}, {"public", "final"}},
      Field{FieldRef{Core, "second", B}, {"public", "final"}},
      Field{FieldRef{Core, "context", Context}, {"public", "final"}}};
  auto Provider = klass(Context,
                        {method(Left, {"public", "abstract"}, 0),
                         method(Right, {"public", "abstract"}, 0)},
                        true);
  Provider.access = {"public", "interface", "abstract"};
  auto Root =
      klass(Object, {method(Super, {"public", "constructor"}, 0)}, true);
  Root.access = {"public"};
  Root.superclass.reset();
  return {Owner, klass(A, {ACtor}, External), klass(B, {BCtor}, External),
          Provider, Root};
}
std::vector<Class> arrayFixture(std::string Type, bool Constant, bool Frame,
                                bool External, bool Primary) {
  const std::string Core = "Lfixture/Core;", Parcel = "Lfixture/Parcel;",
                    Base = "Lfixture/Base;", Object = "Ljava/lang/Object;";
  MethodRef Super{Object, "<init>", {}, "V"};
  MethodRef Target{Primary ? Base : Core, "<init>", {Parcel}, "V"};
  MethodRef Init{Parcel, "<init>", {Type}, "V"};
  auto Delegate = method(
      Target, {Primary ? "public" : "private", "constructor"}, 2,
      {op(0, "invoke-direct", {0}, Super),
       op(1, "iput-object", {1, 0}, FieldRef{Target.owner, "parcel", Parcel}),
       op(2, "return-void")});
  auto Construct =
      method(Init, {"public", "constructor"}, 2,
             {op(0, "invoke-direct", {0}, Super),
              op(1, "iput-object", {1, 0}, FieldRef{Parcel, "values", Type}),
              op(2, "return-void")});
  const unsigned Self = Frame ? 18 : 3;
  std::vector<Instruction> Code{op(0, "new-instance", {0}, Parcel)};
  if (Constant) {
    Code.push_back(op(1, "const/4", {1}));
    Code.back().literal = int64_t(0);
  }
  Code.push_back(
      op(Code.size(), "new-array", {1, Constant ? 1 : Self + 1}, Type));
  Code.push_back(op(Code.size(),
                    Frame ? "invoke-direct/range" : "invoke-direct", {0, 1},
                    Init));
  if (Frame) {
    Code.push_back(op(Code.size(), "move-object/from16", {3, Self}));
    Code.push_back(op(Code.size(), "move-object", {4, 0}));
  }
  Code.push_back(
      op(Code.size(), Frame ? "invoke-direct/range" : "invoke-direct",
         Frame ? std::vector<unsigned>{3, 4} : std::vector<unsigned>{Self, 0},
         Target));
  Code.push_back(op(Code.size(), "return-void"));
  auto Entry = method({Core, "<init>", {"I"}, "V"}, {"public", "constructor"},
                      Self + 2, std::move(Code));
  auto Owner = klass(Core, Primary ? std::vector<Method>{Entry}
                                   : std::vector<Method>{Delegate, Entry});
  auto Parent = klass(
      Base, Primary ? std::vector<Method>{Delegate} : std::vector<Method>{},
      External);
  Parent.access = {"public"};
  auto &Stored = Primary ? Parent : Owner;
  Stored.fields = {
      {FieldRef{Target.owner, "parcel", Parcel}, {"public", "final"}, {}}};
  if (Primary)
    Owner.superclass = Base;
  auto Allocated = klass(Parcel, {Construct}, External);
  Allocated.fields = {
      {FieldRef{Parcel, "values", Type}, {"public", "final"}, {}}};
  if (External)
    for (auto *C : {&Parent, &Allocated})
      for (auto &M : C->methods) {
        M.instructions.clear();
        M.registers = 0;
        M.code_end = 0;
      }
  auto Root =
      klass(Object, {method(Super, {"public", "constructor"}, 0)}, true);
  Root.access = {"public"};
  Root.superclass.reset();
  return {Owner, Parent, Allocated, Root};
}
std::vector<Class> defaultArrayFixture(std::string Type, bool Conditional,
                                       bool Frame, bool External,
                                       bool Synthetic) {
  const std::string Core = "Lfixture/Core;", Result = "Lfixture/Result;",
                    Factory = "Lfixture/Factory;",
                    Object = "Ljava/lang/Object;";
  MethodRef Super{Object, "<init>", {}, "V"};
  MethodRef Target{Core, "<init>", {Result}, "V"};
  MethodRef Make{Factory, "make", {Type}, Result};
  auto Primary =
      method(Target, {"private", "constructor"}, 2,
             {op(0, "invoke-direct", {0}, Super),
              op(1, "iput-object", {1, 0}, FieldRef{Core, "value", Result}),
              op(2, "return-void")});
  const unsigned Self = Frame ? 20 : 4;
  std::vector<Instruction> Code;
  size_t Branch = 0, Jump = 0;
  if (Conditional) {
    Code.push_back(op(0, "and-int/lit8", {0, Self + 4}));
    Code.back().literal = int64_t(1);
    Branch = Code.size();
    Code.push_back(op(Code.size(), "if-eqz", {0}));
  }
  Code.push_back(op(Code.size(), "new-array", {0, Self + 3}, Type));
  if (Frame) {
    Code.push_back(op(Code.size(), "move-object/from16", {10, Self + 2}));
    Code.push_back(op(Code.size(), "move-object", {11, 0}));
  }
  Code.push_back(op(Code.size(),
                    Frame ? "invoke-interface/range" : "invoke-interface",
                    Frame ? std::vector<unsigned>{10, 11}
                          : std::vector<unsigned>{Self + 2, 0},
                    Make));
  Code.push_back(op(Code.size(), "move-result-object", {1}));
  if (Conditional) {
    Jump = Code.size();
    Code.push_back(op(Code.size(), "goto"));
    Code[Branch].target = Code.size();
    Code.push_back(op(Code.size(), "move-object/from16", {1, Self + 1}));
    Code[Jump].target = Code.size();
  }
  if (Frame) {
    Code.push_back(op(Code.size(), "move-object/from16", {12, Self}));
    Code.push_back(op(Code.size(), "move-object", {13, 1}));
  }
  Code.push_back(
      op(Code.size(), Frame ? "invoke-direct/range" : "invoke-direct",
         Frame ? std::vector<unsigned>{12, 13} : std::vector<unsigned>{Self, 1},
         Target));
  Code.push_back(op(Code.size(), "return-void"));
  auto Entry = method({Core, "<init>", {Result, Factory, "I", "I"}, "V"},
                      {"public", "constructor"}, Self + 5, std::move(Code));
  if (Synthetic)
    Entry.access.insert("synthetic");
  auto Owner = klass(Core, {Primary, Entry});
  Owner.fields = {{{Core, "value", Result}, {"public", "final"}, {}}};
  auto Provider =
      klass(Factory, {method(Make, {"public", "abstract"}, 0)}, External);
  Provider.access = {"public", "interface", "abstract"};
  auto Root =
      klass(Object, {method(Super, {"public", "constructor"}, 0)}, true);
  Root.access = {"public"};
  Root.superclass.reset();
  return {Owner, Provider, klass(Result, {}, External), Root};
}
std::vector<Class> arrayStoreFixture(std::string Type, bool Allocate,
                                     bool Conditional, bool Frame,
                                     bool External) {
  auto Input = defaultArrayFixture(Type, Conditional, Frame, External, false);
  auto &M = Input[0].methods[1];
  M.reference.parameters.push_back(Type);
  ++M.registers;
  const unsigned Self = M.registers - M.incomingWords();
  auto &Code = M.instructions;
  const auto Found = std::find_if(Code.begin(), Code.end(), [](const auto &I) {
    return I.opcode == "new-array";
  });
  const size_t At = Found - Code.begin();
  const unsigned PC = Found->pc;
  if (!Allocate)
    *Found = op(PC, "move-object/from16", {0, Self + 5});
  for (auto &I : Code) {
    if (I.pc > PC)
      I.pc += 2;
    if (I.target && *I.target > PC)
      *I.target += 2;
  }
  auto Index = op(PC + 1, "const/4", {2});
  Index.literal = int64_t(0);
  Code.insert(Code.begin() + At + 1,
              {Index, op(PC + 2, "aput-object", {Self + 1, 0, 2})});
  M.code_end += 2;
  return Input;
}

std::vector<Class> pairedFixture(std::string Relation, bool Reference,
                                 bool Frame, bool External, bool Allocate,
                                 bool Primary = false, bool Loop = false) {
  const std::string Core = "Lfixture/Core;", Base = "Lfixture/Base;",
                    Parcel = "Lfixture/Parcel;", Factory = "Lfixture/Factory;",
                    Object = "Ljava/lang/Object;";
  MethodRef Super{Object, "<init>", {}, "V"};
  MethodRef Target{Primary ? Base : Core, "<init>", {Object}, "V"};
  MethodRef Init{Parcel, "<init>", {Object}, "V"};
  MethodRef Step{Factory, "step", {Object, "I"}, Object};
  auto Delegate = method(
      Target, {Primary ? "public" : "private", "constructor"}, 2,
      {op(0, "invoke-direct", {0}, Super),
       op(1, "iput-object", {1, 0}, FieldRef{Target.owner, "value", Object}),
       op(2, "return-void")});
  auto Construct =
      method(Init, {"public", "constructor"}, 2,
             {op(0, "invoke-direct", {0}, Super), op(1, "return-void")});
  const unsigned Self = Frame ? 20 : 8;
  std::vector<Instruction> C;
  if (Allocate)
    C.push_back(op(C.size(), "new-instance", {2}, Parcel));
  C.push_back(op(C.size(), "move-object/from16", {0, Self + 4}));
  C.push_back(op(C.size(), "move/from16", {1, Self + 2}));
  const size_t Branch = C.size();
  C.push_back(op(C.size(), Relation,
                 Reference ? std::vector<unsigned>{Self + 4, Self + 5}
                           : std::vector<unsigned>{1, Self + 3}));
  C.push_back(op(C.size(), "move-object/from16", {3, Self + 1}));
  C.push_back(op(C.size(), "move-object", {4, 0}));
  C.push_back(op(C.size(), "move", {5, 1}));
  C.push_back(op(C.size(),
                 Frame ? "invoke-interface/range" : "invoke-interface",
                 {3, 4, 5}, Step));
  C.push_back(op(C.size(), "move-result-object", {0}));
  if (Loop) {
    C.push_back(op(C.size(), "add-int/lit8", {1, 1}));
    C.back().literal = int64_t(1);
    C.push_back(op(C.size(), "goto"));
    C.back().target = C[Branch].pc;
  }
  C[Branch].target = C.size();
  if (Allocate)
    C.push_back(op(C.size(), "invoke-direct", {2, 0}, Init));
  if (Frame) {
    C.push_back(op(C.size(), "move-object/from16", {6, Self}));
    C.push_back(op(C.size(), "move-object", {7, Allocate ? 2u : 0u}));
  }
  C.push_back(op(C.size(), Frame ? "invoke-direct/range" : "invoke-direct",
                 Frame ? std::vector<unsigned>{6, 7}
                       : std::vector<unsigned>{Self, Allocate ? 2u : 0u},
                 Target));
  C.push_back(op(C.size(), "return-void"));
  auto Entry =
      method({Core, "<init>", {Factory, "I", "I", Object, Object}, "V"},
             {"public", "constructor"}, Self + 6, std::move(C));
  auto Owner = klass(Core, Primary ? std::vector<Method>{Entry}
                                   : std::vector<Method>{Delegate, Entry});
  if (Primary)
    Owner.superclass = Base;
  else
    Owner.fields = {{{Core, "value", Object}, {"public", "final"}, {}}};
  auto Provider =
      klass(Factory, {method(Step, {"public", "abstract"}, 0)}, true);
  Provider.access = {"public", "interface", "abstract"};
  auto Allocated = klass(Parcel, {Construct}, External);
  if (External)
    for (auto &M : Allocated.methods) {
      M.instructions.clear();
      M.registers = 0;
      M.code_end = 0;
    }
  auto Root =
      klass(Object, {method(Super, {"public", "constructor"}, 0)}, true);
  Root.access = {"public"};
  Root.superclass.reset();
  std::vector<Class> Input{Owner, Provider, Allocated, Root};
  if (Primary) {
    auto Parent = klass(Base, {Delegate}, External);
    Parent.access = {"public"};
    Parent.fields = {{{Base, "value", Object}, {"public", "final"}, {}}};
    if (External)
      for (auto &M : Parent.methods) {
        M.instructions.clear();
        M.registers = 0;
        M.code_end = 0;
      }
    Input.push_back(std::move(Parent));
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
TEST(MobileDalvikConstructorRegions,
     ConditionalArgumentsAndSameClassAllocationsKeepThisSite) {
  for (bool Nested : {false, true})
    for (unsigned Reuse : {0u, 1u, 2u})
      for (bool Frame : {false, true})
        for (bool External : {false, true})
          for (unsigned Route = 0; Route < 3; ++Route) {
            if (Nested && Reuse)
              continue;
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{Nested, Reuse, Frame, External, Route}));
            Budget B;
            auto R = recoverJava(
                classes(fixture(Nested, Frame, External, Reuse), Route, B), B);
            EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
            EXPECT_EQ(R.getInteger("minimum_java_release"), 25);
            EXPECT_EQ(R.getInteger("projected_method_count").value_or(0), 0);
            EXPECT_FALSE(R.getArray("generic_signature_projections"));
          }
}
TEST(MobileDalvikConstructorRegions,
     UninitializedThisAndOpenArgumentRegionsFail) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 21; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = fixture(false, false, false);
      auto &M = Input[0].methods[1];
      auto &C = M.instructions;
      if (Mutation == 0)
        C[2].registers = {20};
      else if (Mutation == 1)
        C[3].registers[0] = 20;
      else if (Mutation == 2)
        C[3].registers[1] = 20;
      else if (Mutation == 3)
        C[7].registers[2] = 4;
      else if (Mutation == 4)
        C[6].registers = {0};
      else if (Mutation == 5)
        C[5].target = 16;
      else if (Mutation == 6)
        C[2].target = 8;
      else if (Mutation == 7)
        C[13].target = 10;
      else if (Mutation == 8)
        C[20].registers[0] = 11;
      else if (Mutation == 9)
        C[17].registers[1] = 20;
      else if (Mutation == 10)
        C[16].registers[1] = 25;
      else if (Mutation == 11)
        std::get<MethodRef>(C[7].reference).owner = "Lfixture/Second;";
      else if (Mutation == 12)
        C[3].opcode = "invoke-virtual";
      else if (Mutation == 13)
        Input[3].methods[0].access = {"private", "abstract"};
      else if (Mutation == 14)
        M.code_end = 2;
      else if (Mutation == 15) {
        for (auto &I : C) {
          ++I.pc;
          if (I.target)
            ++*I.target;
        }
        auto Jump = op(0, "goto");
        Jump.target = 5;
        C.insert(C.begin(), Jump);
        ++M.code_end;
      } else if (Mutation == 16) {
        C[6].opcode = "const-wide/16";
      } else if (Mutation == 17)
        C[19].registers[1] = 10;
      else if (Mutation == 18)
        C[5].target = 2;
      else if (Mutation == 19)
        C[1].registers[1] = 20;
      else
        std::get<MethodRef>(C[7].reference).parameters[0] = "I";
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}
TEST(MobileDalvikConstructorRegions,
     LinearArrayArgumentsUseTheSharedAllocationVerifier) {
  for (const auto *Type :
       {"[Z", "[B", "[C", "[S", "[I", "[J", "[F", "[D", "[Ljava/lang/Object;",
        "[[I", "[[Ljava/lang/String;"})
    for (bool Constant : {false, true})
      for (bool Frame : {false, true})
        for (bool External : {false, true})
          for (bool Primary : {false, true})
            for (unsigned Route = 0; Route < 3; ++Route) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{Type, Constant, Frame, External, Primary, Route}));
              Budget B;
              auto Input =
                  arrayFixture(Type, Constant, Frame, External, Primary);
              const auto &M = Input[0].methods[Primary ? 0 : 1];
              const auto Prefix = selectConstructorPrefixDelegation(M, B);
              ASSERT_TRUE(Prefix);
              EXPECT_TRUE(Prefix->requires_dispatch);
              EXPECT_TRUE(std::none_of(
                  M.instructions.begin(), M.instructions.end(),
                  [](const Instruction &I) { return I.target.has_value(); }));
              EXPECT_FALSE(constructorPrefixAllocationArgumentsEnd(M, 0, B));
              llvm::json::Object R;
              ASSERT_NO_THROW(
                  R = recoverJava(classes(std::move(Input), Route, B), B));
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(R.getInteger("minimum_java_release"), 25);
              EXPECT_EQ(R.getInteger("projected_method_count").value_or(0), 0);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
            }
}
TEST(MobileDalvikConstructorRegions,
     LinearArrayArgumentsKeepInitializedContinuationWords) {
  for (unsigned Route = 0; Route < 3; ++Route) {
    auto Input = arrayFixture("[I", false, false, false, false);
    auto &M = Input[0].methods[1];
    auto &Code = M.instructions;
    Code.back().pc = 5;
    auto Store = op(4, "const/4", {2});
    Store.literal = int64_t(1);
    Code.insert(Code.end() - 1, Store);
    ++M.code_end;
    Budget B;
    auto Result = recoverJava(classes(std::move(Input), Route, B), B);
    EXPECT_EQ(Result.getInteger("unrecovered_method_count"), 0);
    EXPECT_EQ(Result.getInteger("minimum_java_release"), 25);
  }
}
TEST(MobileDalvikConstructorRegions,
     LinearArrayArgumentsRejectBrokenFramesAndOpenRegions) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation :
         {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 15u}) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = arrayFixture("[I", false, false, false, false);
      auto &M = Input[0].methods[1];
      auto &C = M.instructions;
      if (Mutation == 0)
        C[0].registers = {3};
      else if (Mutation == 1)
        C[1].registers[0] = 3;
      else if (Mutation == 2)
        C[1].registers[1] = 3;
      else if (Mutation == 3)
        C[1].reference = std::string("I");
      else if (Mutation == 4)
        C[1].registers.push_back(0);
      else if (Mutation == 5)
        C[1].literal = int64_t(1);
      else if (Mutation == 6)
        C[1].target = 3;
      else if (Mutation == 7)
        std::get<MethodRef>(C[2].reference).parameters[0] = "I";
      else if (Mutation == 8)
        C[1].reference = std::string("[Ljava/lang/String;");
      else if (Mutation == 9)
        C[1].registers[1] = 2;
      else if (Mutation == 10)
        C[1].registers[0] = 0;
      else if (Mutation == 11)
        C[1].data.push_back(1);
      else if (Mutation == 12)
        C[2].registers[0] = 1;
      else if (Mutation == 13) {
        for (auto &I : C)
          ++I.pc;
        auto Jump = op(0, "goto");
        Jump.target = 2;
        C.insert(C.begin(), Jump);
        ++M.code_end;
      } else
        C[1].keys.push_back(1);
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}
TEST(MobileDalvikConstructorRegions, OrdinaryArrayCreationKeepsExactOperands) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 11; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto M = method({"Lfixture/Factory;", "create", {"I"}, "[I"},
                      {"public", "static"}, 2,
                      {op(0, "new-array", {0, 1}, std::string("[I")),
                       op(1, "return-object", {0})});
      auto &I = M.instructions[0];
      if (Mutation == 1)
        I.literal = int64_t(1);
      else if (Mutation == 2)
        I.target = 1;
      else if (Mutation == 3)
        I.targets.push_back(1);
      else if (Mutation == 4)
        I.keys.push_back(1);
      else if (Mutation == 5)
        I.data.push_back(1);
      else if (Mutation == 6)
        I.element_width = 1;
      else if (Mutation == 7)
        I.registers.push_back(1);
      else if (Mutation == 8)
        I.reference = std::monostate{};
      else if (Mutation == 9)
        I.reference = std::string("I");
      else if (Mutation == 10)
        I.reference = std::string("[V");
      Budget B;
      auto Input = classes({klass("Lfixture/Factory;", {M})}, Route, B);
      if (!Mutation) {
        auto R = recoverJava(Input, B);
        EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
      } else
        EXPECT_THROW(recoverJava(Input, B), std::runtime_error);
    }
}
TEST(MobileDalvikConstructorRegions,
     DefaultFieldsDoNotNarrowTheLinearSuperclassFrame) {
  for (bool External : {false, true})
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{External, Route}));
      const std::string Core = "Lfixture/Core;", Base = "Lfixture/Base;",
                        Left = "Lfixture/Left;", Right = "Lfixture/Right;",
                        Object = "Ljava/lang/Object;";
      MethodRef Super{Object, "<init>", {}, "V"};
      MethodRef Target{Base, "<init>", {Left}, "V"};
      MethodRef Use{"Lfixture/Use;", "accept", {Right}, "V"};
      auto Parent = klass(
          Base,
          {method(Target, {"public", "constructor"}, 2,
                  {op(0, "invoke-direct", {0}, Super), op(1, "return-void")})},
          External);
      Parent.access = {"public"};
      auto Consumer =
          klass(Use.owner,
                {method(Use, {"public", "static"}, 1, {op(0, "return-void")})},
                External);
      if (External)
        for (auto *C : {&Parent, &Consumer})
          for (auto &M : C->methods) {
            M.instructions.clear();
            M.registers = 0;
            M.code_end = 0;
          }
      auto Entry =
          method({Core, "<init>", {}, "V"}, {"public", "constructor"}, 2,
                 {op(0, "const/4", {0}), op(1, "invoke-direct", {1, 0}, Target),
                  op(2, "invoke-static", {0}, Use), op(3, "return-void")});
      Entry.instructions[0].literal = int64_t(0);
      auto Owner = klass(Core, {Entry});
      Owner.superclass = Base;
      Owner.fields = {{{Core, "unused", "I"}, {"public", "final"}, {}}};
      auto L = klass(Left), R = klass(Right);
      L.access = R.access = {"public", "interface", "abstract"};
      auto Root =
          klass(Object, {method(Super, {"public", "constructor"}, 0)}, true);
      Root.access = {"public"};
      Root.superclass.reset();
      Budget B;
      auto Input = classes({Owner, Parent, Consumer, L, R, Root}, Route, B);
      ConstructorSuperPrologues Prologues;
      planConstructorDelegations(Input, B, {}, &Prologues);
      ASSERT_TRUE(Prologues.contains(Entry.reference));
      EXPECT_FALSE(Prologues.at(Entry.reference).requires_dispatch);
      auto Recovered = recoverJava(Input, B);
      EXPECT_EQ(Recovered.getInteger("unrecovered_method_count"), 0);
      EXPECT_EQ(Recovered.getInteger("minimum_java_release"), 25);
    }
}
TEST(MobileDalvikConstructorRegions,
     ArrayFactoryDefaultsRetainTheirDelegatedFinalInitialization) {
  for (const auto *Type :
       {"[Z", "[B", "[C", "[S", "[I", "[J", "[F", "[D", "[Ljava/lang/Object;",
        "[[I", "[[Ljava/lang/String;"})
    for (bool Conditional : {false, true})
      for (bool Frame : {false, true})
        for (bool External : {false, true})
          for (bool Synthetic : {false, true})
            for (unsigned Route = 0; Route < 3; ++Route) {
              SCOPED_TRACE(::testing::PrintToString(std::tuple{
                  Type, Conditional, Frame, External, Synthetic, Route}));
              auto Input = defaultArrayFixture(Type, Conditional, Frame,
                                               External, Synthetic);
              const auto Entry = Input[0].methods[1].reference;
              Budget B;
              auto Map = classes(std::move(Input), Route, B);
              const auto Edges = planConstructorDelegations(Map, B, {});
              EXPECT_TRUE(Edges.contains(Entry));
              llvm::json::Object R;
              ASSERT_NO_THROW(R = recoverJava(Map, B));
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(R.getInteger("minimum_java_release"), 25);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
            }
}
TEST(MobileDalvikConstructorRegions,
     ArrayFactoryDefaultsRejectUninitializedAndIncompletePaths) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 14; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = defaultArrayFixture("[I", true, false, false, false);
      auto &M = Input[0].methods[1];
      auto &C = M.instructions;
      if (Mutation == 0)
        C[2].registers[0] = 4;
      else if (Mutation == 1)
        C[2].registers[1] = 4;
      else if (Mutation == 2)
        C[2].literal = int64_t(1);
      else if (Mutation == 3)
        C[2].reference = std::string("I");
      else if (Mutation == 4)
        C[2].registers.push_back(1);
      else if (Mutation == 5)
        C[3].registers[0] = 4;
      else if (Mutation == 6)
        C[1].target = C[3].pc;
      else if (Mutation == 7)
        C[1].target = C.back().pc;
      else if (Mutation == 8)
        C[2].reference = std::string("[J");
      else if (Mutation == 9)
        C[2].element_width = 1;
      else if (Mutation == 10)
        C[4].opcode = "move-result";
      else if (Mutation == 11)
        C[2].registers[1] = M.registers;
      else if (Mutation == 12)
        Input[1].methods[0].declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else
        std::get<MethodRef>(C[7].reference).owner = "Lfixture/Other;";
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}
TEST(MobileDalvikConstructorRegions,
     ReferenceArrayStoresRetainConstructorArgumentEffects) {
  for (const auto *Type : {"[Ljava/lang/Object;", "[Ljava/lang/String;", "[[I",
                           "[[Ljava/lang/String;"})
    for (bool Allocate : {false, true})
      for (bool Conditional : {false, true})
        for (bool Frame : {false, true})
          for (bool External : {false, true})
            for (unsigned Route = 0; Route < 3; ++Route) {
              SCOPED_TRACE(::testing::PrintToString(std::tuple{
                  Type, Allocate, Conditional, Frame, External, Route}));
              auto Input = arrayStoreFixture(Type, Allocate, Conditional, Frame,
                                             External);
              const auto Entry = Input[0].methods[1].reference;
              Budget B;
              auto Map = classes(std::move(Input), Route, B);
              EXPECT_TRUE(
                  planConstructorDelegations(Map, B, {}).contains(Entry));
              llvm::json::Object R;
              ASSERT_NO_THROW(R = recoverJava(Map, B));
              EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(R.getInteger("minimum_java_release"), 25);
              EXPECT_FALSE(R.getArray("generic_signature_projections"));
            }
}
TEST(MobileDalvikConstructorRegions,
     ReferenceArrayStoresRejectMalformedAndUninitializedOperands) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 14; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input =
          arrayStoreFixture("[Ljava/lang/String;", false, true, false, false);
      auto &M = Input[0].methods[1];
      auto &C = M.instructions;
      auto &Store = *std::find_if(C.begin(), C.end(), [](const auto &I) {
        return I.opcode == "aput-object";
      });
      if (Mutation == 0)
        Store.registers[0] = 4;
      else if (Mutation == 1)
        Store.registers[1] = 4;
      else if (Mutation == 2)
        Store.registers[2] = 4;
      else if (Mutation == 3)
        Store.registers.pop_back();
      else if (Mutation == 4)
        Store.literal = int64_t(1);
      else if (Mutation == 5)
        Store.reference = std::string("Ljava/lang/Object;");
      else if (Mutation == 6)
        Store.target = Store.pc + 1;
      else if (Mutation == 7)
        Store.data = {1};
      else if (Mutation == 8)
        Store.element_width = 1;
      else if (Mutation == 9)
        Store.opcode = "aput";
      else if (Mutation == 10)
        Store.registers[2] = 3;
      else if (Mutation == 11)
        Store.registers[0] = M.registers;
      else if (Mutation == 12)
        Input[1].methods[0].declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else
        C[1].target = C.back().pc;
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}
TEST(MobileDalvikConstructorRegions, OrdinaryReferenceArraysKeepExactOperands) {
  for (bool Store : {false, true})
    for (unsigned Mutation = 0; Mutation < 8; ++Mutation)
      for (unsigned Route = 0; Route < 3; ++Route) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{Store, Mutation, Route}));
        auto I = op(0, Store ? "aput-object" : "aget-object",
                    Store ? std::vector<unsigned>{2, 0, 1}
                          : std::vector<unsigned>{0, 1, 2});
        if (Mutation == 1)
          I.literal = int64_t(1);
        else if (Mutation == 2)
          I.reference = std::string("Ljava/lang/Object;");
        else if (Mutation == 3)
          I.target = 1;
        else if (Mutation == 4)
          I.targets = {1};
        else if (Mutation == 5)
          I.keys = {1};
        else if (Mutation == 6)
          I.data = {1};
        else if (Mutation == 7)
          I.element_width = 1;
        auto M = method(
            {"Lfixture/Access;", "access",
             Store ? std::vector<std::string>{"[Ljava/lang/String;", "I",
                                              "Ljava/lang/Object;"}
                   : std::vector<std::string>{"[Ljava/lang/String;", "I"},
             Store ? "V" : "Ljava/lang/String;"},
            {"public", "static"}, 3,
            {I, Store ? op(1, "return-void") : op(1, "return-object", {0})});
        Budget B;
        auto Map = classes({klass("Lfixture/Access;", {M})}, Route, B);
        if (Mutation)
          EXPECT_THROW(recoverJava(Map, B), std::runtime_error);
        else {
          auto R = recoverJava(Map, B);
          EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
        }
      }
}

TEST(MobileDalvikConstructorRegions,
     PairedConditionsKeepDelegationAndAllocationRegions) {
  for (const std::string Relation :
       {"if-eq", "if-ne", "if-lt", "if-ge", "if-gt", "if-le"})
    for (bool Reference : {false, true}) {
      if (Reference && Relation != "if-eq" && Relation != "if-ne")
        continue;
      for (bool Frame : {false, true})
        for (bool External : {false, true})
          for (bool Allocate : {false, true})
            for (bool Primary : {false, true})
              for (unsigned Route = 0; Route != 3; ++Route) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{Relation, Reference, Frame, External, Allocate,
                               Primary, Route}));
                Budget B(Limits{});
                auto Input = pairedFixture(Relation, Reference, Frame, External,
                                           Allocate, Primary);
                const auto Entry = Input[0].methods.back().reference;
                auto Map = classes(std::move(Input), Route, B);
                ConstructorSuperPrologues Prologues;
                auto Edges = planConstructorDelegations(Map, B, {}, &Prologues);
                EXPECT_EQ(Edges.contains(Entry), !Primary);
                EXPECT_EQ(Prologues.contains(Entry), Primary);
                EXPECT_NO_THROW(recoverJava(Map, B));
              }
    }
  for (bool Frame : {false, true})
    for (bool External : {false, true})
      for (bool Primary : {false, true})
        for (unsigned Route = 0; Route != 3; ++Route) {
          Budget B(Limits{});
          auto Input = pairedFixture("if-ge", false, Frame, External, false,
                                     Primary, true);
          const auto Entry = Input[0].methods.back().reference;
          auto Map = classes(std::move(Input), Route, B);
          ConstructorSuperPrologues Prologues;
          auto Edges = planConstructorDelegations(Map, B, {}, &Prologues);
          EXPECT_EQ(Edges.contains(Entry), !Primary);
          EXPECT_EQ(Prologues.contains(Entry), Primary);
          EXPECT_NO_THROW(recoverJava(Map, B));
        }
}
TEST(MobileDalvikConstructorRegions,
     PairedConditionsRejectMalformedAndUninitializedInputs) {
  for (unsigned Mutation = 0; Mutation != 19; ++Mutation)
    for (unsigned Route = 0; Route != 3; ++Route) {
      Budget B(Limits{});
      auto Input = pairedFixture("if-ge", false, false, false, false);
      auto &M = Input[0].methods.back();
      auto &C = M.instructions;
      auto &I = C[2];
      const unsigned Self = M.registers - M.incomingWords();
      switch (Mutation) {
      case 0:
        I.registers[0] = Self;
        break;
      case 1:
        I.registers[1] = Self;
        break;
      case 2:
        I.registers.pop_back();
        break;
      case 3:
        I.registers.push_back(0);
        break;
      case 4:
        I.target = I.pc;
        break;
      case 5:
        I.target = M.code_end + 1;
        break;
      case 6:
        I.target = C[7].pc;
        break;
      case 7:
        I.literal = int64_t(0);
        break;
      case 8:
        I.reference = std::string("Ljava/lang/Object;");
        break;
      case 9:
        I.keys = {0};
        break;
      case 10:
        I.targets = {*I.target};
        break;
      case 11:
        I.data = {0};
        break;
      case 12:
        I.element_width = 1;
        break;
      case 13:
        I.target.reset();
        break;
      case 14:
        I.opcode = "if-unknown";
        break;
      case 15:
        I.registers = {Self + 4, Self + 5};
        break;
      case 16:
        M.reference.parameters[1] = "F";
        break;
      case 17:
        I.target = C.back().pc;
        break;
      case 18:
        I.registers[1] = M.registers;
        break;
      }
      auto Map = classes(std::move(Input), Route, B);
      EXPECT_ANY_THROW(recoverJava(Map, B)) << Mutation;
    }
}
TEST(MobileDalvikConstructorRegions, OrdinaryConditionsKeepExactOperands) {
  for (bool Zero : {false, true})
    for (unsigned Mutation = 0; Mutation != 9; ++Mutation)
      for (unsigned Route = 0; Route != 3; ++Route) {
        Method M = method(
            {"Lfixture/Core;", "choose", {"I", "I"}, "I"}, {"public", "static"},
            3,
            {op(0, Zero ? "if-eqz" : "if-eq",
                Zero ? std::vector<unsigned>{1} : std::vector<unsigned>{1, 2}),
             op(1, "const/4", {0}), op(2, "return", {0}), op(3, "const/4", {0}),
             op(4, "return", {0})});
        auto &I = M.instructions[0];
        I.target = 3;
        M.instructions[1].literal = int64_t(0);
        M.instructions[3].literal = int64_t(1);
        switch (Mutation) {
        case 1:
          I.literal = int64_t(0);
          break;
        case 2:
          I.reference = std::string("Ljava/lang/Object;");
          break;
        case 3:
          I.targets = {3};
          break;
        case 4:
          I.keys = {0};
          break;
        case 5:
          I.data = {0};
          break;
        case 6:
          I.element_width = 1;
          break;
        case 7:
          I.target.reset();
          break;
        case 8:
          I.registers.push_back(0);
          break;
        }
        Budget B(Limits{});
        auto Map = classes({klass("Lfixture/Core;", {M})}, Route, B);
        if (!Mutation)
          EXPECT_NO_THROW(recoverJava(Map, B));
        else
          EXPECT_ANY_THROW(recoverJava(Map, B)) << Mutation;
      }
}

namespace {
std::vector<Class> guardedZeroFixture(bool Frame, bool External, bool Copy) {
  const std::string Core = "Lfixture/Core;", Parent = "Lfixture/Parent;",
                    Token = "Lfixture/Token;", Child = "Lfixture/Child;",
                    Trace = "Lfixture/Trace;", Object = "Ljava/lang/Object;";
  MethodRef Root{Object, "<init>", {}, "V"},
      Super{Parent, "<init>", {"I", Token}, "V"},
      Seed{Trace, "seed", {"I"}, "I"},
      Accept{Trace, "accept", {Object, Child}, "V"},
      Finish{Trace, "finish", {Object}, "V"};
  const unsigned Self = Frame ? 20 : 3, Input = Self + 1, Alternate = Self + 2,
                 Null = Frame ? 14 : 1, Scalar = Frame ? 13 : 0,
                 Auxiliary = Frame ? 15 : 2, BodyInput = Frame ? 10 : Input,
                 BodySelf = Frame ? 12 : Self;
  std::vector<Instruction> Code{
      op(0, "if-eqz", {Alternate}),
      op(1, "const/16", {Scalar}),
      op(2, "goto"),
      op(3, "const/16", {Scalar}),
      op(4, "invoke-static", {Scalar}, Seed),
      op(5, "move-result", {Scalar}),
      op(6, "const/4", {Null}),
      op(7, Frame ? "invoke-direct/range" : "invoke-direct",
         Frame ? std::vector<unsigned>{12, 13, 14}
               : std::vector<unsigned>{Self, Scalar, Null},
         Super),
      op(8, "instance-of", {0, BodyInput}, Child),
      op(9, "if-eqz", {0}),
      op(10, "move-object/from16", {Null, BodyInput}),
      op(11, "check-cast", {Null}, Child),
      op(12, "move-object", {0, Null}),
      op(13, "if-eqz", {0}),
      op(14, "invoke-static", {BodySelf, 0}, Accept),
      op(15, "invoke-static", {BodySelf}, Finish),
      op(16, "return-void")};
  Code[0].target = 3;
  Code[2].target = 4;
  Code[9].target = 12;
  Code[13].target = 15;
  Code[1].literal = int64_t{17};
  Code[3].literal = int64_t{29};
  Code[6].literal = int64_t{0};
  auto insert = [&](unsigned At, Instruction I) {
    for (auto &Old : Code) {
      if (Old.pc >= At)
        ++Old.pc;
      if (Old.target && *Old.target >= At)
        ++*Old.target;
    }
    I.pc = At;
    Code.insert(Code.begin() + At, std::move(I));
  };
  if (Copy) {
    Code[6].registers = {Auxiliary};
    insert(7, op(0, "move-object", {Null, Auxiliary}));
  }
  if (Frame) {
    insert(Copy ? 8 : 7, op(0, "move-object/from16", {12, Self}));
    insert(Copy ? 10 : 9, op(0, "move-object/from16", {BodyInput, Input}));
  }
  auto C = klass(Core, {method({Core, "<init>", {Object, "Z", Token}, "V"},
                               {"public", "constructor"}, Self + 4, Code)});
  C.superclass = Parent;
  auto P =
      klass(Parent,
            {method(Super, {"public", "constructor"}, 3,
                    {op(0, "invoke-direct", {0}, Root), op(1, "return-void")})},
            External);
  P.access.erase("final");
  auto T =
      klass(Trace,
            {method(Seed, {"public", "static"}, 1, {op(0, "return", {0})}),
             method(Accept, {"public", "static"}, 2, {op(0, "return-void")}),
             method(Finish, {"public", "static"}, 1, {op(0, "return-void")})},
            External);
  if (External)
    for (auto *Owner : {&P, &T})
      for (auto &M : Owner->methods) {
        M.instructions.clear();
        M.registers = M.code_end = 0;
      }
  return {C, P, T, klass(Token, {}, External), klass(Child, {}, External)};
}
} // namespace

TEST(MobileDalvikConstructorRegions,
     GuardedSuperKeepsZeroAcrossReferenceJoins) {
  for (bool Frame : {false, true})
    for (bool External : {false, true})
      for (bool Copy : {false, true})
        for (unsigned Route = 0; Route < 3; ++Route) {
          auto Input = guardedZeroFixture(Frame, External, Copy);
          const auto Original = Input[0].methods[0].instructions;
          Budget B;
          auto Map = classes(std::move(Input), Route, B);
          auto R = recoverJava(Map, B);
          EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
          EXPECT_EQ(R.getInteger("minimum_java_release"), 25);
          EXPECT_EQ(Map.at("Lfixture/Core;").methods[0].instructions.size(),
                    Original.size());
        }
}

TEST(MobileDalvikConstructorRegions, GuardedSuperDoesNotInventNullReferences) {
  for (unsigned Mutation = 0; Mutation < 6; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(Mutation);
      auto Input = guardedZeroFixture(false, true, false);
      auto &Code = Input[0].methods[0].instructions;
      if (Mutation == 0) {
        Code[6] = op(6, "move-object", {1, 6});
      } else if (Mutation == 1) {
        Code[6].literal = int64_t{1};
      } else if (Mutation == 2) {
        Code[11] = op(11, "nop");
      } else if (Mutation == 3) {
        Code[11].reference = std::string("Lfixture/Token;");
      } else if (Mutation == 4) {
        Code[8] = op(8, "move-object", {1, 6});
      } else {
        Code[6] = op(6, "move", {1, 5});
      }
      Budget B;
      EXPECT_THROW(recoverJava(classes(std::move(Input), Route, B), B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> guardedDelegationTailClasses(bool External, bool Range,
                                                unsigned Saved) {
  const std::string Core = "Lfixture/Core;", Trace = "Lfixture/Trace;",
                    Object = "Ljava/lang/Object;",
                    Array = "[Ljava/lang/Object;";
  const std::string SavedType = Saved == 1 ? Object : Saved == 2 ? "F" : "I";
  MethodRef Root{Object, "<init>", {}, "V"}, Target{Core, "<init>", {"I"}, "V"},
      Seed{Trace, "seed", {"I"}, "I"},
      SavedCall{Trace, "saved", {SavedType}, "V"},
      Accept{Core, "accept", {Object}, "V"},
      Observe{Trace, "observe", {Core, Object}, "V"};
  std::vector<Instruction> Code{op(0, "const", {2}),
                                op(1, "if-eqz", {9}),
                                op(2, "invoke-static", {10}, Seed),
                                op(3, "move-result", {0}),
                                op(4, "goto"),
                                op(5, "const/16", {0}),
                                op(6, "nop"),
                                op(7, "add-int", {1, 0, 10}),
                                op(8, "invoke-direct", {8, 1}, Target),
                                op(9, "invoke-static", {2}, SavedCall),
                                op(10, "array-length", {3, 11}),
                                op(11, "const/4", {4}),
                                op(12, "if-ge", {4, 3}),
                                op(13, "aget-object", {5, 11, 4}),
                                op(14, "invoke-virtual", {8, 5}, Accept),
                                op(15, "add-int/lit8", {4, 4}),
                                op(16, "goto"),
                                op(17, "return-void")};
  Code[0].literal = int64_t(Saved == 1 ? 0 : Saved == 2 ? 0x3f800000 : 23);
  Code[1].target = 5;
  Code[4].target = 7;
  Code[5].literal = int64_t(37);
  Code[11].literal = int64_t(0);
  Code[12].target = 17;
  Code[15].literal = int64_t(1);
  Code[16].target = 12;
  if (Range) {
    for (auto &I : Code) {
      if (I.pc >= 8)
        I.pc += 2;
      if (I.target && *I.target >= 8)
        *I.target += 2;
    }
    Code[8].opcode = "invoke-direct/range";
    Code[8].registers = {6, 7};
    Code.insert(Code.begin() + 8,
                {op(8, "move-object", {6, 8}), op(9, "move", {7, 1})});
  }
  auto Primary = method(Target, {"private", "constructor"}, 2,
                        {op(0, "invoke-direct", {0}, Root),
                         op(1, "iput", {1, 0}, FieldRef{Core, "seed", "I"}),
                         op(2, "return-void")});
  auto Entry = method({Core, "<init>", {"Z", "I", Array}, "V"},
                      {"public", "constructor", "varargs"}, 12, Code);
  auto Owner = klass(Core, {Primary, Entry,
                            method(Accept, {"public"}, 2,
                                   {op(0, "invoke-static", {0, 1}, Observe),
                                    op(1, "return-void")})});
  Owner.access.erase("final");
  Owner.fields = {Field{FieldRef{Core, "seed", "I"}, {"public", "final"}}};
  auto Provider =
      klass(Trace,
            {method(Seed, {"public", "static"}, 1, {op(0, "return", {0})}),
             method(SavedCall, {"public", "static"}, 1, {op(0, "return-void")}),
             method(Observe, {"public", "static"}, 2, {op(0, "return-void")})},
            External);
  if (External)
    for (auto &M : Provider.methods) {
      M.instructions.clear();
      M.registers = M.code_end = 0;
    }
  auto ObjectClass =
      klass(Object, {method(Root, {"public", "constructor"}, 0)}, true);
  ObjectClass.access = {"public"};
  ObjectClass.superclass.reset();
  return {Owner, Provider, ObjectClass};
}
} // namespace

TEST(MobileDalvikConstructorRegions,
     GuardedThisDelegationKeepsContinuationBoundaryAndWords) {
  for (bool External : {false, true})
    for (bool Range : {false, true})
      for (unsigned Saved = 0; Saved < 3; ++Saved)
        for (unsigned Route = 0; Route < 3; ++Route) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{External, Range, Saved, Route}));
          auto Input = guardedDelegationTailClasses(External, Range, Saved);
          const auto Original = Input[0].methods[1].instructions;
          const auto Entry = Input[0].methods[1].reference;
          const auto Target = Input[0].methods[0].reference;
          Budget B;
          auto Map = classes(std::move(Input), Route, B);
          const auto Prefix = selectConstructorPrefixDelegation(
              Map.at(Entry.owner).methods[1], B);
          ASSERT_TRUE(Prefix);
          EXPECT_TRUE(Prefix->requires_dispatch);
          EXPECT_EQ(Prefix->call, Range ? 10 : 8);
          const auto Edges = planConstructorDelegations(Map, B, {});
          ASSERT_TRUE(Edges.contains(Entry));
          EXPECT_EQ(Edges.at(Entry), Target);
          auto Result = recoverJava(Map, B);
          EXPECT_EQ(Result.getInteger("unrecovered_method_count"), 0);
          EXPECT_EQ(Result.getInteger("minimum_java_release"), 25);
          EXPECT_EQ(Map.at(Entry.owner).methods[1].instructions.size(),
                    Original.size());
        }
}

TEST(MobileDalvikConstructorRegions,
     GuardedThisContinuationsRejectPrefixReentryAndUnprovedWords) {
  for (unsigned Mutation = 0; Mutation < 14; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input = guardedDelegationTailClasses(false, false, 0);
      auto &Code = Input[0].methods[1].instructions;
      if (Mutation == 0)
        Code[16].target = 1;
      else if (Mutation == 1)
        Code[16].target = 8;
      else if (Mutation == 2)
        Code[4].target = 9;
      else if (Mutation == 3)
        Code[12].target = 99;
      else if (Mutation == 4)
        Code[2].registers = {8};
      else if (Mutation == 5)
        Code[9] =
            op(9, "iput", {1, 8}, FieldRef{"Lfixture/Core;", "seed", "I"});
      else if (Mutation == 6)
        std::get<MethodRef>(Code[8].reference).parameters = {"J"};
      else if (Mutation == 7)
        Code[9] = op(9, "unsupported-tail");
      else if (Mutation == 8)
        Code[0].registers = {11};
      else if (Mutation == 9)
        Code[0] = op(0, "move-object", {2, 11});
      else if (Mutation == 10)
        std::get<MethodRef>(Code[9].reference).parameters = {
            "Ljava/lang/Object;"};
      else if (Mutation == 11)
        Code[9] = op(9, "invoke-direct", {8, 1}, Input[0].methods[0].reference);
      else if (Mutation == 12)
        Code[8].registers[0] = 11;
      else
        Code[9] = op(9, "return-void");
      Budget B;
      EXPECT_THROW(recoverJava(classes(std::move(Input), Route, B), B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> interfaceStaticPrefixClasses(const std::string &Type,
                                                bool External, bool Range,
                                                bool This, unsigned Scope) {
  const std::string Core = "Lfixture/Core;", Parent = "Lfixture/Parent;",
                    Ancestor = "Lfixture/Ancestor;",
                    Object = "Ljava/lang/Object;";
  const unsigned Words = width(Type);
  MethodRef Root{Object, "<init>", {}, "V"}, AI{Ancestor, "<init>", {}, "V"},
      PI{Parent, "<init>", {Type}, "V"}, CI{Core, "<init>", {Type}, "V"};
  std::vector<unsigned> Arguments{0, 1};
  if (Words == 2)
    Arguments.push_back(2);
  auto A =
      klass(Ancestor,
            {method(AI, {"public", "constructor"}, 1,
                    {op(0, "invoke-direct", {0}, Root), op(1, "return-void")})},
            External);
  A.access.erase("final");
  A.fields = {
      Field{FieldRef{Ancestor, "sharedValue", Type}, {"protected", "static"}}};
  auto P =
      klass(Parent,
            {method(PI, {"public", "constructor"}, Words + 1,
                    {op(0, "invoke-direct", {0}, AI), op(1, "return-void")})},
            External);
  P.access.erase("final");
  P.superclass = Ancestor;
  auto Primary =
      method(CI, {"public", "constructor"}, Words + 1,
             {op(0, "invoke-direct", Arguments, PI), op(1, "return-void")});
  const auto Suffix = Type.starts_with('L') || Type.starts_with('[') ? "-object"
                      : Words == 2                                   ? "-wide"
                      : Type == "Z" ? "-boolean"
                                    : "";
  Arguments = {Range ? 1u : 7u, 2};
  if (Words == 2)
    Arguments.push_back(3);
  auto Entry = method({Core, "<init>", {}, "V"}, {"public", "constructor"}, 8,
                      {op(0, std::string("sget") + Suffix, {2},
                          FieldRef{Core, "sharedValue", Type}),
                       Range ? op(1, "move-object", {1, 7}) : op(1, "nop"),
                       op(2, Range ? "invoke-direct/range" : "invoke-direct",
                          Arguments, This ? CI : PI),
                       op(3, "return-void")});
  auto C = klass(Core, {Primary, Entry});
  C.superclass = Parent;
  auto I = klass("Lfixture/Shared;", {}, External),
       L = klass("Lfixture/Left;", {}, External),
       R = klass("Lfixture/Right;", {}, External);
  for (auto *Node : {&I, &L, &R})
    Node->access = {"public", "abstract", "interface"};
  I.fields = {Field{FieldRef{I.name, "unrelated", "I"},
                    {"public", "static", "final"},
                    FieldValue(int64_t(5))}};
  L.interfaces = {I.name};
  R.interfaces = {I.name};
  if (Scope == 0)
    C.interfaces = {L.name, R.name};
  else if (Scope == 1)
    P.interfaces = {L.name, R.name};
  else {
    C.interfaces = {L.name};
    P.interfaces = {R.name};
  }
  if (External)
    for (auto *Owner : {&A, &P})
      for (auto &M : Owner->methods) {
        M.instructions.clear();
        M.registers = M.code_end = 0;
      }
  auto O = klass(Object, {method(Root, {"public", "constructor"}, 0)}, true);
  O.access = {"public"};
  O.superclass.reset();
  return {C, P, A, I, L, R, O};
}
} // namespace

TEST(MobileDalvikConstructorRegions,
     InheritedStaticPrefixesKeepKnownInterfaceScopes) {
  for (const std::string Type :
       {"I", "J", "F", "D", "Z", "Ljava/lang/Object;", "[I"})
    for (bool External : {false, true})
      for (bool Range : {false, true})
        for (bool This : {false, true})
          for (unsigned Scope = 0; Scope < 3; ++Scope)
            for (unsigned Route = 0; Route < 3; ++Route) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{Type, External, Range, This, Scope, Route}));
              auto Input = interfaceStaticPrefixClasses(Type, External, Range,
                                                        This, Scope);
              const auto Constructor = Input[0].methods[1];
              const auto Read = Constructor.instructions[0];
              Budget B;
              auto Map = classes(std::move(Input), Route, B);
              EXPECT_EQ(constructorPrefixStaticRead(Constructor, Read, Map, B),
                        std::get<FieldRef>(Read.reference));
              auto Result = recoverJava(Map, B);
              EXPECT_EQ(Result.getInteger("unrecovered_method_count"), 0);
              EXPECT_EQ(Result.getInteger("minimum_java_release"), 25);
            }
}

TEST(MobileDalvikConstructorRegions,
     InheritedStaticInterfacesRejectHiddenFieldsAndOpenGraphs) {
  for (unsigned Mutation = 0; Mutation < 10; ++Mutation)
    for (bool External : {false, true})
      for (unsigned Route = 0; Route < 3; ++Route) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{Mutation, External, Route}));
        auto Input = interfaceStaticPrefixClasses("J", External, true, true, 2);
        if (Mutation == 0)
          Input[3].fields.push_back(
              Field{FieldRef{Input[3].name, "sharedValue", "J"},
                    {"public", "static", "final"},
                    FieldValue(int64_t(1))});
        else if (Mutation == 1)
          Input[4].fields.push_back(
              Field{FieldRef{Input[4].name, "sharedValue", "I"},
                    {"public", "static", "final"},
                    FieldValue(int64_t(1))});
        else if (Mutation == 2)
          Input[4].interfaces = {"Lfixture/Missing;"};
        else if (Mutation == 3)
          Input[4].interfaces = {Input[4].name};
        else if (Mutation == 4)
          Input[3].interfaces = {Input[5].name};
        else if (Mutation == 5)
          Input[4].access = {"public"};
        else if (Mutation == 6)
          Input[2].fields[0].access = {"private", "static"};
        else if (Mutation == 7)
          Input[1].superclass = Input[0].name;
        else if (Mutation == 8)
          Input[1].superclass = "Lfixture/Missing;";
        else
          Input[1].fields.push_back(
              Field{FieldRef{Input[1].name, "sharedValue", "I"},
                    {"public", "static"}});
        const auto Constructor = Input[0].methods[1];
        const auto Read = Constructor.instructions[0];
        Budget B;
        ClassMap Raw;
        for (const auto &C : Input)
          Raw.emplace(C.name, C);
        EXPECT_FALSE(constructorPrefixStaticRead(Constructor, Read, Raw, B));
        EXPECT_THROW(recoverJava(classes(std::move(Input), Route, B), B),
                     std::runtime_error);
      }
}

// Closed nested allocations before constructor delegation.
namespace {
std::vector<Class> nestedPrefixAllocationFixture(
    bool LeafExternal = false, bool WrapExternal = false,
    bool BaseExternal = false, bool Primary = false, bool Wide = false,
    bool Range = false, bool Frame = false, unsigned Depth = 1) {
  const std::string Core = "Lfixture/Core;", Leaf = "Lfixture/Leaf;",
                    Wrap = "Lfixture/Wrap;", Base = "Lfixture/Base;",
                    Value = "Lfixture/Value;", Object = "Ljava/lang/Object;";
  const std::string Tail = Wide ? "J" : Object;
  const unsigned Words = Wide ? 2 : 1;
  const MethodRef Root{Object, "<init>", {}, "V"};
  const MethodRef LeafInit{Leaf, "<init>", {Object, Tail}, "V"};
  const MethodRef WrapInit{Wrap, "<init>", {Value, Tail}, "V"};
  const MethodRef BaseInit{Base, "<init>", {Object, Value}, "V"};
  const MethodRef Delegate{Core, "<init>", {Object, Value}, "V"};
  const MethodRef Entry{Core, "<init>", {Object, Object, Tail}, "V"};
  auto initializer = [&](const MethodRef &Ref, bool External) {
    unsigned Count = 1;
    for (const auto &Type : Ref.parameters)
      Count += Type == "J" ? 2 : 1;
    return method(
        Ref, {"public", "constructor"}, External ? 0 : Count,
        External ? std::vector<Instruction>{}
                 : std::vector<Instruction>{op(0, "invoke-direct", {0}, Root),
                                            op(1, "return-void")});
  };
  auto LeafClass =
      klass(Leaf, {initializer(LeafInit, LeafExternal)}, LeafExternal);
  auto WrapClass =
      klass(Wrap, {initializer(WrapInit, WrapExternal)}, WrapExternal);
  LeafClass.interfaces = WrapClass.interfaces = {Value};
  auto BaseClass =
      klass(Base, {initializer(BaseInit, BaseExternal)}, BaseExternal);
  BaseClass.access = {"public"};
  auto Contract = klass(Value, {}, true);
  Contract.access = {"public", "interface", "abstract"};
  auto ObjectClass = klass(Object, {initializer(Root, true)}, true);
  ObjectClass.access = {"public"};
  ObjectClass.superclass.reset();
  auto Target = method(
      Delegate, {"private", "constructor"}, 3,
      {op(0, "invoke-direct", {0, 1, 2}, BaseInit), op(1, "return-void")});
  const unsigned Self = Frame ? 20 : 8;
  std::vector<Instruction> C;
  auto add = [&](std::string Opcode, std::vector<unsigned> Registers = {},
                 Reference Ref = {}) {
    C.push_back(
        op(C.size(), std::move(Opcode), std::move(Registers), std::move(Ref)));
  };
  for (unsigned I = 0; I < Depth; ++I)
    add("new-instance", {I}, Wrap);
  add("new-instance", {Depth}, Leaf);
  add("move-object/from16", {Depth + 1, Self + 2});
  add(Wide ? "move-wide/from16" : "move-object/from16", {Depth + 2, Self + 3});
  std::vector<unsigned> Arguments{Depth, Depth + 1, Depth + 2};
  if (Wide)
    Arguments.push_back(Depth + 3);
  add(Range ? "invoke-direct/range" : "invoke-direct", Arguments, LeafInit);
  for (unsigned I = Depth; I-- > 0;) {
    add(Wide ? "move-wide/from16" : "move-object/from16", {I + 2, Self + 3});
    Arguments = {I, I + 1, I + 2};
    if (Wide)
      Arguments.push_back(I + 3);
    add(Range ? "invoke-direct/range" : "invoke-direct", Arguments, WrapInit);
  }
  const auto Delegation = Primary ? BaseInit : Delegate;
  if (Frame || Range) {
    if (Frame) {
      add("move-object/from16", {7, Self});
      add("move-object", {4, 7});
    } else
      add("move-object/from16", {4, Self});
    add("move-object/from16", {5, Self + 1});
    add("move-object", {6, 0});
    add("invoke-direct/range", {4, 5, 6}, Delegation);
  } else
    add("invoke-direct", {Self, Self + 1, 0}, Delegation);
  add("return-void");
  auto Caller = method(Entry, {"public", "constructor"}, Self + 3 + Words, C);
  auto Owner = klass(Core, {Target, Caller});
  Owner.access = {"public"};
  Owner.superclass = Base;
  return {Owner, LeafClass, WrapClass, BaseClass, Contract, ObjectClass};
}
} // namespace

TEST(MobileDalvikConstructorRegions,
     NestedPrefixesReuseTheCompleteAllocationGraph) {
  for (bool LeafExternal : {false, true})
    for (bool WrapExternal : {false, true})
      for (bool BaseExternal : {false, true})
        for (bool Primary : {false, true})
          for (bool Wide : {false, true})
            for (bool Range : {false, true})
              for (bool Frame : {false, true})
                for (unsigned Depth : {1u, 2u, 3u})
                  for (unsigned Route = 0; Route < 3; ++Route) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{LeafExternal, WrapExternal, BaseExternal,
                                   Primary, Wide, Range, Frame, Depth, Route}));
                    auto Input = nestedPrefixAllocationFixture(
                        LeafExternal, WrapExternal, BaseExternal, Primary, Wide,
                        Range, Frame, Depth);
                    const auto M = Input[0].methods[1];
                    Budget B;
                    const auto End = constructorPrefixAllocationEnd(M, 0, B);
                    ASSERT_TRUE(End);
                    const auto *Init =
                        std::get_if<MethodRef>(&M.instructions[*End].reference);
                    ASSERT_NE(Init, nullptr);
                    EXPECT_EQ(Init->owner, Input[2].name);
                    EXPECT_EQ(M.instructions[*End].registers.front(), 0u);
                    EXPECT_FALSE(
                        constructorPrefixAllocationArgumentsEnd(M, 0, B));
                    const auto Prefix = selectConstructorPrefixDelegation(M, B);
                    ASSERT_TRUE(Prefix);
                    EXPECT_TRUE(Prefix->requires_dispatch);
                    EXPECT_EQ(Prefix->call, M.instructions.size() - 2);
                    EXPECT_EQ(Prefix->allocations,
                              (std::map<size_t, size_t>{{0, *End}}));
                    if (Route == 2)
                      for (auto &Cls : Input)
                        std::reverse(Cls.methods.begin(), Cls.methods.end());
                    const auto R =
                        recoverJava(classes(std::move(Input), Route, B), B);
                    EXPECT_EQ(R.getInteger("class_count"), 1 + !LeafExternal +
                                                               !WrapExternal +
                                                               !BaseExternal);
                    EXPECT_EQ(R.getInteger("method_count"), 2 + !LeafExternal +
                                                                !WrapExternal +
                                                                !BaseExternal);
                    EXPECT_EQ(R.getInteger("minimum_java_release"), 25);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_EQ(
                        R.getInteger("projected_method_count").value_or(0), 0);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                    EXPECT_FALSE(R.getArray("class_source_bindings"));
                  }
}

TEST(MobileDalvikConstructorRegions,
     NestedPrefixesRejectOpenLifetimesAndChangedInitializers) {
  for (bool External : {false, true})
    for (unsigned Mutation = 0; Mutation < 36; ++Mutation)
      for (unsigned Route = 0; Route < 3; ++Route) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{External, Mutation, Route}));
        auto Input =
            nestedPrefixAllocationFixture(External, External, External);
        auto &M = Input[0].methods[1];
        auto &C = M.instructions;
        const unsigned Self = M.registers - M.incomingWords();
        switch (Mutation) {
        case 0:
          C[1].registers = {0};
          break;
        case 1:
          C[1].registers = {Self};
          break;
        case 2:
          C[1].reference = Input[2].name;
          break;
        case 3:
          std::get<MethodRef>(C[4].reference).owner = Input[2].name;
          break;
        case 4:
          C[4].registers[0] = 0;
          break;
        case 5:
          C[4].registers[1] = 0;
          break;
        case 6:
          C.erase(C.begin() + 4);
          break;
        case 7:
          std::get<MethodRef>(C[6].reference).returns = "I";
          break;
        case 8:
          C[4].opcode = "invoke-virtual";
          break;
        case 9:
          C[1].literal = int64_t(1);
          break;
        case 10:
          C[1].element_width = 1;
          break;
        case 11:
          C[1].target = 4;
          break;
        case 12:
          C.back() = op(8, "goto");
          C.back().target = 1;
          break;
        case 13:
          M.tries = {{0, 2, {{std::nullopt, 0}}}};
          break;
        case 14:
          C[1].pc = C[0].pc;
          break;
        case 15:
          C[6].registers.push_back(3);
          break;
        case 16:
          C[1].registers = {M.registers};
          break;
        case 17:
          C[4].registers[2] = Self;
          break;
        case 18:
          C[6].registers[1] = Self;
          break;
        case 19:
          C[4].opcode += "/invented";
          break;
        case 20:
          C[1].registers.push_back(2);
          break;
        case 21:
          C[1].keys = {1};
          break;
        case 22:
          C[1].targets = {4};
          break;
        case 23:
          C[1].data = {1};
          break;
        case 24:
          C[2].registers[1] = 1;
          break;
        case 25:
          C[2].registers[0] = 1;
          break;
        case 26:
          C[3].opcode = "move-wide/from16";
          break;
        case 27:
          Input[1].methods[0].access = {"private", "constructor"};
          break;
        case 28:
          Input[2].access.insert("abstract");
          break;
        case 29:
          Input[1].methods.clear();
          break;
        case 30: {
          // The wide low word is disjoint from the outer receiver. Its high
          // word still overwrites the pending inner allocation.
          for (auto &I : C)
            for (auto &R : I.registers)
              if (R == 1)
                R = 4;
          auto Load = op(2, "const-wide/16", {3});
          Load.literal = int64_t(0);
          C.insert(C.begin() + 2, Load);
          break;
        }
        case 31:
          C[6].registers[0] = 1;
          break;
        case 32:
          C.insert(C.begin() + 5, C[4]);
          break;
        case 33:
          C[2] = op(2, "goto");
          C[2].target = 3;
          break;
        case 34:
          M.registers = 3;
          break;
        case 35:
          C.insert(C.begin(), op(0, "move-object", {7, Self}));
          C[2].registers[0] = 7;
          C[5].registers[0] = 7;
          C[7].registers[1] = 7;
          break;
        }
        if (Mutation == 6 || Mutation == 30 || Mutation == 32 ||
            Mutation == 35) {
          for (size_t I = 0; I < C.size(); ++I)
            C[I].pc = I;
          M.code_end = C.size();
        }
        Budget B;
        EXPECT_THROW(recoverJava(classes(std::move(Input), Route, B), B),
                     std::runtime_error);
      }
}

// Filled reference arrays before constructor delegation.
namespace {
std::vector<Class> filledReferencePrefixFixture(
    std::string Element = "Ljava/lang/Object;", bool ParentExternal = false,
    bool FactoryExternal = false, bool Primary = false, bool ArrayRange = false,
    bool CallRange = false, bool Reuse = false, bool Guarded = false,
    bool Empty = false) {
  const std::string Core = "Lfixture/Core;", Base = "Lfixture/Base;",
                    Factory = "Lfixture/Factory;",
                    Object = "Ljava/lang/Object;", Array = "[" + Element;
  const MethodRef Root{Object, "<init>", {}, "V"};
  const MethodRef Parent{Base, "<init>", {Array}, "V"};
  const MethodRef Prepare{Factory, "prepare", {Array}, Array};
  const MethodRef Delegate{Core, "<init>", {Element, Element}, "V"};
  const FieldRef Stored{Base, "values", Array};
  auto P = method(Parent, {"public", "constructor"}, 2,
                  {op(0, "invoke-direct", {0}, Root),
                   op(1, "iput-object", {1, 0}, Stored), op(2, "return-void")});
  auto F =
      method(Prepare, {"public", "static"}, 1, {op(0, "return-object", {0})});
  for (auto Pair :
       {std::pair{&P, ParentExternal}, std::pair{&F, FactoryExternal}})
    if (Pair.second) {
      Pair.first->instructions.clear();
      Pair.first->registers = 0;
      Pair.first->code_end = 0;
    }
  auto prefix = [&](MethodRef Ref) {
    const unsigned Result = Reuse ? 9 : 1;
    std::vector<Instruction> C;
    if (Guarded) {
      C.push_back(op(0, "if-eqz", {9}));
      C.back().target = 2;
      C.push_back(op(1, "nop"));
    }
    auto add = [&](std::string Kind, std::vector<unsigned> Words = {},
                   Reference R = {}) {
      C.push_back(
          op(C.size(), std::move(Kind), std::move(Words), std::move(R)));
    };
    add(ArrayRange ? "filled-new-array/range" : "filled-new-array",
        Empty ? std::vector<unsigned>{} : std::vector<unsigned>{9, 10}, Array);
    add("move-result-object", {Result});
    add(CallRange ? "invoke-static/range" : "invoke-static", {Result}, Prepare);
    add("move-result-object", {Result});
    if (CallRange) {
      add("move-object", {3, 8});
      add("move-object", {4, Result});
    }
    add(CallRange ? "invoke-direct/range" : "invoke-direct",
        CallRange ? std::vector<unsigned>{3, 4}
                  : std::vector<unsigned>{8, Result},
        Parent);
    add("return-void");
    return method(Ref, {"public", "constructor"}, 9 + Ref.parameters.size(), C);
  };
  auto Target = prefix(Delegate);
  MethodRef Entry{Core, "<init>", {Element, Element, "I"}, "V"};
  auto Caller =
      Primary
          ? prefix(Entry)
          : method(Entry, {"public", "constructor"}, 12,
                   {op(0, "move-object", {0, 8}), op(1, "move-object", {1, 9}),
                    op(2, "move-object", {2, 10}),
                    op(3, CallRange ? "invoke-direct/range" : "invoke-direct",
                       {0, 1, 2}, Delegate),
                    op(4, "return-void")});
  auto C = klass(Core, {Target, Caller});
  C.superclass = Base;
  auto B = klass(Base, {P}, ParentExternal);
  B.access = {"public"};
  B.fields = {{Stored, {"public", "final"}, {}}};
  auto Provider = klass(Factory, {F}, FactoryExternal);
  auto Marker = klass("Lfixture/Value;", {}, true);
  Marker.access = {"public", "interface", "abstract"};
  auto ObjectClass =
      klass(Object, {method(Root, {"public", "constructor"}, 0)}, true);
  ObjectClass.access = {"public"};
  ObjectClass.superclass.reset();
  return {C, B, Provider, Marker, ObjectClass};
}
} // namespace

TEST(MobileDalvikConstructorRegions,
     FilledReferencePrefixesKeepSharedInitializationProof) {
  for (const std::string Element :
       {"Ljava/lang/Object;", "Ljava/lang/String;", "Lfixture/Value;", "[I",
        "[Ljava/lang/Object;"})
    for (bool External : {false, true})
      for (bool Primary : {false, true})
        for (bool Range : {false, true})
          for (bool Reuse : {false, true})
            for (bool Guarded : {false, true})
              for (bool Empty : {false, true})
                for (unsigned Route = 0; Route < 3; ++Route) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{Element, External, Primary, Range, Reuse,
                                 Guarded, Empty, Route}));
                  auto Input = filledReferencePrefixFixture(
                      Element, External, !External, Primary, Range, !Range,
                      Reuse, Guarded, Empty);
                  const auto Target = Input[0].methods[0].reference,
                             Entry = Input[0].methods[1].reference;
                  Budget B;
                  const auto Classes = classes(std::move(Input), Route, B);
                  ConstructorSuperPrologues Prologues;
                  const auto Delegations =
                      planConstructorDelegations(Classes, B, {}, &Prologues);
                  EXPECT_TRUE(Prologues.contains(Target));
                  if (Primary)
                    EXPECT_TRUE(Prologues.contains(Entry));
                  else
                    EXPECT_EQ(Delegations.at(Entry), Target);
                  const auto Report = recoverJava(Classes, B);
                  EXPECT_EQ(Report.getInteger("class_count"), 2);
                  EXPECT_EQ(Report.getInteger("method_count"), 3);
                  EXPECT_EQ(Report.getInteger("minimum_java_release"), 25);
                  EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                  EXPECT_EQ(
                      Report.getInteger("projected_method_count").value_or(0),
                      0);
                  EXPECT_FALSE(
                      Report.getArray("generic_signature_projections"));
                }
}

TEST(MobileDalvikConstructorRegions,
     FilledReferencePrefixesRejectUnprovedWordsAndResults) {
  for (bool External : {false, true})
    for (unsigned Mutation = 0; Mutation < 35; ++Mutation)
      for (unsigned Route = 0; Route < 3; ++Route) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{External, Mutation, Route}));
        auto Input = filledReferencePrefixFixture("Ljava/lang/Object;",
                                                  External, External);
        if (Mutation >= 32)
          Input = filledReferencePrefixFixture("Ljava/lang/Object;", External,
                                               External, false, false, true,
                                               true, true);
        auto &M = Input[0].methods[0];
        auto &A = M.instructions[0];
        auto &R = M.instructions[1];
        if (Mutation >= 32) {
          // The prefix replaced incoming word 9, then copied its current
          // argument to word 4. A later read cannot resurrect the old input.
          std::vector<Instruction> Tail;
          if (Mutation == 32)
            Tail = {op(0, "invoke-static", {9}, Input[2].methods[0].reference)};
          else if (Mutation == 33)
            Tail = {op(0, "move-object", {7, 9}),
                    op(0, "invoke-static", {7}, Input[2].methods[0].reference)};
          else
            Tail = {op(0, "array-length", {0, 9})};
          M.instructions.insert(M.instructions.end() - 1, Tail.begin(),
                                Tail.end());
          for (size_t I = 0; I < M.instructions.size(); ++I)
            M.instructions[I].pc = I;
          M.code_end = M.instructions.size();
        } else
          switch (Mutation) {
          case 0:
            A.reference = std::string("[J");
            break;
          case 1:
            A.reference = std::string("[D");
            break;
          case 2:
            A.reference = std::string("[F");
            break;
          case 3:
            A.reference = std::string("Ljava/lang/Object;");
            break;
          case 4:
            A.registers = {8};
            break;
          case 5:
            A.registers = {7};
            break;
          case 6:
            A.registers = {11};
            break;
          case 7:
            R.opcode = "move-result";
            break;
          case 8:
            R.opcode = "move-result-wide";
            break;
          case 9:
            R = op(1, "nop");
            break;
          case 10:
            R.registers = {8};
            break;
          case 11:
            R.registers = {11};
            break;
          case 12:
            R.registers = {1, 2};
            break;
          case 13:
            R.reference = std::string("[Ljava/lang/Object;");
            break;
          case 14:
            R.literal = int64_t(0);
            break;
          case 15:
            A.literal = int64_t(0);
            break;
          case 16:
            A.target = 4;
            break;
          case 17:
            R.target = 4;
            break;
          case 18:
            A.targets = {4};
            break;
          case 19:
            A.keys = {1};
            break;
          case 20:
            A.data = {1};
            break;
          case 21:
            A.element_width = 1;
            break;
          case 22:
            R.pc = A.pc;
            break;
          case 23:
            R.opcode = "move-result-object/invented";
            break;
          case 24:
            A.opcode = "filled-new-array/invented";
            break;
          case 25:
            Input[2].methods[0].access = {"private", "static"};
            break;
          case 26:
            Input[1].methods[0].access = {"private", "constructor"};
            break;
          case 27:
            M.instructions[2].registers = {2};
            break;
          case 28:
            M.instructions[4].registers = {1, 8};
            break;
          case 29:
            M.instructions[2].opcode = "invoke-virtual";
            break;
          case 30:
            M.instructions.insert(M.instructions.begin(),
                                  op(0, "move-object", {7, 8}));
            M.instructions[1].registers = {7};
            for (size_t I = 0; I < M.instructions.size(); ++I)
              M.instructions[I].pc = I;
            M.code_end = M.instructions.size();
            break;
          case 31:
            M.instructions.insert(M.instructions.begin(), op(0, "goto"));
            M.instructions[0].target = 2;
            for (size_t I = 0; I < M.instructions.size(); ++I)
              M.instructions[I].pc = I;
            M.code_end = M.instructions.size();
            break;
          }
        Budget B;
        EXPECT_THROW(recoverJava(classes(std::move(Input), Route, B), B),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> linearTypeTestPrefix(bool SDK, bool Super, bool Range,
                                        bool Generic, unsigned Type,
                                        unsigned InputKind) {
  const std::string Object = "Ljava/lang/Object;", Core = "Lfixture/TypeCore;",
                    Config = "Lfixture/Options;", Parent = "Lfixture/TypeBase;",
                    Marker = "Lfixture/Marker;";
  const std::string TargetOwner = Super ? Parent : Core;
  const MethodRef ObjectInit{Object, "<init>", {}, "V"};
  const MethodRef Primary{TargetOwner, "<init>", {Object, Config}, "V"};
  const MethodRef Make{Config, "create", {}, Config};
  const MethodRef First{Config, "first", {"Z"}, Config};
  const MethodRef Second{Config, "second", {"Z"}, Config};
  auto Target = method(
      Primary, {"public", "constructor"}, 3,
      {op(0, "invoke-direct", {0}, ObjectInit),
       op(1, "iput-object", {1, 0}, FieldRef{TargetOwner, "value", Object}),
       op(2, "iput-object", {2, 0}, FieldRef{TargetOwner, "options", Config}),
       op(3, "return-void")});
  const unsigned Self = Range ? 20 : 2, Value = Self + 1, Flag = Self + 2;
  std::vector<Instruction> Code;
  auto Add = [&](std::string Name, std::vector<unsigned> Words = {},
                 Reference Ref = {}) {
    Code.push_back(
        op(Code.size(), std::move(Name), std::move(Words), std::move(Ref)));
  };
  Add(Range ? "invoke-static/range" : "invoke-static", {}, Make);
  Add("move-result-object", {0});
  unsigned Tested = Value;
  if (InputKind == 1) {
    Add("const/4", {1});
    Code.back().literal = int64_t(0);
    Tested = 1;
  } else if (InputKind == 2) {
    Add(Range ? "move-object/from16" : "move-object", {1, Value});
    Tested = 1;
  }
  Add("instance-of", {1, Tested},
      Type == 0   ? Marker
      : Type == 1 ? "[Lfixture/Marker;"
                  : "[I");
  Add(Range ? "invoke-virtual/range" : "invoke-virtual", {0, 1}, First);
  Add("move-result-object", {0});
  Add(Range ? "move/from16" : "move", {1, Flag});
  Add(Range ? "invoke-virtual/range" : "invoke-virtual", {0, 1}, Second);
  Add("move-result-object", {0});
  if (Range) {
    Add("move-object/from16", {6, Self});
    Add("move-object/from16", {7, Value});
    Add("move-object", {8, 0});
    Add("invoke-direct/range", {6, 7, 8}, Primary);
  } else
    Add("invoke-direct", {Self, Value, 0}, Primary);
  Add("return-void");
  auto Caller = method({Core, "<init>", {Object, "Z", "Z"}, "V"},
                       {"public", "constructor"}, Self + 4, Code);
  auto Owner = klass(Core, Super ? std::vector<Method>{Caller}
                                 : std::vector<Method>{Target, Caller});
  auto Base =
      klass(Parent, Super ? std::vector<Method>{Target} : std::vector<Method>{},
            true);
  Base.access.erase("final");
  if (Super) {
    Owner.superclass = Parent;
    Base.external = SDK;
  }
  auto &Initialized = Super ? Base : Owner;
  Initialized.fields = {
      {FieldRef{TargetOwner, "value", Object}, {"public", "final"}},
      {FieldRef{TargetOwner, "options", Config}, {"public", "final"}}};
  if (Super && SDK) {
    Base.methods[0].instructions.clear();
    Base.methods[0].registers = Base.methods[0].code_end = 0;
  }
  if (Generic) {
    Owner.generic_signature =
        "<K:Ljava/lang/Object;>" +
        (Super ? std::string("Lfixture/TypeBase<TK;>;") : Object);
    if (Super)
      Base.generic_signature = "<P:Ljava/lang/Object;>Ljava/lang/Object;";
  }
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  auto Provider =
      klass(Config,
            {method(Make, {"public", "static"}, 1,
                    {Zero, op(1, "return-object", {0})}),
             method(First, {"public"}, 2, {op(0, "return-object", {0})}),
             method(Second, {"public"}, 2, {op(0, "return-object", {0})})},
            SDK);
  if (SDK)
    for (auto &M : Provider.methods) {
      M.instructions.clear();
      M.registers = M.code_end = 0;
    }
  auto TestedClass = klass(Marker, {}, true);
  TestedClass.access = {"public", "abstract", "interface"};
  auto Root =
      klass(Object, {method(ObjectInit, {"public", "constructor"}, 0)}, true);
  Root.superclass.reset();
  Root.access = {"public"};
  return {Owner, Base, Provider, TestedClass, Root};
}
} // namespace

TEST(MobileDalvikConstructorRegions,
     LinearTypeTestsKeepOriginalCallOrderAndBooleanWords) {
  for (bool SDK : {false, true})
    for (bool Super : {false, true})
      for (bool Range : {false, true})
        for (bool Generic : {false, true})
          for (unsigned Type = 0; Type < 3; ++Type)
            for (unsigned InputKind = 0; InputKind < 3; ++InputKind)
              for (unsigned Route = 0; Route < 3; ++Route) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, Super, Range, Generic, Type, InputKind, Route}));
                auto Input = linearTypeTestPrefix(SDK, Super, Range, Generic,
                                                  Type, InputKind);
                const auto Original = Input;
                Budget B;
                const auto Selected = selectConstructorPrefixDelegation(
                    Input[0].methods.back(), B);
                ASSERT_TRUE(Selected);
                EXPECT_TRUE(Selected->requires_dispatch);
                const auto Classes = classes(std::move(Input), Route, B);
                const auto Report = recoverJava(Classes, B);
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                EXPECT_EQ(Report.getInteger("minimum_java_release"), 25);
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
                EXPECT_FALSE(Report.getArray("field_source_bindings"));
                for (const auto &C : Original) {
                  const auto &After = Classes.at(C.name);
                  EXPECT_EQ(After.generic_signature, C.generic_signature);
                  EXPECT_EQ(After.access, C.access);
                  for (size_t I = 0; I < C.methods.size(); ++I) {
                    EXPECT_EQ(After.methods[I].reference,
                              C.methods[I].reference);
                    EXPECT_EQ(After.methods[I].access, C.methods[I].access);
                    EXPECT_EQ(After.methods[I].generic_signature,
                              C.methods[I].generic_signature);
                  }
                }
              }
}

TEST(MobileDalvikConstructorRegions,
     LinearTypeTestsRejectUnavailableReferencesAndMalformedCalls) {
  for (bool Super : {false, true})
    for (bool SDK : {false, true})
      for (unsigned Mutation = 0; Mutation < 18; ++Mutation)
        for (unsigned Route = 0; Route < 3; ++Route) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{Super, SDK, Mutation, Route}));
          auto Input = linearTypeTestPrefix(SDK, Super, false, false, 0, 0);
          auto &M = Input[0].methods.back();
          auto &Test = M.instructions[2];
          switch (Mutation) {
          case 0:
            Test.registers[1] = 2;
            break;
          case 1:
            Test.registers[0] = 2;
            break;
          case 2:
            Test.registers.pop_back();
            break;
          case 3:
            Test.reference = std::monostate{};
            break;
          case 4:
            Test.reference = std::string("I");
            break;
          case 5:
            Test.registers[1] = 4;
            break;
          case 6:
            Test.opcode = "instance-of/vendor";
            break;
          case 7:
            Test.registers[0] = M.registers;
            break;
          case 8:
            Test.registers[1] = 1;
            break;
          case 9:
            M.instructions[3].registers.pop_back();
            break;
          case 10:
            M.instructions[3].opcode = "invoke-static";
            break;
          case 11:
            M.instructions[4].opcode = "move-result-wide";
            break;
          case 12:
            Input[2].methods[1].access = {"private"};
            break;
          case 13:
            Input[2].methods[0].access.erase("static");
            break;
          case 14:
            M.instructions[0].registers = {2};
            break;
          case 15:
            M.instructions[M.instructions.size() - 2].registers[0] = 3;
            break;
          case 16:
            Test.target = M.instructions.back().pc;
            break;
          case 17:
            M.instructions[M.instructions.size() - 2].registers.pop_back();
            break;
          }
          Budget B;
          EXPECT_THROW(recoverJava(classes(std::move(Input), Route, B), B),
                       std::runtime_error);
        }
}

namespace {
std::vector<Class> arrayLengthConstructorPrefix(const std::string &Array,
                                                bool SDK, bool Super,
                                                bool Range, bool Reuse,
                                                bool High) {
  const std::string Core = "Lfixture/Core;", Base = "Lfixture/Base;",
                    Probe = "Lfixture/Probe;", Object = "Ljava/lang/Object;";
  const MethodRef ObjectInit{Object, "<init>", {}, "V"};
  const MethodRef Target{Super ? Base : Core, "<init>", {Array, "I", "J"}, "V"};
  const MethodRef Entry{Core, "<init>", {Array, "J"}, "V"};
  const MethodRef Before{Probe, "before", {}, "V"};
  const MethodRef After{Probe, "after", {"I"}, "V"};
  const FieldRef Data{Target.owner, "data", Array},
      Length{Target.owner, "length", "I"}, Tag{Target.owner, "tag", "J"};
  auto Primary =
      method(Target, {"public", "constructor"}, 5,
             {op(0, "invoke-direct", {0}, ObjectInit),
              op(1, "iput-object", {1, 0}, Data), op(2, "iput", {2, 0}, Length),
              op(3, "iput-wide", {3, 0}, Tag), op(4, "return-void")});
  const unsigned Self = High ? 20 : 7, Result = Reuse ? 1 : 0;
  auto Delegating = method(
      Entry, {"public", "constructor"}, Self + 4,
      {op(0, "move-object/from16", {1, Self + 1}),
       op(1, "invoke-static", {}, Before), op(2, "array-length", {Result, 1}),
       op(3, "invoke-static", {Result}, After),
       op(4, "move-object/from16", {2, Self}),
       op(5, "move-object/from16", {3, Self + 1}), op(6, "move", {4, Result}),
       op(7, "move-wide/from16", {5, Self + 2}),
       op(8, Range ? "invoke-direct/range" : "invoke-direct", {2, 3, 4, 5, 6},
          Target),
       op(9, "return-void")});
  auto Owner = klass(Core, Super ? std::vector<Method>{Delegating}
                                 : std::vector<Method>{Primary, Delegating});
  auto Parent = klass(Base, {Primary});
  Parent.access.erase("final");
  if (Super)
    Owner.superclass = Base;
  auto &Fields = Super ? Parent.fields : Owner.fields;
  for (const auto &Field : {Data, Length, Tag})
    Fields.push_back({Field, {"public", "final"}, {}});
  auto Provider =
      klass(Probe,
            {method(Before, {"public", "static"}, 0,
                    SDK ? std::vector<Instruction>{}
                        : std::vector<Instruction>{op(0, "return-void")}),
             method(After, {"public", "static"}, SDK ? 0 : 1,
                    SDK ? std::vector<Instruction>{}
                        : std::vector<Instruction>{op(0, "return-void")})},
            SDK);
  auto Root =
      klass(Object, {method(ObjectInit, {"public", "constructor"}, 0)}, true);
  Root.superclass.reset();
  Root.access = {"public"};
  auto Exception = klass("Ljava/io/IOException;", {}, true);
  Exception.superclass = "Ljava/lang/Exception;";
  for (auto *C : {&Owner, &Parent})
    for (auto &M : C->methods)
      M.declared_throws = {Exception.name};
  std::vector<Class> Input{Owner, Provider, Root, Exception};
  if (Super)
    Input.push_back(Parent);
  return Input;
}
} // namespace

TEST(MobileDalvikConstructorRegions,
     ArrayLengthsKeepDelegationReceiptsAndOriginalFinalStores) {
  for (const auto &Array : {"[Z", "[B", "[S", "[C", "[I", "[J", "[F", "[D",
                            "[Ljava/lang/Object;", "[[B"})
    for (bool SDK : {false, true})
      for (bool Super : {false, true})
        for (bool Range : {false, true})
          for (bool Reuse : {false, true})
            for (bool High : {false, true})
              for (unsigned Route = 0; Route < 3; ++Route) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{Array, SDK, Super, Range, Reuse, High, Route}));
                auto Input = arrayLengthConstructorPrefix(Array, SDK, Super,
                                                          Range, Reuse, High);
                const auto Entry = Input[0].methods.back().reference;
                const auto Target = std::get<MethodRef>(
                    Input[0].methods.back().instructions[8].reference);
                Budget B;
                const auto Selected = selectConstructorPrefixDelegation(
                    Input[0].methods.back(), B);
                ASSERT_TRUE(Selected);
                EXPECT_EQ(Selected->call, 8u);
                EXPECT_TRUE(Selected->requires_dispatch);
                if (Route == 2)
                  for (auto &C : Input)
                    std::reverse(C.methods.begin(), C.methods.end());
                const auto Classes = classes(Input, Route, B);
                ConstructorSuperPrologues Prologues;
                const auto Delegations =
                    planConstructorDelegations(Classes, B, {}, &Prologues);
                if (Super) {
                  ASSERT_TRUE(Prologues.contains(Entry));
                  EXPECT_EQ(Prologues.at(Entry).target, Target);
                  EXPECT_TRUE(Prologues.at(Entry).requires_dispatch);
                } else {
                  ASSERT_TRUE(Delegations.contains(Entry));
                  EXPECT_EQ(Delegations.at(Entry), Target);
                }
                const auto Report = recoverJava(Classes, B);
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                EXPECT_EQ(Report.getInteger("minimum_java_release"), 25);
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
                EXPECT_FALSE(Report.getArray("field_source_bindings"));
                EXPECT_FALSE(
                    Report.getBoolean("requires_java_rebuild_postprocessing")
                        .value_or(false));
                for (const auto &C : Input) {
                  const auto &After = Classes.at(C.name);
                  EXPECT_EQ(After.access, C.access);
                  for (size_t I = 0; I < C.methods.size(); ++I) {
                    EXPECT_EQ(After.methods[I].reference,
                              C.methods[I].reference);
                    EXPECT_EQ(After.methods[I].access, C.methods[I].access);
                    EXPECT_EQ(After.methods[I].declared_throws,
                              C.methods[I].declared_throws);
                  }
                }
              }
}

TEST(MobileDalvikConstructorRegions,
     ArrayLengthsRejectUninitializedReceiverAndMalformedOperands) {
  for (bool Super : {false, true})
    for (unsigned Route = 0; Route < 3; ++Route)
      for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{Super, Route, Mutation}));
        auto Input =
            arrayLengthConstructorPrefix("[B", true, Super, true, false, true);
        auto &M = Input[0].methods.back();
        auto &Length = M.instructions[2];
        const unsigned Self = M.registers - M.incomingWords();
        switch (Mutation) {
        case 0:
          Length.registers = {0};
          break;
        case 1:
          Length.registers.push_back(1);
          break;
        case 2:
          Length.registers[1] = Self;
          break;
        case 3:
          Length.registers[0] = Self;
          break;
        case 4:
          M.instructions[0].registers[1] = Self;
          break;
        case 5:
          Length.literal = int64_t{0};
          break;
        case 6:
          Length.reference = "[B";
          break;
        case 7:
          Length.target = 8;
          break;
        case 8:
          Length.keys = {0};
          break;
        case 9:
          Length.targets = {8};
          break;
        case 10:
          Length.data = {0};
          break;
        case 11:
          Length.element_width = 1;
          break;
        case 12:
          Length.opcode = "array-length/vendor";
          break;
        case 13:
          M.instructions[0] = op(0, "const/4", {1});
          M.instructions[0].literal = int64_t{0};
          break;
        case 14:
          M.instructions[0] = op(0, "move/from16", {1, Self + 2});
          break;
        case 15:
          Length.registers[0] = Self + 3;
          break;
        case 16:
          M.instructions[8].registers.pop_back();
          break;
        case 17:
          M.instructions[8].registers[0] = 3;
          break;
        case 18:
          M.instructions[1] = op(1, "goto");
          M.instructions[1].target = 4;
          break;
        case 19:
          Input[1].methods[0].access.erase("static");
          break;
        }
        if (Mutation >= 5 && Mutation <= 12) {
          // The ordinary body and the constructor prefix share one shape
          // validator, including the exact unsuffixed instruction opcode.
          auto Ordinary = Length;
          Ordinary.pc = 0;
          Ordinary.registers = {0, 1};
          auto Read =
              method({"Lfixture/Core;", "length", {"[B"}, "I"},
                     {"public", "static"}, 2, {Ordinary, op(1, "return", {0})});
          Budget Plain;
          EXPECT_THROW(recoverJava(classes({klass("Lfixture/Core;", {Read})},
                                           Route, Plain),
                                   Plain),
                       std::runtime_error);
        }
        Budget B;
        EXPECT_THROW(recoverJava(classes(std::move(Input), Route, B), B),
                     std::runtime_error);
      }
}

TEST(MobileDalvikConstructorRegion, ConditionalOverloadsShareCapturedArguments) {
  const std::string object = "Ljava/lang/Object;", string = "Ljava/lang/String;",
                    item = "Lfixture/Item;", factory = "Lfixture/Factory;";
  const MethodRef root{object, "<init>", {}, "V"};
  const MethodRef ordinary{item, "<init>", {"Z", "J"}, "V"};
  const MethodRef named{item, "<init>", {string, "Z", "J"}, "V"};
  for (bool external : {false, true})
    for (bool reversed : {false, true})
      for (unsigned change = 0; change < 4; ++change) {
        SCOPED_TRACE(::testing::PrintToString(std::tuple{external, reversed, change}));
        auto constructor = [&](const MethodRef &ref, unsigned words) {
          if (external)
            return method(ref, {"public", "constructor"}, 0);
          return method(ref, {"public", "constructor"}, words,
                        {op(0, "invoke-direct", {0}, root), op(1, "return-void")});
        };
        auto branch = op(1, "if-nez", {1});
        branch.target = 4;
        auto jump = op(3, "goto");
        jump.target = 5;
        auto short_arm = op(2, "invoke-direct", {0, 2, 3, 4}, ordinary);
        auto long_arm = op(4, "invoke-direct", {0, 1, 2, 3, 4}, named);
        if (change == 1)
          long_arm.registers[2] = 5; // Another initialized boolean.
        if (change == 2)
          long_arm.registers.back() = 3; // The wide high word is missing.
        if (change == 3)
          branch.registers[0] = 0; // The tested word is uninitialized.
        if (reversed) {
          std::swap(short_arm, long_arm);
          short_arm.pc = 2;
          long_arm.pc = 4;
        }
        auto create = method({factory, "make", {string, "Z", "J", "Z"}, item},
                             {"public", "static"}, 6,
                             {op(0, "new-instance", {0}, item), branch,
                              short_arm, jump, long_arm, op(5, "return-object", {0})});
        auto base = klass(object, {method(root, {"public", "constructor"}, 0)}, true);
        base.access = {"public"};
        base.superclass.reset();
        Budget budget;
        auto classes = linkClasses(
            {klass(factory, {create}),
             klass(item, {constructor(ordinary, 4), constructor(named, 5)}, external),
             klass(string, {}, true), base}, budget);
        if (change) {
          EXPECT_THROW(recoverJava(classes, budget), std::runtime_error);
        } else {
          const auto report = recoverJava(classes, budget);
          EXPECT_EQ(report.getInteger("unrecovered_method_count"), 0);
          EXPECT_EQ(report.getInteger("projected_method_count").value_or(0), 0);
        }
      }
}

TEST(MobileDalvikConstructorRegion, ConditionalArmsPrepareTheSameConstructor) {
  const std::string object = "Ljava/lang/Object;", item = "Lfixture/Item;",
                    holder = "Lfixture/Holder;", token = "Lfixture/Token;";
  const MethodRef root{object, "<init>", {}, "V"};
  const MethodRef init{item, "<init>", {token, "I"}, "V"};
  const FieldRef field{holder, "value", token};
  for (bool external : {false, true})
    for (bool exposeReceiver : {false, true}) {
      SCOPED_TRACE(
          ::testing::PrintToString(std::pair{external, exposeReceiver}));
      auto base =
          klass(object, {method(root, {"public", "constructor"}, 0)}, true);
      base.access = {"public"};
      base.superclass.reset();
      auto provider = klass(holder, {}, external);
      Field value;
      value.reference = field;
      value.access = {"public"};
      provider.fields.push_back(value);
      auto constructor = external ? method(init, {"public", "constructor"}, 0)
                                  : method(init, {"public", "constructor"}, 3,
                                           {op(0, "invoke-direct", {0}, root),
                                            op(1, "return-void")});
      auto condition = op(1, "if-ltz", {4});
      condition.target = 5;
      auto jump = op(4, "goto");
      jump.target = 7;
      auto read = op(2, "iget-object", {1, exposeReceiver ? 0u : 2u}, field);
      auto make =
          method({"Lfixture/Factory;", "pick", {holder, object, "I"}, item},
                 {"public", "static"}, 5,
                 {op(0, "new-instance", {0}, item), condition, read,
                  op(3, "invoke-direct", {0, 1, 4}, init), jump,
                  op(5, "check-cast", {3}, token),
                  op(6, "invoke-direct", {0, 3, 4}, init),
                  op(7, "return-object", {0})});
      Budget budget;
      auto input = linkClasses({std::move(base), std::move(provider),
                                klass(token, {}, external),
                                klass(item, {constructor}, external),
                                klass(make.reference.owner, {make})},
                               budget);
      if (exposeReceiver) {
        EXPECT_THROW(recoverJava(input, budget), std::runtime_error);
      } else {
        const auto report = recoverJava(input, budget);
        EXPECT_EQ(report.getInteger("unrecovered_method_count"), 0);
        EXPECT_GE(report.getInteger("minimum_java_release").value_or(8), 14);
        const auto *units = report.getArray("source_units");
        ASSERT_TRUE(units);
        bool prepared = false;
        for (const auto &unit : *units) {
          const auto *row = unit.getAsObject();
          if (row && row->getString("class") == make.reference.owner)
            prepared = row->getString("source")->contains(
                "new fixture.Item(switch (0)");
        }
        EXPECT_TRUE(prepared);
      }
    }
}

TEST(MobileDalvikConstructorRegion,
     ReverseLiteralSubtractionOrdersConstructorOperands) {
  const std::string object = "Ljava/lang/Object;", item = "Lfixture/Item;";
  const MethodRef init{item, "<init>", {"I"}, "V"};
  auto base = klass(object, {}, true);
  base.access = {"public"};
  base.superclass.reset();
  auto provider =
      klass(item, {method(init, {"public", "constructor"}, 0)}, true);
  for (const auto &[opcode, literal, valid] :
       std::vector<std::tuple<std::string, int, bool>>{
           {"rsub-int/lit8", 8, true},
           {"rsub-int", -32768, true},
           {"rsub-int/lit8", 128, false},
           {"rsub-int", -32769, false}}) {
    SCOPED_TRACE(::testing::PrintToString(std::pair{opcode, literal}));
    auto subtract = op(1, opcode, {1, 1});
    subtract.literal = int64_t(literal);
    auto make = method(
        {"Lfixture/Factory;", "make", {"I"}, item}, {"public", "static"}, 2,
        {op(0, "new-instance", {0}, item), subtract,
         op(2, "invoke-direct", {0, 1}, init), op(3, "return-object", {0})});
    Budget budget;
    auto classes = linkClasses(
        {base, provider, klass(make.reference.owner, {make})}, budget);
    if (!valid) {
      EXPECT_THROW(recoverJava(classes, budget), std::runtime_error);
      continue;
    }
    const auto report = recoverJava(classes, budget);
    const auto *units = report.getArray("source_units");
    ASSERT_TRUE(units);
    ASSERT_EQ(units->size(), 1u);
    const auto source = (*units)[0].getAsObject()->getString("source");
    ASSERT_TRUE(source);
    EXPECT_TRUE(source->contains(") - (v1))")) << source->str();
  }
}

TEST(MobileDalvikConstructorRegion, UnaryArgumentsKeepTypeAndReceiverWords) {
  for (const auto &[opcode, type] :
       std::vector<std::pair<std::string, std::string>>{{"not-int", "I"},
                                                        {"neg-int", "I"},
                                                        {"not-long", "J"},
                                                        {"neg-long", "J"},
                                                        {"neg-float", "F"},
                                                        {"neg-double", "D"}}) {
    const bool wide = width(type) == 2;
    for (bool super : {false, true}) {
      auto object = klass("Ljava/lang/Object;", {}, true);
      object.superclass.reset();
      object.access = {"public"};
      MethodRef objectInit{object.name, "<init>", {}, "V"};
      object.methods.push_back(method(objectInit, {"public", "constructor"}, 0));
      auto owner = klass("Lfixture/Values;");
      auto parent = klass("Lfixture/Base;");
      parent.access.erase("final");
      MethodRef init{super ? parent.name : owner.name, "<init>", {type}, "V"};
      auto target = method(init, {"public", "constructor"}, wide ? 3 : 2,
                           {op(0, "invoke-direct", {0}, objectInit),
                            op(1, "return-void")});
      if (super) {
        owner.superclass = parent.name;
        parent.methods.push_back(target);
      } else {
        owner.methods.push_back(target);
      }
      auto call = method(
          {owner.name, "<init>", {type, "Z"}, "V"}, {"public", "constructor"},
          wide ? 6 : 5,
          {op(0, opcode, {0, 3}),
           op(1, "invoke-direct", wide ? std::vector<unsigned>{2, 0, 1}
                                       : std::vector<unsigned>{2, 0}, init),
           op(2, "return-void")});
      for (bool overlap : {false, true}) {
        SCOPED_TRACE(::testing::PrintToString(std::tuple{opcode, super, overlap}));
        auto input = owner;
        if (overlap)
          call.instructions[0].registers[0] = wide ? 1 : 2;
        input.methods.push_back(call);
        Budget b;
        auto graph = linkClasses({input, parent, object}, b);
        if (overlap) {
          EXPECT_THROW(recoverJava(graph, b), std::runtime_error);
        } else {
          EXPECT_EQ(recoverJava(graph, b).getInteger("unrecovered_method_count"), 0);
        }
      }
    }
  }
}

TEST(MobileDalvikConstructorRegions, DiscardedAllocationKeepsNullArgumentDispatch) {
  const std::string object = "Ljava/lang/Object;", input = "Lfixture/Input;",
                    box = "Lfixture/Box;";
  const MethodRef read{input, "read", {}, object}, init{box, "<init>", {object}, "V"};
  auto api = klass(input, {method(read, {"public", "abstract"}, 0)}, true);
  api.access = {"public", "abstract", "interface"};
  auto target = klass(box, {method(init, {"public", "constructor"}, 0)}, true);
  for (int value : {0, 1}) {
    auto zero = op(2, "const/4", {0});
    zero.literal = int64_t(value);
    auto run = method({"Lfixture/Example;", "run", {}, object}, {"public", "static"}, 1,
                      {op(0, "new-instance", {0}, box), zero,
                       op(3, "invoke-interface", {0}, read), op(6, "throw", {0})});
    Budget budget;
    auto graph = linkClasses({klass(run.reference.owner, {run}), target, api}, budget);
    if (value) {
      EXPECT_THROW(recoverJava(graph, budget), std::runtime_error);
      continue;
    }
    const auto report = recoverJava(graph, budget);
    EXPECT_EQ(report.getInteger("unrecovered_method_count"), 0);
    const auto unit = report.getArray("source_units")->front().getAsObject();
    ASSERT_TRUE(unit);
    const auto source = unit->getString("source");
    ASSERT_TRUE(source);
    EXPECT_TRUE(source->contains("new fixture.Box("));
    EXPECT_TRUE(source->contains(".read()"));
  }
}

TEST(MobileDalvikConstructorRegions, JoinedReferenceArraysRetainLoadedElementTypes) {
  auto base = klass("Lfixture/Base;", {}, true);
  base.access = {"public"};
  auto left = klass("Lfixture/Left;", {}, true);
  auto right = klass("Lfixture/Right;", {}, true);
  left.superclass = base.name;
  right.superclass = base.name;
  auto branch = op(0, "if-eqz", {2});
  branch.target = 5;
  auto join = op(3, "goto");
  join.target = 6;
  auto index = op(6, "const/4", {1});
  index.literal = int64_t(0);
  auto run = method({"Lfixture/Arrays;", "first", {"Z", "[" + left.name, "[" + right.name},
                     base.name}, {"public", "static"}, 5,
                    {branch, op(2, "move-object", {0, 3}), join,
                     op(5, "move-object", {0, 4}), index,
                     op(7, "aget-object", {0, 0, 1}), op(9, "return-object", {0})});
  Budget budget;
  auto graph = linkClasses({klass(run.reference.owner, {run}), left, right, base}, budget);
  EXPECT_EQ(recoverJava(graph, budget).getInteger("unrecovered_method_count"), 0);
}

TEST(MobileDalvikConstructorRegions,
     AllocationBranchesKeepTheirOwnContinuation) {
  const std::string object = "Ljava/lang/Object;", box = "Lfixture/Box;",
                    observer = "Lfixture/Observer;";
  const MethodRef read{observer, "read", {}, object};
  const MethodRef observe{observer, "observe", {box}, "V"};
  const MethodRef shortInit{box, "<init>", {object}, "V"};
  const MethodRef longInit{box, "<init>", {object, object}, "V"};
  const auto target = klass(box,
                            {method(shortInit, {"public", "constructor"}, 0),
                             method(longInit, {"public", "constructor"}, 0)},
                            true);
  const auto effects = klass(observer,
                             {method(read, {"public", "static"}, 0),
                              method(observe, {"public", "static"}, 0)},
                             true);
  for (bool exposeUninitialized : {false, true}) {
    auto branch = op(3, "if-eqz", {2});
    branch.target = 7;
    auto run = method(
        {"Lfixture/Factory;", "make", {"Z", object}, box}, {"public", "static"},
        4,
        {op(0, "new-instance", {0}, box), op(1, "invoke-static", {}, read),
         op(2, "move-result-object", {1}), branch,
         op(4, "invoke-direct", {0, 1}, shortInit),
         op(5, "invoke-static", {0}, observe), op(6, "return-object", {0}),
         op(7, "invoke-direct", {0, 1, 3}, longInit),
         op(8, "return-object", {0})});
    if (exposeUninitialized)
      run.instructions[7].registers[2] = 0;
    Budget budget;
    auto graph = linkClasses(
        {klass(run.reference.owner, {run}), target, effects}, budget);
    if (exposeUninitialized) {
      EXPECT_THROW(recoverJava(graph, budget), std::runtime_error);
      continue;
    }
    const auto report = recoverJava(graph, budget);
    ASSERT_EQ(report.getInteger("unrecovered_method_count"), 0);
    const auto *source = report.getArray("source_units")->front().getAsObject();
    ASSERT_TRUE(source && source->getString("source"));
    const auto text = *source->getString("source");
    EXPECT_EQ(text.count("new fixture.Box("), 2u);
    EXPECT_EQ(text.count(".read()"), 2u);
    EXPECT_EQ(text.count(".observe("), 1u);
  }
}
