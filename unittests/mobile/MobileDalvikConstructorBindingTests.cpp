#include "MobileDalvik.h"
#include "MobileDalvikSignature.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <tuple>

using namespace neverd::mobile;
using namespace neverd::mobile::dalvik;
namespace {
Instruction op(unsigned PC, std::string Opcode,
               std::vector<unsigned> Registers = {}, Literal Value = {},
               Reference Ref = {}, std::optional<unsigned> Target = {}) {
  Instruction I;
  I.pc = PC;
  I.opcode = std::move(Opcode);
  I.registers = std::move(Registers);
  I.literal = std::move(Value);
  I.reference = std::move(Ref);
  I.target = Target;
  return I;
}
Method ctor(MethodRef Ref, unsigned Registers, std::vector<Instruction> Code) {
  Method M;
  M.reference = std::move(Ref);
  M.access = {"public", "constructor"};
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
std::vector<Class> fixture(bool Public, bool Nested, bool Range, bool Frame,
                           bool Alpha) {
  const std::string Name = Nested ? "Lfixture/Host$Core;" : "Lfixture/Core;";
  const std::string Marker = "Lkotlin/jvm/internal/DefaultConstructorMarker;";
  const std::vector<std::string> Args{"Ljava/lang/Object;", "I", "I",
                                      "Ljava/lang/String;"};
  auto Target = ctor({Name, "<init>", Args, "V"}, 5,
                     {op(0, "invoke-direct", {0}, {},
                         MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                      op(1, "return-void")});
  const std::string T = Alpha ? "Element" : "T";
  Target.generic_signature = "(T" + T + ";IILjava/lang/String;)V";
  auto Extended = Args;
  Extended.insert(Extended.end(), {"I", Marker});
  auto Mask = ctor({Name, "<init>", Extended, "V"}, 8,
                   {op(0, "and-int/lit8", {0, 6}, int64_t(4)),
                    op(1, "if-eqz", {0}, {}, {}, 3),
                    op(2, "const/high16", {4}, int64_t(-2147483648LL)),
                    op(3, "and-int/lit8", {0, 6}, int64_t(8)),
                    op(4, "if-eqz", {0}, {}, {}, 6),
                    op(5, "const-string", {5}, std::string("")),
                    op(6, Range ? "invoke-direct/range" : "invoke-direct",
                       {1, 2, 3, 4, 5}, {}, Target.reference),
                    op(7, "return-void")});
  Mask.access.insert("synthetic");
  auto C = klass(Name, {Target, Mask});
  C.access = Public ? Access{"public", "final"} : Access{"final"};
  C.generic_signature = "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;";
  if (Frame)
    for (auto &M : C.methods) {
      M.registers += 3;
      for (auto &I : M.instructions)
        for (auto &R : I.registers)
          R += 3;
    }
  auto Object = klass("Ljava/lang/Object;",
                      {ctor({"Ljava/lang/Object;", "<init>", {}, "V"}, 0, {})});
  Object.superclass.reset();
  Object.external = true;
  auto String = klass("Ljava/lang/String;");
  String.access.insert("final");
  String.external = true;
  auto M = klass(Marker);
  M.external = true;
  std::vector<Class> Input{C, Object, String, M};
  if (Nested) {
    auto Host = klass("Lfixture/Host;");
    Input[0].enclosing = Host.name;
    Input[0].inner_class_present = true;
    Input[0].inner_name = "Core";
    Input[0].inner_access = Public ? Access{"public", "static", "final"}
                                   : Access{"private", "static", "final"};
    Input.push_back(Host);
  }
  return Input;
}

std::vector<Class> closedVarargsFixture(bool Variadic, bool Range, bool Frame,
                                        bool External, bool DifferentArity,
                                        bool CallerVarargs = false) {
  const std::string Core = "Lfixture/Core;", Carrier = "Lfixture/Carrier;",
                    Leaf = "Lfixture/Leaf;", Object = "Ljava/lang/Object;";
  MethodRef Super{Object, "<init>", {}, "V"};
  auto Target =
      ctor({Core, "<init>", {Carrier}, "V"}, 2,
           {op(0, "invoke-direct", {0}, {}, Super),
            op(1, "iput-object", {1, 0}, {}, FieldRef{Core, "value", Object}),
            op(2, "return-void")});
  Target.generic_signature = "(Lfixture/Carrier<Lfixture/Leaf;>;)V";
  std::vector<std::string> Args =
      DifferentArity ? std::vector<std::string>{"I", "[Lfixture/Leaf;"}
                     : std::vector<std::string>{"[Lfixture/Leaf;"};
  auto Sibling = ctor({Core, "<init>", Args, "V"}, DifferentArity ? 3 : 2,
                      {op(0, "invoke-direct", {0}, {}, Super),
                       op(1, "iput-object", {DifferentArity ? 2u : 1u, 0}, {},
                          FieldRef{Core, "value", Object}),
                       op(2, "return-void")});
  if (Variadic)
    Sibling.access.insert("varargs");
  const unsigned Self = Frame ? (Range ? 20 : 10) : 3;
  std::vector<Instruction> C{op(0, "move-object/from16", {0, Self + 1}),
                             op(1, "check-cast", {0}, {}, Carrier)};
  if (Range) {
    C.push_back(op(C.size(), "move-object/from16", {1, Self}));
    C.push_back(op(C.size(), "move-object", {2, 0}));
  }
  C.push_back(
      op(C.size(), Range ? "invoke-direct/range" : "invoke-direct",
         Range ? std::vector<unsigned>{1, 2} : std::vector<unsigned>{Self, 0},
         {}, Target.reference));
  C.push_back(op(C.size(), "return-void"));
  auto Entry = ctor(
      {Core, "<init>", {Object, CallerVarargs ? "[Lfixture/Leaf;" : "I"}, "V"},
      Self + 3, std::move(C));
  if (CallerVarargs)
    Entry.access.insert("varargs");
  auto Owner = klass(Core, {Target, Sibling, Entry});
  Owner.access.insert("final");
  Owner.fields = {{{Core, "value", Object}, {"public", "final"}, {}}};
  auto Container = klass(Carrier);
  Container.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  Container.external = External;
  auto Element = klass(Leaf);
  Element.access.insert("final");
  Element.external = External;
  auto Root = klass(Object, {ctor(Super, 0, {})});
  Root.superclass.reset();
  Root.external = true;
  return {Owner, Container, Element, Root};
}

std::vector<Class> packageDefaultFixture(bool Public, unsigned TailKind,
                                         bool Synthetic, bool Declared,
                                         bool Frame, bool Range,
                                         bool External) {
  const std::string Core = "Lfixture/Hidden;", Carrier = "Lfixture/Carrier;",
                    Object = "Ljava/lang/Object;";
  const std::string Tail =
      TailKind == 0 ? Core
                    : (TailKind == 2 || TailKind == 4 ? "Lother/Link;"
                                                      : "Lfixture/Link;");
  MethodRef Super{Object, "<init>", {}, "V"},
      Read{Carrier, "current", {}, Object};
  Field State{FieldRef{Core, "state", Carrier}, {"private", "final"}, {}};
  State.generic_signature = "Lfixture/Carrier<Ljava/lang/Object;>;";
  Field Next{FieldRef{Core, "next", Tail}, {"private", "final"}, {}};
  Field Initial{FieldRef{Core, "initial", Object}, {"private", "final"}, {}};
  auto Primary =
      ctor({Core, "<init>", {Carrier, Tail}, "V"}, 4,
           {op(0, "invoke-direct", {1}, {}, Super),
            op(1, "iput-object", {2, 1}, {}, State.reference),
            op(2, "iput-object", {3, 1}, {}, Next.reference),
            op(3, "iget-object", {0, 1}, {}, State.reference),
            op(4, Range ? "invoke-interface/range" : "invoke-interface", {0},
               {}, Read),
            op(5, "move-result-object", {0}),
            op(6, "iput-object", {0, 1}, {}, Initial.reference),
            op(7, "return-void")});
  Primary.generic_signature =
      "(Lfixture/Carrier<+Ljava/lang/Object;>;" + Tail + ")V";
  auto Mask =
      ctor({Core, "<init>", {Carrier, Tail, "I", Object}, "V"}, 5,
           {op(0, "and-int/lit8", {3, 3}, int64_t(2)),
            op(1, "if-eqz", {3}, {}, {}, 3), op(2, "const/4", {2}, int64_t(0)),
            op(3, Range ? "invoke-direct/range" : "invoke-direct", {0, 1, 2},
               {}, Primary.reference),
            op(4, "return-void")});
  if (Synthetic)
    Mask.access.insert("synthetic");
  if (Declared)
    Mask.generic_signature = "(Lfixture/Carrier<+Ljava/lang/Object;>;" + Tail +
                             "ILjava/lang/Object;)V";
  auto Owner = klass(Core, {Primary, Mask});
  Owner.access = Public ? Access{"public", "final"} : Access{"final"};
  Owner.fields = {State, Next, Initial};
  if (Frame)
    for (auto &M : Owner.methods) {
      M.registers += 3;
      for (auto &I : M.instructions)
        for (auto &R : I.registers)
          R += 3;
    }
  auto Get = ctor(Read, 0, {});
  Get.access = {"public", "abstract"};
  Get.generic_signature = "()TValue;";
  auto Provider = klass(Carrier, {Get});
  Provider.access = {"public", "interface", "abstract"};
  Provider.generic_signature = "<Value:Ljava/lang/Object;>Ljava/lang/Object;";
  Provider.external = External;
  auto Root = klass(Object, {ctor(Super, 0, {})});
  Root.external = true;
  Root.superclass.reset();
  std::vector<Class> Input{Owner, Provider, Root};
  if (TailKind) {
    auto Link = klass(Tail);
    Link.access = TailKind == 2 ? Access{"public", "final"} : Access{"final"};
    Link.external = TailKind == 2 || TailKind == 3;
    Input.push_back(std::move(Link));
  }
  return Input;
}

ClassMap classes(std::vector<Class> Input, unsigned Route, Budget &B) {
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

TEST(MobileDalvikConstructorBinding,
     NonpublicObjectOwnersKeepRawMaskDelegation) {
  for (bool Public : {false, true})
    for (bool Nested : {false, true})
      for (bool Range : {false, true})
        for (bool Frame : {false, true})
          for (bool Alpha : {false, true})
            for (bool Composite : {false, true})
              for (unsigned Route = 0; Route < 3; ++Route) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    Public, Nested, Range, Frame, Alpha, Composite, Route}));
                auto Input = fixture(Public, Nested, Range, Frame, Alpha);
                if (Composite)
                  Input[0].methods[1].instructions[0].literal = int64_t(3);
                const auto Target = Input[0].methods[0].reference;
                const auto Mask = Input[0].methods[1].reference;
                Budget B;
                auto Classes = classes(Input, Route, B);
                const auto Plans = validateGenericSignatures(Classes, B);
                EXPECT_TRUE(Plans.constructor_calls.contains({Mask, Target}));
                const auto Report = recoverJava(Classes, B);
                EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                EXPECT_EQ(Report.getInteger("minimum_java_release"), 25);
                EXPECT_FALSE(Report.getArray("generic_signature_projections"));
              }
}

TEST(MobileDalvikConstructorBinding,
     NonpublicMaskOwnersRejectIncompleteContracts) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 24; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = fixture(false, false, true, false, false);
      auto &C = Input[0];
      auto &Target = C.methods[0];
      auto &Mask = C.methods[1];
      if (Mutation == 0)
        C.access.clear();
      else if (Mutation == 1)
        C.generic_signature.reset();
      else if (Mutation == 2)
        C.generic_signature = "<U:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 3)
        C.generic_signature = "<T:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 4)
        Target.generic_signature =
            "<T:Ljava/lang/Object;>(TT;IILjava/lang/String;)V";
      else if (Mutation == 5)
        Target.access.insert("varargs");
      else if (Mutation == 6)
        Target.access.insert("synthetic");
      else if (Mutation == 7)
        Target.access.insert("static");
      else if (Mutation == 8)
        Mask.access.erase("synthetic");
      else if (Mutation == 9)
        Mask.generic_signature =
            "(Ljava/lang/Object;IILjava/lang/String;ILkotlin/jvm/internal/"
            "DefaultConstructorMarker;)V";
      else if (Mutation == 10)
        Mask.instructions[6].registers[0] = 0;
      else if (Mutation == 11)
        Mask.instructions[6].registers.pop_back();
      else if (Mutation == 12)
        Mask.instructions[6].registers[1] = 6;
      else if (Mutation == 13)
        Mask.instructions[1].target = 7;
      else if (Mutation == 14)
        Mask.instructions[0].literal = std::string("invalid");
      else if (Mutation == 15)
        Mask.instructions[2].registers[0] = 1;
      else if (Mutation == 16)
        Mask.instructions[5].registers[0] = 1;
      else if (Mutation == 17)
        Mask.tries = {{0, 6, {{std::nullopt, 7}}}};
      else if (Mutation == 18)
        Mask.instructions.back().registers = {0};
      else if (Mutation == 19)
        Mask.instructions[0].opcode = "and-unknown/lit8";
      else if (Mutation == 20)
        C.interfaces = {"Lmissing/Interface;"};
      else if (Mutation == 21)
        C.superclass = "Lmissing/Parent;";
      else if (Mutation == 22)
        Input[3].name = "Lother/Marker;";
      else
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> typedStringFixture(bool Public, bool Nested, bool Range,
                                      bool Frame, bool Alpha, bool Copy,
                                      bool Jumbo, bool Bounded, bool Array,
                                      std::string Context, bool Nonempty) {
  auto Input = fixture(Public, Nested, Range, false, Alpha);
  auto &C = Input[0];
  auto Target = C.methods[0];
  const std::string T = Alpha ? "Element" : "T";
  const std::string Bound =
      Bounded ? "Ljava/lang/Number;" : "Ljava/lang/Object;";
  const std::string Argument = (Array ? "[" : "") + Bound;
  const std::string Typed = (Array ? "[T" : "T") + T + ";";
  C.generic_signature = "<" + T + ":" + Bound + ">Ljava/lang/Object;";
  Target.reference.parameters[0] = Argument;
  Target.reference.parameters[3] = Context;
  Target.generic_signature = "(" + Typed + "II" + Context + ")V";
  const unsigned Self = Range ? (Copy ? 6 : 5) : (Copy ? 2 : 1);
  const unsigned LiteralSlot = Range ? (Copy ? 5 : 4) : 0;
  std::vector<Instruction> Code{
      op(0, Jumbo ? "const-string/jumbo" : "const-string", {LiteralSlot},
         std::string(Nonempty ? "owned-context" : ""))};
  if (Copy)
    Code.push_back(
        op(Code.size(), "move-object", {Range ? 4u : 1u, LiteralSlot}));
  std::vector<unsigned> Args{Self, Self + 1, Self + 2, Self + 3,
                             Copy ? 1u : 0u};
  if (Range) {
    for (unsigned I = 0; I < 4; ++I)
      Code.push_back(
          op(Code.size(), I < 2 ? "move-object" : "move", {I, Self + I}));
    Args = {0, 1, 2, 3, 4};
  }
  Code.push_back(op(Code.size(),
                    Range ? "invoke-direct/range" : "invoke-direct", Args, {},
                    Target.reference));
  Code.push_back(op(Code.size(), "return-void"));
  auto Caller =
      ctor({C.name, "<init>", {Argument, "I", "I"}, "V"}, Self + 4, Code);
  Caller.generic_signature = "(" + Typed + "II)V";
  C.methods = {Target, Caller};
  Input[2].interfaces = {"Ljava/lang/CharSequence;"};
  auto Sequence = klass("Ljava/lang/CharSequence;");
  Sequence.access = {"public", "abstract", "interface"};
  Sequence.external = true;
  Input.push_back(Sequence);
  auto Number = klass("Ljava/lang/Number;");
  Number.access.insert("abstract");
  Number.external = true;
  Input.push_back(Number);
  if (Frame)
    for (auto &M : Input[0].methods) {
      M.registers += 3;
      for (auto &I : M.instructions)
        for (auto &R : I.registers)
          R += 3;
    }
  return Input;
}
} // namespace

TEST(MobileDalvikConstructorBinding, TypedStringsKeepOriginalInputProvenance) {
  const std::vector<std::vector<bool>> Cases{
      {false, false, false, false, false, false, false, false, false},
      {true, false, false, false, false, false, false, false, false},
      {false, true, false, false, false, false, false, false, false},
      {true, true, true, true, true, true, true, true, true},
      {true, false, true, false, false, true, false, true, false},
      {false, false, false, true, false, true, true, false, true},
      {true, false, false, false, true, false, true, true, false},
      {false, true, true, true, true, false, false, false, true}};
  for (unsigned Case = 0; Case < Cases.size(); ++Case)
    for (const std::string Context :
         {"Ljava/lang/String;", "Ljava/lang/Object;",
          "Ljava/lang/CharSequence;"})
      for (bool Nonempty : {false, true})
        for (unsigned Route = 0; Route < 3; ++Route) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{Case, Context, Nonempty, Route}));
          const auto &V = Cases[Case];
          auto Input = typedStringFixture(V[0], V[1], V[2], V[3], V[4], V[5],
                                          V[6], V[7], V[8], Context, Nonempty);
          const auto Target = Input[0].methods[0].reference;
          const auto Caller = Input[0].methods[1].reference;
          Budget B;
          auto Classes = classes(Input, Route, B);
          const auto Plans = validateGenericSignatures(Classes, B);
          EXPECT_TRUE(Plans.constructor_calls.contains({Caller, Target}));
          const auto Report = recoverJava(Classes, B);
          EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
          EXPECT_EQ(Report.getInteger("minimum_java_release").value_or(8), 8);
          EXPECT_FALSE(Report.getArray("generic_signature_projections"));
        }
}

TEST(MobileDalvikConstructorBinding,
     TypedStringsRejectUnprovenContextBindings) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 32; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input =
          typedStringFixture(true, false, false, false, false, Mutation == 25,
                             false, false, false, "Ljava/lang/String;", false);
      auto &C = Input[0];
      auto &Target = C.methods[0];
      auto &Caller = C.methods[1];
      auto &Literal = Caller.instructions[0];
      auto &Call = Caller.instructions[Caller.instructions.size() - 2];
      const unsigned Self = Caller.registers - Caller.incomingWords();
      if (Mutation == 0)
        Literal.opcode = "const-unknown";
      else if (Mutation == 1)
        Literal.literal = std::monostate{};
      else if (Mutation == 2)
        Literal.reference = std::string("Ljava/lang/String;");
      else if (Mutation == 3)
        Literal.registers[0] = Self;
      else if (Mutation == 4)
        Literal.registers[0] = Self + 1;
      else if (Mutation == 5)
        Literal.registers[0] = Caller.registers;
      else if (Mutation == 6)
        Literal.pc = Caller.code_end;
      else if (Mutation == 7)
        Literal.target = 0;
      else if (Mutation == 8)
        Literal.targets = {1};
      else if (Mutation == 9)
        Literal.keys = {1};
      else if (Mutation == 10)
        Literal.data = {1};
      else if (Mutation == 11)
        Literal.element_width = 1;
      else if (Mutation == 12)
        Literal.registers.push_back(0);
      else if (Mutation == 13)
        Caller.generic_signature.reset();
      else if (Mutation == 14)
        Caller.generic_signature = "<T:Ljava/lang/Object;>(TT;II)V";
      else if (Mutation == 15)
        Target.generic_signature =
            "<T:Ljava/lang/Object;>(TT;IILjava/lang/String;)V";
      else if (Mutation == 16)
        Target.generic_signature = "(TT;IITT;)V";
      else if (Mutation == 17) {
        Target.reference.parameters[3] = "[Ljava/lang/String;";
        Target.generic_signature = "(TT;II[Ljava/lang/String;)V";
        Call.reference = Target.reference;
      } else if (Mutation == 18) {
        Target.reference.parameters[3] = "Ljava/util/List;";
        Target.generic_signature = "(TT;IILjava/util/List<TT;>;)V";
        Call.reference = Target.reference;
      } else if (Mutation == 19)
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 20)
        Caller.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 21)
        Call.registers[0] = 0;
      else if (Mutation == 22)
        Call.registers.pop_back();
      else if (Mutation == 23)
        Call.registers[1] = 0;
      else if (Mutation == 24)
        Call.registers.back() = Self + 1;
      else if (Mutation == 25)
        Caller.instructions[1].registers[1] = Self + 2;
      else if (Mutation == 26) {
        Caller.instructions.insert(Caller.instructions.begin() + 1,
                                   op(1, "const/4", {0}, int64_t(1)));
        for (unsigned I = 0; I < Caller.instructions.size(); ++I)
          Caller.instructions[I].pc = I;
        Caller.code_end = Caller.instructions.size();
      } else if (Mutation == 27)
        Call.opcode = "invoke-static";
      else if (Mutation == 28)
        Target.generic_signature = "(TT;IILjava/lang/String;)";
      else if (Mutation == 29)
        C.generic_signature = "<U:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 30)
        C.generic_signature = "<T:Ljava/lang/Number;>Ljava/lang/Object;";
      else
        Caller.tries = {{0, 1, {{std::nullopt, Caller.code_end - 1}}}};
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}

TEST(MobileDalvikConstructorBinding,
     ClosedFixedTargetsPrecedeVarargsExpansion) {
  for (bool Variadic : {false, true})
    for (bool Range : {false, true})
      for (bool Frame : {false, true})
        for (bool External : {false, true})
          for (bool DifferentArity : {false, true})
            for (bool CallerVarargs : {false, true})
              for (unsigned Route = 0; Route < 3; ++Route) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{Variadic, Range, Frame, External, DifferentArity,
                               CallerVarargs, Route}));
                auto Input =
                    closedVarargsFixture(Variadic, Range, Frame, External,
                                         DifferentArity, CallerVarargs);
                auto Target = Input[0].methods[0].reference,
                     Entry = Input[0].methods[2].reference;
                Budget B;
                auto Map = classes(std::move(Input), Route, B);
                auto Plans = validateGenericSignatures(Map, B);
                EXPECT_TRUE(Plans.constructor_delegations.contains(Entry));
                EXPECT_TRUE(Plans.constructor_calls.contains({Entry, Target}));
                llvm::json::Object R;
                ASSERT_NO_THROW(R = recoverJava(Map, B));
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_EQ(R.getInteger("minimum_java_release"), 25);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
              }
}
TEST(MobileDalvikConstructorBinding,
     ClosedVarargsSiblingsKeepGenericAndReceiverProofs) {
  for (unsigned Mutation = 0; Mutation < 18; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input = closedVarargsFixture(true, true, false, false, false);
      auto &Owner = Input[0];
      auto &Target = Owner.methods[0];
      auto &Sibling = Owner.methods[1];
      auto &Entry = Owner.methods[2];
      switch (Mutation) {
      case 0:
        Owner.access.erase("final");
        Owner.access.insert("abstract");
        break;
      case 1:
        Owner.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 2:
        Target.generic_signature =
            "<T:Ljava/lang/Object;>(Lfixture/Carrier<TT;>;)V";
        break;
      case 3:
        Entry.generic_signature = "<T:Ljava/lang/Object;>(TT;I)V";
        break;
      case 4:
        Sibling.generic_signature = "<T:Lfixture/Leaf;>([TT;)V";
        break;
      case 5:
        Sibling.reference.parameters[0] = "Lfixture/Leaf;";
        break;
      case 6:
        Target.access.insert("varargs");
        break;
      case 7:
        Input.erase(Input.begin() + 2);
        break;
      case 8:
        Target.generic_signature = "(Lfixture/Carrier<Lmissing/Leaf;>;)V";
        break;
      case 9:
        Entry.instructions[1].registers[0] = 3;
        break;
      case 10:
        Entry.instructions[4].registers.pop_back();
        break;
      case 11:
        Entry.instructions[2].registers[1] = 4;
        break;
      case 12:
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
        break;
      case 13:
        Target.generic_signature = "(Ljava/lang/Object;)V";
        break;
      case 14:
        Input[1].generic_signature = "<E:Ljava/lang/Number;>Ljava/lang/Object;";
        break;
      case 15:
        Entry.instructions[1].registers[0] = Entry.registers;
        break;
      case 16:
        Entry.access.insert("varargs");
        break;
      case 17:
        Entry.access.insert("varargs");
        Entry.reference.parameters[1] = "[Lfixture/Leaf;";
        Entry.generic_signature = "<T:Lfixture/Leaf;>(Ljava/lang/Object;[TT;)V";
        break;
      }
      Budget B;
      EXPECT_THROW(recoverJava(classes(std::move(Input), Route, B), B),
                   std::runtime_error);
    }
}

TEST(MobileDalvikConstructorBinding,
     PackageOwnersKeepClosedDefaultConstructorScopes) {
  for (bool Public : {true, false})
    for (unsigned TailKind : {0u, 1u, 2u})
      for (bool Synthetic : {false, true})
        for (bool Declared : {false, true})
          for (bool Frame : {false, true})
            for (bool Range : {false, true})
              for (bool External : {false, true})
                for (unsigned Route = 0; Route < 3; ++Route) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{Public, TailKind, Synthetic, Declared, Frame,
                                 Range, External, Route}));
                  auto Input =
                      packageDefaultFixture(Public, TailKind, Synthetic,
                                            Declared, Frame, Range, External);
                  const auto Target = Input[0].methods[0].reference,
                             Caller = Input[0].methods[1].reference;
                  Budget B;
                  auto Map = classes(std::move(Input), Route, B);
                  auto Plans = validateGenericSignatures(Map, B);
                  EXPECT_TRUE(Plans.constructor_delegations.contains(Caller));
                  EXPECT_TRUE(
                      Plans.constructor_calls.contains({Caller, Target}));
                  llvm::json::Object R;
                  ASSERT_NO_THROW(R = recoverJava(Map, B));
                  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                  EXPECT_EQ(R.getInteger("minimum_java_release"), 25);
                  EXPECT_FALSE(R.getArray("generic_signature_projections"));
                }
}
TEST(MobileDalvikConstructorBinding,
     PackageDefaultsRejectForeignAndUninitializedBindings) {
  for (unsigned Mutation = 0; Mutation < 17; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input =
          packageDefaultFixture(false,
                                Mutation == 0   ? 3u
                                : Mutation == 1 ? 4u
                                                : 0u,
                                true, false, false, true, Mutation == 16);
      auto &Owner = Input[0];
      auto &Target = Owner.methods[0];
      auto &Caller = Owner.methods[1];
      switch (Mutation) {
      case 0:
      case 1:
        break;
      case 2:
        Owner.access.insert("private");
        break;
      case 3:
        Owner.access.insert("protected");
        break;
      case 4:
        Owner.access.erase("final");
        Owner.access.insert("abstract");
        break;
      case 5:
        Owner.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 6:
        Target.generic_signature =
            "<T:Ljava/lang/Object;>(Lfixture/Carrier<TT;>;Lfixture/Hidden;)V";
        break;
      case 7:
        Caller.generic_signature =
            "<T:Ljava/lang/Object;>(Lfixture/Carrier<TT;>;Lfixture/"
            "Hidden;ILjava/lang/Object;)V";
        break;
      case 8:
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
        break;
      case 9:
        Input[1].generic_signature =
            "<Value:Ljava/lang/Number;>Ljava/lang/Object;";
        break;
      case 10:
        Input[1].generic_signature.reset();
        break;
      case 11:
        Caller.instructions[2].registers[0] = 0;
        break;
      case 12:
        Caller.instructions[3].registers.pop_back();
        break;
      case 13:
        Caller.instructions[3].registers = {0, 1, 3};
        break;
      case 14:
        Caller.instructions[1].target = Caller.instructions.back().pc;
        break;
      case 15:
        Caller.instructions[0].registers[0] = Caller.registers;
        break;
      case 16:
        Input[1].access.erase("public");
        break;
      }
      Budget B;
      EXPECT_THROW(recoverJava(classes(std::move(Input), Route, B), B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> fixedHeaderBoundedFixture(bool Parent, bool Interface,
                                             bool Package, bool SDK, bool Range,
                                             bool Frame, bool Nominal,
                                             bool Alpha) {
  auto Input = fixture(true, false, Range, Frame, Alpha);
  auto &Owner = Input[0];
  const std::string Bound = Nominal ? "Lfixture/Bound;" : "Ljava/lang/Object;";
  const std::string Super = Parent ? "Lfixture/Parent;" : "Ljava/lang/Object;";
  const std::string Tag = "Lfixture/ObserverTag;";
  const std::string Formal = Alpha ? "Element" : "T";
  Owner.superclass = Super;
  Owner.interfaces =
      Interface ? std::vector<std::string>{Tag} : std::vector<std::string>{};
  Owner.generic_signature =
      "<" + Formal + ":" + Bound + ">" + Super + (Interface ? Tag : "");
  auto &Target = Owner.methods[0];
  Target.access = {"private", "constructor"};
  Target.reference.parameters[0] = Bound;
  Target.instructions[0].reference = MethodRef{Super, "<init>", {}, "V"};
  auto &Mask = Owner.methods[1];
  Mask.reference.parameters[0] = Bound;
  if (Package)
    Mask.access.erase("public");
  Mask.instructions[Mask.instructions.size() - 2].reference = Target.reference;
  auto Base = klass("Lfixture/Bound;");
  Base.external = SDK;
  auto P = klass("Lfixture/Parent;",
                 {ctor({"Lfixture/Parent;", "<init>", {}, "V"}, 1,
                       {op(0, "invoke-direct", {0}, {},
                           MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                        op(1, "return-void")})});
  P.external = SDK;
  if (SDK) {
    P.methods[0].instructions.clear();
    P.methods[0].registers = P.methods[0].code_end = 0;
  }
  auto Marker = klass(Tag);
  Marker.access = {"public", "interface", "abstract"};
  Marker.external = SDK;
  Input.push_back(Base);
  Input.push_back(P);
  Input.push_back(Marker);
  return Input;
}
} // namespace

TEST(MobileDalvikConstructorBinding, FixedHeadersKeepBoundedSameOwnerMasks) {
  for (bool Parent : {true, false})
    for (bool Interface : {true, false})
      for (bool Package : {true, false})
        for (bool SDK : {false, true})
          for (bool Range : {false, true})
            for (bool Frame : {false, true})
              for (bool Nominal : {true, false})
                for (bool Alpha : {false, true})
                  for (unsigned Route = 0; Route < 3; ++Route) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{Parent, Interface, Package, SDK, Range,
                                   Frame, Nominal, Alpha, Route}));
                    auto Input = fixedHeaderBoundedFixture(
                        Parent, Interface, Package, SDK, Range, Frame, Nominal,
                        Alpha);
                    const auto Target = Input[0].methods[0].reference;
                    const auto Caller = Input[0].methods[1].reference;
                    const auto Original = Input[0].generic_signature;
                    Budget B;
                    const auto Classes = classes(std::move(Input), Route, B);
                    const auto Plans = validateGenericSignatures(Classes, B);
                    ASSERT_TRUE(
                        Plans.constructor_calls.contains({Caller, Target}));
                    const auto R = recoverJava(Classes, B);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                    EXPECT_EQ(Classes.at(Target.owner).generic_signature,
                              Original);
                  }
}

TEST(MobileDalvikConstructorBinding,
     FixedBoundedMasksRejectUnknownHeadersAndAccess) {
  for (unsigned Mutation = 0; Mutation < 13; ++Mutation)
    for (unsigned Route = 0; Route < 3; ++Route) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Mutation, Route}));
      auto Input = fixedHeaderBoundedFixture(true, true, true, true, false,
                                             false, true, false);
      auto &Owner = Input[0];
      auto &Target = Owner.methods[0];
      auto &Caller = Owner.methods[1];
      switch (Mutation) {
      case 0:
        Input[5].generic_signature = "<X:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 1:
        Input[6].generic_signature = "<X:Ljava/lang/Object;>Ljava/lang/Object;";
        break;
      case 2:
        Owner.superclass = "Lmissing/Parent;";
        Owner.generic_signature =
            "<T:Lfixture/Bound;>Lmissing/Parent;Lfixture/ObserverTag;";
        break;
      case 3:
        Input[5].access = {"public", "interface", "abstract"};
        break;
      case 4:
        Owner.generic_signature =
            "<T:Lmissing/Bound;>Lfixture/Parent;Lfixture/ObserverTag;";
        break;
      case 5:
        Owner.access.erase("public");
        break;
      case 6:
        Owner.access.erase("final");
        break;
      case 7:
        Caller.access.erase("synthetic");
        break;
      case 8:
        Caller.generic_signature = Caller.reference.signature();
        break;
      case 9:
        Target.access = {"protected", "constructor"};
        break;
      case 10:
        Target.access.insert("varargs");
        break;
      case 11:
        Target.generic_signature =
            "<X:Ljava/lang/Object;>(TT;IILjava/lang/String;)V";
        break;
      case 12:
        Caller.instructions[Caller.instructions.size() - 2].opcode =
            "invoke-static";
        break;
      }
      Budget B;
      if (Mutation == 5) {
        // Package visibility preserves this independent owner's formals and
        // every known nominal ancestor. Check the newly supported contract;
        // all other malformed headers and invocation roles still fail below.
        const auto TargetRef = Target.reference;
        const auto CallerRef = Caller.reference;
        const auto Original = Owner.generic_signature;
        auto Map = classes(std::move(Input), Route, B);
        const auto Plans = validateGenericSignatures(Map, B);
        ASSERT_TRUE(Plans.constructor_calls.contains({CallerRef, TargetRef}));
        ASSERT_TRUE(Plans.constructor_delegations.contains(CallerRef));
        EXPECT_EQ(Plans.constructor_delegations.at(CallerRef), TargetRef);
        EXPECT_FALSE(Map.at(TargetRef.owner).access.contains("public"));
        const auto R = recoverJava(Map, B);
        EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
        EXPECT_FALSE(R.getArray("generic_signature_projections"));
        EXPECT_EQ(Map.at(TargetRef.owner).generic_signature, Original);
      } else
        EXPECT_THROW(recoverJava(classes(std::move(Input), Route, B), B),
                     std::runtime_error);
    }
}

TEST(MobileDalvikConstructorBinding,
     ExtensibleOwnersKeepClosedVarargsAndPackageBindings) {
  for (unsigned Variant : {0u, 1u})
    for (unsigned Route : {0u, 1u, 2u}) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Variant, Route}));
      auto Input =
          Variant
              ? packageDefaultFixture(false, 0, true, false, false, true, false)
              : closedVarargsFixture(true, true, false, false, false);
      Input[0].access.erase("final");
      const auto Caller = Input[0].methods[Variant ? 1 : 2].reference;
      const auto Target = Input[0].methods[0].reference;
      const auto Signature = Input[0].methods[0].generic_signature;
      Budget B;
      const auto Map = classes(std::move(Input), Route, B);
      const auto Plans = validateGenericSignatures(Map, B);
      EXPECT_EQ(Plans.constructor_delegations.at(Caller), Target);
      EXPECT_TRUE(Plans.constructor_calls.contains({Caller, Target}));
      EXPECT_FALSE(Map.at(Target.owner).access.contains("final"));
      const auto &Methods = Map.at(Target.owner).methods;
      const auto Declaration =
          std::find_if(Methods.begin(), Methods.end(),
                       [&](const auto &M) { return M.reference == Target; });
      ASSERT_NE(Declaration, Methods.end());
      EXPECT_EQ(Declaration->generic_signature, Signature);
      const auto R = recoverJava(Map, B);
      EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
      EXPECT_FALSE(R.getArray("generic_signature_projections"));
    }
}

namespace {
std::vector<Class> mixedStarConstructorFixture(bool SDK, bool Range,
                                               bool Reverse, bool Nested,
                                               const std::string &Variance,
                                               int64_t Value) {
  const std::string Name = "Lfixture/Closed;", Carrier = "Lfixture/Carrier;",
                    Nominal = "Ljava/lang/CharSequence;";
  const auto Argument = Variance + Nominal;
  auto Type =
      "Lfixture/Carrier<" + (Reverse ? "*" + Argument : Argument + "*") + ">;";
  if (Nested)
    Type = "Lfixture/Carrier<" + Type + "*>;";
  const MethodRef RootInit{"Ljava/lang/Object;", "<init>", {}, "V"};
  auto Target =
      ctor({Name, "<init>", {Carrier, "Z"}, "V"}, 3,
           {op(0, "invoke-direct", {0}, {}, RootInit), op(1, "return-void")});
  Target.generic_signature = "(" + Type + "Z)V";
  auto Integer = Target;
  Integer.reference.parameters[1] = "I";
  Integer.generic_signature = "(" + Type + "I)V";
  const unsigned Self = Range ? 32 : 2;
  auto Caller = ctor({Name, "<init>", {Carrier}, "V"}, Self + 2, {});
  Caller.generic_signature = "(" + Type + ")V";
  if (Range)
    Caller.instructions = {
        op(0, "move-object/from16", {0, Self}),
        op(1, "move-object/from16", {1, Self + 1}),
        op(2, "const/4", {2}, Value),
        op(3, "invoke-direct/range", {0, 1, 2}, {}, Target.reference),
        op(4, "return-void")};
  else
    Caller.instructions = {
        op(0, "const/4", {0}, Value),
        op(1, "invoke-direct", {Self, Self + 1, 0}, {}, Target.reference),
        op(2, "return-void")};
  Caller.code_end = Caller.instructions.back().pc + 1;
  auto Core = klass(Name, {Target, Caller, Integer});
  auto Provider = klass(Carrier);
  Provider.access = {"public", "interface", "abstract"};
  Provider.generic_signature =
      "<K:Ljava/lang/Object;V:Ljava/lang/Object;>Ljava/lang/Object;";
  Provider.external = SDK;
  auto Text = klass(Nominal);
  Text.access = {"public", "interface", "abstract"};
  Text.external = true;
  auto Root = klass("Ljava/lang/Object;", {ctor(RootInit, 0, {})});
  Root.external = true;
  Root.superclass.reset();
  return {Core, Provider, Text, Root};
}
} // namespace

TEST(MobileDalvikConstructorBinding,
     MixedClosedStarsKeepOriginalDelegationAndPrimitiveOverload) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Reverse : {false, true})
        for (bool Nested : {false, true})
          for (const std::string Variance : {"", "+", "-"})
            for (int64_t Value : {int64_t(0), int64_t(1)})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, Range, Reverse, Nested, Variance, Value, Route}));
                auto Input = mixedStarConstructorFixture(
                    SDK, Range, Reverse, Nested, Variance, Value);
                auto Target = Input[0].methods[0], Caller = Input[0].methods[1];
                EXPECT_NO_THROW(([&] {
                  Budget B;
                  auto Classes = classes(Input, Route, B);
                  auto Plans = validateGenericSignatures(Classes, B);
                  EXPECT_TRUE(Plans.constructor_calls.contains(
                      {Caller.reference, Target.reference}));
                  auto Report = recoverJava(Classes, B);
                  EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                  EXPECT_EQ(
                      Report.getInteger("projected_method_count").value_or(0),
                      0);
                  EXPECT_FALSE(
                      Report.getArray("generic_signature_projections"));
                  EXPECT_EQ(Classes.at(Caller.reference.owner)
                                .methods[1]
                                .generic_signature,
                            Caller.generic_signature);
                })());
              }
}
TEST(MobileDalvikConstructorBinding,
     MixedClosedStarsRejectMissingDeclarationsBoundsScopesAndOrigins) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 16; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Input =
            mixedStarConstructorFixture(SDK, true, false, true, "+", 0);
        auto &Target = Input[0].methods[0], &Caller = Input[0].methods[1];
        auto Replace = [&](Method &M, const std::string &From,
                           const std::string &To) {
          auto &S = *M.generic_signature;
          auto At = S.find(From);
          ASSERT_NE(At, std::string::npos);
          S.replace(At, From.size(), To);
        };
        switch (Mutation) {
        case 0:
          Replace(Caller, "Ljava/lang/CharSequence;", "TMissing;");
          break;
        case 1:
          Replace(Target, "Ljava/lang/CharSequence;", "Lfixture/Missing;");
          break;
        case 2:
          Replace(Target, ";*>", ";>");
          break;
        case 3:
          Replace(Target, ";*>", ";**>");
          break;
        case 4:
          Input[1].generic_signature =
              "<K:Ljava/lang/Number;V:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 5:
          Input[1].access = {"private", "interface", "abstract"};
          break;
        case 6:
          Input.erase(Input.begin() + 1);
          break;
        case 7:
          Target.access.insert("native");
          break;
        case 8:
          Target.access.insert("abstract");
          break;
        case 9:
          Target.access.insert("varargs");
          break;
        case 10:
          Caller.instructions[2].opcode = "const-string";
          Caller.instructions[2].literal = std::string("owned-reference");
          break;
        case 11:
          Caller.instructions[2].registers = {0};
          break;
        case 12:
          Caller.instructions[3].registers.pop_back();
          break;
        case 13:
          Caller.instructions[3].registers.back() = Caller.registers;
          break;
        case 14:
          Caller.generic_signature =
              "<X:Ljava/lang/Object;>(Lfixture/Carrier<+TX;*>;)V";
          break;
        case 15:
          Replace(Target, "Lfixture/Carrier<", "Ljava/util/Map<");
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(classes(Input, Route, B), B);
                     })(),
                     std::runtime_error);
      }
}
