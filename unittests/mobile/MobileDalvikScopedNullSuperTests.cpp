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
std::vector<Class> fixture(bool SDK, bool Range, bool Copy, bool Wide,
                           bool Alpha, unsigned Variance, bool Sibling) {
  const std::string Object = "Ljava/lang/Object;", T = Alpha ? "Element" : "T",
                    Scalar = Wide ? "J" : "Z";
  const std::string Argument = Variance == 3
                                   ? "*"
                                   : std::string(Variance == 1   ? "-"
                                                 : Variance == 2 ? "+"
                                                                 : "") +
                                         "TP;";
  auto Parent = klass("Lfixture/Parent;", SDK);
  Parent.generic_signature = "<P:Ljava/lang/Object;>Ljava/lang/Object;";
  MethodRef Selected{
      Parent.name, "<init>", {Scalar, "Lfixture/Callback;"}, "V"};
  auto Sink = method(
      Selected, {"public", "constructor"},
      SDK    ? 0
      : Wide ? 4
             : 3,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{op(0, "invoke-direct", {0},
                                        MethodRef{Object, "<init>", {}, "V"}),
                                     op(1, "return-void")});
  Sink.generic_signature =
      "(" + Scalar + "Lfixture/Callback<" + Argument + ">;)V";
  Parent.methods.push_back(Sink);
  if (Sibling) {
    auto Other = Sink;
    Other.reference.parameters[1] = "Ljava/lang/String;";
    Other.generic_signature.reset();
    Parent.methods.push_back(Other);
  }
  auto Child = klass("Lfixture/Child;");
  Child.superclass = Parent.name;
  Child.generic_signature =
      "<" + T + ":Ljava/lang/Object;>Lfixture/Parent<T" + T + ";>;";
  const unsigned Null = Wide ? 3 : 2;
  auto Zero = op(0, "const/4", {Copy ? 0 : Null});
  Zero.literal = int64_t(0);
  std::vector<Instruction> Code{Zero};
  if (Copy)
    Code.push_back(op(Code.size(), "move-object", {Null, 0}));
  Code.push_back(op(Code.size(), "move-object", {0, 4}));
  Code.push_back(op(Code.size(), Wide ? "move-wide" : "move", {1, 5}));
  Code.push_back(op(
      Code.size(), Range ? "invoke-direct/range" : "invoke-direct",
      Wide ? std::vector<unsigned>{0, 1, 2, 3} : std::vector<unsigned>{0, 1, 2},
      Selected));
  Code.push_back(op(Code.size(), "return-void"));
  auto Caller =
      method({Child.name, "<init>", {Scalar, "Z", "Lfixture/Function;"}, "V"},
             {"public", "constructor"}, Wide ? 9 : 8, Code);
  Caller.generic_signature = "(" + Scalar + "ZLfixture/Function<T" + T +
                             ";Ljava/lang/Comparable<*>;>;)V";
  Child.methods.push_back(Caller);
  auto Callback = klass("Lfixture/Callback;", true);
  Callback.access = {"public", "interface", "abstract"};
  Callback.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Function = klass("Lfixture/Function;", true);
  Function.access = {"public", "interface", "abstract"};
  Function.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Comparable = klass("Ljava/lang/Comparable;", true);
  Comparable.access = {"public", "interface", "abstract"};
  Comparable.generic_signature = "<C:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  return {Child,    Parent,     Callback,
          Function, Comparable, klass("Ljava/lang/String;", true),
          Root};
}
} // namespace

TEST(MobileDalvikScopedNullSuper,
     RetainCompleteScopedNullViewsScalarsAndFixedOverloads) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Copy : {false, true})
        for (bool Wide : {false, true})
          for (bool Alpha : {false, true})
            for (unsigned Variance : {0u, 1u, 2u, 3u})
              for (bool Sibling : {false, true})
                for (unsigned Route : {0u, 1u, 2u}) {
                  SCOPED_TRACE(::testing::PrintToString(
                      std::tuple{SDK, Range, Copy, Wide, Alpha, Variance,
                                 Sibling, Route}));
                  auto Input =
                      fixture(SDK, Range, Copy, Wide, Alpha, Variance, Sibling);
                  const auto Original = Input;
                  const auto Caller = Input[0].methods[0].reference,
                             Target = Input[1].methods[0].reference;
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
                  EXPECT_TRUE(
                      Plans.superclass_calls.contains({Caller, Target}));
                  for (const auto &C : Original) {
                    const auto &After = Classes.at(C.name);
                    EXPECT_EQ(After.generic_signature, C.generic_signature);
                    EXPECT_EQ(After.access, C.access);
                    for (size_t I = 0; I < C.methods.size(); ++I) {
                      EXPECT_EQ(After.methods[I].reference,
                                C.methods[I].reference);
                      EXPECT_EQ(After.methods[I].generic_signature,
                                C.methods[I].generic_signature);
                      EXPECT_EQ(After.methods[I].access, C.methods[I].access);
                    }
                  }
                  const auto Report = recoverJava(Classes, B);
                  EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(
                      Report.getArray("generic_signature_projections"));
                }
}

TEST(MobileDalvikScopedNullSuper,
     RejectNonNullWordsBorrowedScopesAndUnprovedTargets) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, true, false, false, 1, false);
      auto &Caller = Input[0].methods[0], &Target = Input[1].methods[0];
      if (Mutation == 0)
        Caller.instructions[0].literal = int64_t(1);
      else if (Mutation == 1)
        Caller.instructions[0].opcode = "const-wide/16";
      else if (Mutation == 2)
        Caller.instructions[4].registers.back() = 6;
      else if (Mutation == 3)
        Caller.instructions[4].registers.pop_back();
      else if (Mutation == 4)
        Caller.instructions[4].registers[0] = 2;
      else if (Mutation == 5)
        Target.generic_signature = "(ZLfixture/Callback<-TMissing;>;)V";
      else if (Mutation == 6)
        Target.generic_signature =
            "<U:Ljava/lang/Object;>(ZLfixture/Callback<-TU;>;)V";
      else if (Mutation == 7)
        Caller.generic_signature = "<U:Ljava/lang/Object;>(ZZLfixture/"
                                   "Function<TT;Ljava/lang/Comparable<*>;>;)V";
      else if (Mutation == 8)
        Input[0].generic_signature =
            "<T:Ljava/lang/String;>Lfixture/Parent<TT;>;";
      else if (Mutation == 9)
        Input[0].generic_signature = "Lfixture/Parent;";
      else if (Mutation == 10)
        Input[2].generic_signature = "<C:Ljava/lang/String;>Ljava/lang/Object;";
      else if (Mutation == 11)
        Target.generic_signature = "(ZLfixture/Callback<-TP;TP;>;)V";
      else if (Mutation == 12)
        Target.access.insert("private");
      else if (Mutation == 13)
        Target.access.insert("static");
      else if (Mutation == 14)
        Target.access.insert("native");
      else if (Mutation == 15)
        Target.access.insert("varargs");
      else if (Mutation == 16)
        Target.declared_throws = {"Ljava/lang/Exception;"};
      else if (Mutation == 17) {
        auto Sibling = Target;
        Sibling.reference.parameters[1] = "Ljava/lang/String;";
        Sibling.generic_signature =
            "<U:Ljava/lang/Object;>(ZLjava/lang/String;)V";
        Input[1].methods.push_back(Sibling);
      } else if (Mutation == 18)
        Caller.instructions[0].registers[0] = Caller.registers;
      else
        Caller.instructions[1].registers[0] = 3;
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
