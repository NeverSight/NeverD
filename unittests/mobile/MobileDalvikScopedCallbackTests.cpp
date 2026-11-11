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
Class klass(std::string Name, std::vector<Method> Methods = {}) {
  Class C;
  C.name = std::move(Name);
  C.superclass = "Ljava/lang/Object;";
  C.access = {"public"};
  C.source_id = "owned-fixture";
  C.methods = std::move(Methods);
  return C;
}
std::vector<Class> fixture(bool Public, bool Nested, std::string Scalar,
                           bool Range, bool Frame, bool Alpha,
                           unsigned Variance, unsigned Aux = 1) {
  const std::string Name = Nested ? "Lfixture/Host$Core;" : "Lfixture/Core;";
  const std::string T = Alpha ? "Input" : "T";
  const std::string R = Alpha ? "Output" : "R";
  Method Constructor;
  Constructor.reference = {Name, "<init>", {"Ljava/lang/Object;"}, "V"};
  Constructor.access = {"public", "constructor"};
  Constructor.generic_signature = "(T" + T + ";)V";
  Constructor.registers = 2;
  Constructor.instructions = {
      op(0, "invoke-direct", {0}, {},
         MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
      op(1, "return-void")};
  Constructor.code_end = 2;
  Method Target;
  std::vector<std::string> Args{"Lfixture/Mapper;"};
  std::string ScalarSignature;
  for (unsigned I = 0; I < Aux; ++I) {
    Args.push_back(Scalar);
    ScalarSignature += Scalar;
  }
  Target.reference = {Name, "convert", Args, "Lfixture/Box;"};
  Target.access = {"public", "final"};
  Target.generic_signature = "<" + R + ":Ljava/lang/Object;>(Lfixture/Mapper<" +
                             (Variance & 1 ? "" : "-") + "T" + T + ";" +
                             (Variance & 2 ? "" : "+") + "T" + R + ";>;" +
                             ScalarSignature + ")Lfixture/Box<T" + R + ";>;";
  const unsigned Width = Scalar == "I" ? 1 : 2;
  const unsigned AuxWords = Aux * Width;
  Target.registers = 3 + AuxWords;
  Target.instructions = {op(0, "const/4", {0}, int64_t(0)),
                         op(1, "return-object", {0})};
  Target.code_end = 2;
  Method Raw;
  Args.insert(Args.begin(), Name);
  Args.insert(Args.end(), {"I", "Ljava/lang/Object;"});
  Raw.reference = {Name, "convert$default", Args, "Lfixture/Box;"};
  Raw.access = {"public", "static", "synthetic"};
  Raw.registers = 5 + AuxWords;
  std::vector<unsigned> Words{1, 2};
  for (unsigned I = 0; I < AuxWords; ++I)
    Words.push_back(3 + I);
  if (Aux)
    Raw.instructions = {
        op(0, "and-int/lit8", {0, 3 + AuxWords}, int64_t(2)),
        op(1, "if-eqz", {0}, {}, {}, 3),
        op(2, Width == 1 ? "const/4" : "const-wide/16", {3}, int64_t(0))};
  const unsigned CallPC = Aux ? 3 : 0;
  Raw.instructions.push_back(
      op(CallPC, Range ? "invoke-virtual/range" : "invoke-virtual", Words, {},
         Target.reference));
  Raw.instructions.push_back(op(CallPC + 1, "move-result-object", {0}));
  Raw.instructions.push_back(op(CallPC + 2, "return-object", {0}));
  Raw.code_end = CallPC + 3;
  auto C = klass(Name, {Constructor, Target, Raw});
  C.access = Public ? Access{"public", "final"} : Access{"final"};
  C.generic_signature = "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;";
  if (Frame)
    for (auto &M : C.methods) {
      M.registers += 3;
      for (auto &I : M.instructions)
        for (auto &Reg : I.registers)
          Reg += 3;
    }
  Method ObjectConstructor;
  ObjectConstructor.reference = {"Ljava/lang/Object;", "<init>", {}, "V"};
  ObjectConstructor.access = {"public", "constructor"};
  auto Object = klass("Ljava/lang/Object;", {ObjectConstructor});
  Object.superclass.reset();
  Object.external = true;
  auto Mapper = klass("Lfixture/Mapper;");
  Mapper.access = {"public", "interface", "abstract"};
  Mapper.generic_signature =
      "<P:Ljava/lang/Object;Q:Ljava/lang/Object;>Ljava/lang/Object;";
  Mapper.external = true;
  auto Box = klass("Lfixture/Box;");
  Box.access.insert("final");
  Box.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  Box.external = true;
  std::vector<Class> Input{C, Object, Mapper, Box};
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

TEST(MobileDalvikScopedCallback, ClassInputAndMethodResultKeepDistinctOwners) {
  for (bool Public : {false, true})
    for (bool Nested : {false, true})
      for (const std::string Scalar : {"I", "J", "D"})
        for (unsigned Aux : {0u, 1u, 7u})
          for (bool Range : {false, true})
            for (bool Frame : {false, true})
              for (bool Alpha : {false, true})
                for (unsigned Variance = 0; Variance < 4; ++Variance)
                  for (unsigned Route = 0; Route < 3; ++Route) {
                    if ((!Aux && Scalar != "I") ||
                        (!Range && 2 + Aux * (Scalar == "I" ? 1 : 2) > 5))
                      continue;
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{Public, Nested, Scalar, Aux, Range, Frame,
                                   Alpha, Variance, Route}));
                    auto Input = fixture(Public, Nested, Scalar, Range, Frame,
                                         Alpha, Variance, Aux);
                    const auto Target = Input[0].methods[1].reference;
                    const auto Raw = Input[0].methods[2].reference;
                    Budget B;
                    auto Classes = classes(Input, Route, B);
                    const auto Plans = validateGenericSignatures(Classes, B);
                    EXPECT_TRUE(
                        Plans.erased_generic_invocations.contains(Target));
                    EXPECT_FALSE(Plans.methods.contains(Raw));
                    const auto Report = recoverJava(Classes, B);
                    EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                    EXPECT_FALSE(
                        Report.getArray("generic_signature_projections"));
                  }
}

TEST(MobileDalvikScopedCallback,
     IncompleteClassMethodRelationsRequireIndependentRawCallProof) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 37; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = fixture(false, false, "I", Mutation == 36, false, false, 0,
                           Mutation == 36 ? 8 : 1);
      auto &C = Input[0];
      auto &Target = C.methods[1];
      auto &Raw = C.methods[2];
      auto Signature = *Target.generic_signature;
      auto replace = [&](std::string From, std::string To) {
        const auto At = Signature.find(From);
        ASSERT_NE(At, Signature.npos);
        Signature.replace(At, From.size(), To);
        Target.generic_signature = Signature;
      };
      if (Mutation == 0)
        C.access.clear();
      else if (Mutation == 1)
        C.generic_signature.reset();
      else if (Mutation == 2)
        C.generic_signature = "<T:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 3)
        C.interfaces = {"Lmissing/Interface;"};
      else if (Mutation == 4)
        C.superclass = "Lmissing/Parent;";
      else if (Mutation == 5)
        replace("<R:", "<S:Ljava/lang/Object;R:");
      else if (Mutation == 6)
        replace("R:Ljava/lang/Object;", "R:Ljava/lang/Number;");
      else if (Mutation == 7)
        Target.generic_signature =
            "<R:Ljava/lang/Object;>(Lfixture/Mapper<-TT;+TR;>;I)";
      else if (Mutation == 8)
        replace("-TT;", "-TR;");
      else if (Mutation == 9)
        replace("-TT;", "+TT;");
      else if (Mutation == 10)
        replace("+TR;", "-TR;");
      else if (Mutation == 11)
        replace("+TR;", "+TT;");
      else if (Mutation == 12)
        replace("Box<TR;>", "Box<TT;>");
      else if (Mutation == 13)
        replace("Box<TR;>", "Box<+TR;>");
      else if (Mutation == 14)
        replace("Box<TR;>", "Box<*>");
      else if (Mutation == 15)
        replace("-TT;", "*");
      else if (Mutation == 16)
        Input[2].generic_signature =
            "<P:Ljava/lang/Object;Q:Ljava/lang/Object;S:Ljava/lang/"
            "Object;>Ljava/lang/Object;";
      else if (Mutation == 17)
        Input[2].generic_signature =
            "<P:Ljava/lang/Number;Q:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 18)
        Input[3].generic_signature = "<E:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 19)
        Target.access.insert("static");
      else if (Mutation == 20)
        Target.access.insert("varargs");
      else if (Mutation == 21)
        Target.access.insert("native");
      else if (Mutation == 22)
        Target.access.insert("synthetic");
      else if (Mutation == 23)
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 24)
        Target.reference.returns = "Ljava/lang/Object;";
      else if (Mutation == 25) {
        Target.reference.parameters[1] = "Ljava/lang/Object;";
        replace(";I)", ";Ljava/lang/Object;)");
      } else if (Mutation == 26)
        replace("Lfixture/Mapper<-TT;+TR;>;I", "I");
      else if (Mutation == 27)
        Raw.instructions[3].registers[0] = 0;
      else if (Mutation == 28)
        Raw.instructions[3].registers[1] = 3;
      else if (Mutation == 29)
        std::get<MethodRef>(Raw.instructions[3].reference).owner =
            "Lmissing/Owner;";
      else if (Mutation == 30) {
        C.enclosing = "Lmissing/Host;";
        C.inner_class_present = true;
        C.inner_name = "Core";
        C.inner_access = {"private", "static", "final"};
      } else if (Mutation == 31)
        Input[3].name = "Lmissing/Box;";
      else if (Mutation == 32)
        Input[2].name = "Lmissing/Mapper;";
      else if (Mutation == 33)
        C.generic_signature =
            "<T:Ljava/lang/Object;U:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 34) {
        replace("R:Ljava/lang/Object;", "T:Ljava/lang/Object;");
        replace("TR;", "TT;");
        replace("TR;", "TT;");
      } else if (Mutation == 35)
        Target.access.erase("final");
      Budget B;
      // These shapes lose the specialized callback relation but still have a
      // legal invocation on the original raw receiver and the same descriptor.
      if (Mutation == 5 || Mutation == 6 || (Mutation >= 8 && Mutation <= 15) ||
          Mutation == 22 || Mutation >= 33) {
        const auto Graph = classes(Input, Route, B);
        const auto Plans = validateGenericSignatures(Graph, B);
        EXPECT_FALSE(
            Plans.erased_generic_invocations.contains(Target.reference));
        EXPECT_TRUE(Plans.raw_receiver_invocations.contains(Target.reference));
        EXPECT_EQ(recoverJava(Graph, B).getInteger("unrecovered_method_count"),
                  0);
      } else {
        EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                     std::runtime_error);
      }
    }
}
