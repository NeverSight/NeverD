#include "MobileDalvik.h"
#include "MobileDalvikSignature.h"
#include "gtest/gtest.h"

#include <algorithm>
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
Class klass(std::string Name, std::vector<Method> Methods = {}) {
  Class C;
  C.name = std::move(Name);
  C.superclass = "Ljava/lang/Object;";
  C.access = {"public"};
  C.source_id = "owned-fixture";
  C.methods = std::move(Methods);
  return C;
}
std::vector<Class> fixture(bool SDKInput, bool Range, bool Frame, bool Nested,
                           bool Alpha) {
  const std::string Anchor = Nested ? "Lfixture/Host$Core;" : "Lfixture/Core;";
  const auto Base = Anchor.substr(0, Anchor.size() - 1);
  const std::string Cell = Base + "$Cell;", Factory = Base + "$Cell$Factory;";
  const std::string T = Alpha ? "Value" : "T";
  auto Convert = method({Factory, "convert", {"Lfixture/Source;"}, Cell},
                        {"public", "final"}, 3,
                        {op(0, "const/4", {0}), op(1, "return-object", {0})});
  Convert.instructions[0].literal = int64_t(0);
  Convert.generic_signature = "<" + T +
                              ":Ljava/lang/Object;>(Lfixture/Source<T" + T +
                              ";>;)" + Base + "$Cell<T" + T + ";>;";
  auto F = klass(Factory, {Convert});
  F.access.insert("final");
  F.enclosing = Cell;
  F.inner_class_present = true;
  F.inner_name = "Factory";
  F.inner_access = {"public", "static", "final"};
  auto C = klass(Cell);
  C.access = {"final"};
  C.enclosing = Anchor;
  C.inner_class_present = true;
  C.inner_name = "Cell";
  C.inner_access = {"private", "static", "final"};
  C.generic_signature = "<" + std::string(Alpha ? "Element" : "T") +
                        ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Source = klass("Lfixture/Source;");
  Source.access = {"public", "abstract", "interface"};
  Source.external = SDKInput;
  Source.generic_signature = "<" + std::string(Alpha ? "Item" : "T") +
                             ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Call = method(
      {Anchor, "call", {Factory, Source.name}, Cell}, {"public", "static"}, 3,
      {op(0, Range ? "invoke-virtual/range" : "invoke-virtual", {1, 2},
          Convert.reference),
       op(1, "move-result-object", {0}), op(2, "return-object", {0})});
  if (Frame) {
    Call.registers += 3;
    for (auto &I : Call.instructions)
      for (auto &R : I.registers)
        R += 3;
    F.methods[0].registers += 3;
    for (auto &I : F.methods[0].instructions)
      for (auto &R : I.registers)
        R += 3;
  }
  auto A = klass(Anchor, {Call});
  auto Object = klass("Ljava/lang/Object;");
  Object.external = true;
  Object.superclass.reset();
  std::vector<Class> Input{F, C, A, Source, Object};
  if (Nested) {
    auto Host = klass("Lfixture/Host;");
    Input[2].enclosing = Host.name;
    Input[2].inner_class_present = true;
    Input[2].inner_name = "Core";
    Input[2].inner_access = {"public", "static"};
    Input.push_back(Host);
  }
  return Input;
}
ClassMap classes(std::vector<Class> Input, unsigned Route, Budget &B) {
  if (Route == 2)
    std::reverse(Input.begin(), Input.end());
  if (Route)
    return linkClasses(Input, B);
  ClassMap Result;
  for (auto &C : Input)
    Result.emplace(C.name, C);
  return Result;
}
} // namespace

TEST(MobileDalvikScopedCarrier, NestedInstanceFactoriesKeepPrivateResultScope) {
  for (bool SDKInput : {false, true})
    for (bool Range : {false, true})
      for (bool Frame : {false, true})
        for (bool Nested : {false, true})
          for (bool Alpha : {false, true})
            for (unsigned Route = 0; Route < 3; ++Route) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDKInput, Range, Frame, Nested, Alpha, Route}));
              auto Input = fixture(SDKInput, Range, Frame, Nested, Alpha);
              const auto Ref = Input[0].methods[0].reference;
              const auto Original = Input[0].methods[0].generic_signature;
              Budget B;
              auto Classes = classes(Input, Route, B);
              const auto Plans = validateGenericSignatures(Classes, B);
              EXPECT_TRUE(Plans.erased_generic_invocations.contains(Ref));
              for (const auto &T : Plans.methods.at(Ref).types)
                if (T.kind == GenericType::Kind::TypeVariable)
                  EXPECT_EQ(T.variable_owner, Ref.identity());
              const auto Report = recoverJava(Classes, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
              EXPECT_EQ(Classes.at(Ref.owner).methods[0].generic_signature,
                        Original);
            }
}

TEST(MobileDalvikScopedCarrier, RejectsUnprovedNestedOwnerOrFormalArguments) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 30; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = fixture(false, true, false, false, false);
      auto &Factory = Input[0];
      auto &Cell = Input[1];
      auto &Make = Factory.methods[0];
      auto replace = [&](const std::string &From, const std::string &To) {
        auto &Signature = *Make.generic_signature;
        const auto At = Signature.find(From);
        ASSERT_NE(At, std::string::npos);
        Signature.replace(At, From.size(), To);
      };
      if (Mutation == 0)
        Factory.enclosing = Input[2].name;
      else if (Mutation == 1)
        Factory.inner_access.erase("static");
      else if (Mutation == 2)
        Factory.inner_class_present = false;
      else if (Mutation == 3)
        Factory.inner_name.reset();
      else if (Mutation == 4)
        Factory.access.erase("final");
      else if (Mutation == 5)
        Make.access.erase("final");
      else if (Mutation == 6)
        Make.access.insert("static");
      else if (Mutation == 7)
        Make.access.insert("synthetic");
      else if (Mutation == 8)
        Make.access.insert("bridge");
      else if (Mutation == 9)
        Make.access.insert("native");
      else if (Mutation == 10)
        Cell.external = true;
      else if (Mutation == 11) {
        Cell.inner_access.erase("private");
        Cell.inner_access.insert("protected");
      } else if (Mutation == 12)
        Cell.inner_access.erase("static");
      else if (Mutation == 13)
        Cell.enclosing = "Lmissing/Owner;";
      else if (Mutation == 14)
        Cell.generic_signature.reset();
      else if (Mutation == 15)
        Cell.generic_signature = "<T:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 16)
        Input[3].generic_signature.reset();
      else if (Mutation == 17)
        Input[3].generic_signature = "<T:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 18)
        Input[2].external = true;
      else if (Mutation == 19)
        Input[2].generic_signature = "<X:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 20)
        replace("Source<TT;>", "Source<+TT;>");
      else if (Mutation == 21)
        replace("$Cell<TT;>", "$Cell<+TT;>");
      else if (Mutation == 22)
        replace("$Cell<TT;>", "$Cell<Ljava/lang/Object;>");
      else if (Mutation == 23)
        replace("Source<TT;>", "Source<TU;>");
      else if (Mutation == 24)
        replace("<T:Ljava/lang/Object;>", "<T:Ljava/lang/Number;>");
      else if (Mutation == 25)
        Make.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 26)
        Input[2].methods[0].instructions[0].registers.pop_back();
      else if (Mutation == 27)
        Factory.external = true;
      else if (Mutation == 28)
        Factory.generic_signature =
            "<X:Ljava/lang/Object;X:Ljava/lang/Object;>Ljava/lang/Object;";
      else
        Make.access = {"private", "final"};
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}

TEST(MobileDalvikScopedCarrier, GenericOwnersRetainExistingRawReceiverProof) {
  // This valid unused class formal already has an independent raw-receiver
  // receipt. It must not be misclassified as an unsupported lexical transfer.
  for (unsigned Route = 0; Route < 3; ++Route) {
    auto Input = fixture(false, true, false, false, false);
    Input[0].generic_signature = "<X:Ljava/lang/Object;>Ljava/lang/Object;";
    const auto Ref = Input[0].methods[0].reference;
    const auto Original = Input[0].methods[0].generic_signature;
    Budget B;
    auto Classes = classes(Input, Route, B);
    EXPECT_EQ(recoverJava(Classes, B).getInteger("unrecovered_method_count"),
              0);
    EXPECT_EQ(Classes.at(Ref.owner).methods[0].generic_signature, Original);
  }
}

TEST(MobileDalvikScopedCarrier, PrivateFactoriesRejectForeignAndMixedCallers) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (bool Mixed : {false, true})
      for (bool OtherPackage : {false, true}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{Route, Mixed, OtherPackage}));
        auto Input = fixture(false, true, false, false, false);
        const std::string Name =
            OtherPackage ? "Lother/Client;" : "Lfixture/Client;";
        auto Call = Input[2].methods[0];
        Call.reference.owner = Name;
        if (!Mixed)
          Input[2].methods.clear();
        Input.push_back(klass(Name, {Call}));
        Budget B;
        EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                     std::runtime_error);
      }
}

namespace {
std::vector<Class> nestedTransferFixture(bool Public, bool Range, bool Frame,
                                         bool Alpha, bool Nested,
                                         unsigned Covariance,
                                         std::string Scalar, unsigned Aux) {
  const std::string T = Alpha ? "Element" : "T";
  const std::string BoxName = Nested ? "Lfixture/Host$Box;" : "Lfixture/Box;";
  const auto BoxPrefix = BoxName.substr(0, BoxName.size() - 1);
  const std::string InnerInput =
      BoxPrefix + "<" + (Covariance & 2 ? "+" : "") + "T" + T + ";>;";
  const std::string InputType = "Lfixture/Carrier<" +
                                std::string(Covariance & 1 ? "+" : "") +
                                InnerInput + ">;";
  const std::string OutputType =
      "Lfixture/Carrier<" + BoxPrefix + "<T" + T + ";>;>;";
  std::vector<std::string> Args{"Lfixture/Carrier;"};
  std::string ScalarSignature;
  for (unsigned I = 0; I < Aux; ++I) {
    Args.push_back(Scalar);
    ScalarSignature += Scalar;
  }
  const unsigned Words = 1 + Aux * (Scalar == "I" ? 1 : 2);
  auto Target =
      method({"Lfixture/Core;", "transfer", Args, "Lfixture/Carrier;"},
             Public ? Access{"public", "static"} : Access{"private", "static"},
             1 + Words, {op(0, "const/4", {0}), op(1, "return-object", {0})});
  Target.instructions[0].literal = int64_t(0);
  if (Frame)
    Target.access.insert("final");
  Target.generic_signature = "<" + T + ":Ljava/lang/Object;>(" + InputType +
                             ScalarSignature + ")" + OutputType;
  std::vector<unsigned> Registers;
  for (unsigned I = 1; I <= Words; ++I)
    Registers.push_back(I);
  auto Bridge =
      method({"Lfixture/Core;", "bridge", Args, "Lfixture/Carrier;"},
             {"public", "static", "final", "synthetic"}, 1 + Words,
             {op(0, Range ? "invoke-static/range" : "invoke-static", Registers,
                 Target.reference),
              op(1, "move-result-object", {0}), op(2, "return-object", {0})});
  auto Owner = klass("Lfixture/Core;", {Target, Bridge});
  Owner.access.insert("final");
  if (Frame)
    for (auto &M : Owner.methods) {
      M.registers += 3;
      for (auto &I : M.instructions)
        for (auto &R : I.registers)
          R += 3;
    }
  auto Carrier = klass("Lfixture/Carrier;");
  Carrier.access = {"public", "interface", "abstract"};
  Carrier.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  Carrier.external = true;
  auto Box = klass(BoxName);
  Box.access.insert("final");
  Box.generic_signature = "<Value:Ljava/lang/Object;>Ljava/lang/Object;";
  Box.external = !Nested;
  auto Object = klass("Ljava/lang/Object;");
  Object.superclass.reset();
  Object.external = true;
  std::vector<Class> Input{Owner, Carrier, Box, Object};
  if (Nested) {
    auto Host = klass("Lfixture/Host;");
    Input[2].enclosing = Host.name;
    Input[2].inner_class_present = true;
    Input[2].inner_name = "Box";
    Input[2].inner_access = {"public", "static", "final"};
    Input.push_back(Host);
  }
  return Input;
}
} // namespace

TEST(MobileDalvikScopedCarrier, NestedTransfersKeepBothCompleteCarrierLayers) {
  for (unsigned Variant = 0; Variant < 16; ++Variant)
    for (bool Public : {false, true})
      for (unsigned Covariance = 0; Covariance < 4; ++Covariance)
        for (unsigned AuxCase = 0; AuxCase < 4; ++AuxCase)
          for (unsigned Route = 0; Route < 3; ++Route) {
            const bool Range = Variant & 1;
            const std::string Scalar = AuxCase < 2    ? "I"
                                       : AuxCase == 2 ? "J"
                                                      : "D";
            const unsigned Aux = AuxCase == 0 ? 0 : AuxCase == 3 ? 7 : 2;
            if (!Range && 1 + Aux * (Scalar == "I" ? 1 : 2) > 5)
              continue;
            SCOPED_TRACE(::testing::PrintToString(
                std::tuple{Variant, Public, Covariance, AuxCase, Route}));
            auto Input =
                nestedTransferFixture(Public, Range, Variant & 2, Variant & 4,
                                      Variant & 8, Covariance, Scalar, Aux);
            const auto Target = Input[0].methods[0].reference;
            const auto Bridge = Input[0].methods[1].reference;
            if (Route == 2)
              std::reverse(Input[0].methods.begin(), Input[0].methods.end());
            Budget B;
            auto Classes = classes(Input, Route, B);
            const auto Plans = validateGenericSignatures(Classes, B);
            EXPECT_TRUE(Plans.erased_generic_invocations.contains(Target));
            EXPECT_FALSE(Plans.methods.contains(Bridge));
            const auto Report = recoverJava(Classes, B);
            EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
            EXPECT_FALSE(Report.getArray("generic_signature_projections"));
          }
}

TEST(MobileDalvikScopedCarrier, NestedTransfersRejectMissingOrForeignBindings) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 40; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input =
          nestedTransferFixture(false, true, false, false, false, 3, "I", 2);
      auto &Owner = Input[0];
      auto &Target = Owner.methods[0];
      auto &Bridge = Owner.methods[1];
      auto replace = [&](std::string From, std::string To) {
        auto &S = *Target.generic_signature;
        const auto At = S.find(From);
        ASSERT_NE(At, S.npos);
        S.replace(At, From.size(), To);
      };
      if (Mutation == 0)
        Owner.access.erase("final");
      else if (Mutation == 1)
        Owner.generic_signature = "<X:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 2)
        Owner.superclass = "Lmissing/Parent;";
      else if (Mutation == 3)
        Owner.interfaces = {"Lmissing/Interface;"};
      else if (Mutation == 4)
        Target.access.erase("static");
      else if (Mutation == 5)
        Target.access.insert("varargs");
      else if (Mutation == 6)
        Target.access.insert("native");
      else if (Mutation == 7)
        Target.access.insert("synthetic");
      else if (Mutation == 8)
        replace("<T:", "<U:Ljava/lang/Object;T:");
      else if (Mutation == 9)
        replace("T:Ljava/lang/Object;", "T:Ljava/lang/Number;");
      else if (Mutation == 10)
        replace("Carrier<+Lfixture/Box", "Carrier<-Lfixture/Box");
      else if (Mutation == 11)
        replace("Box<+TT;>", "Box<-TT;>");
      else if (Mutation == 12)
        replace(")Lfixture/Carrier<Lfixture/Box<TT;>;>",
                ")Lfixture/Carrier<+Lfixture/Box<TT;>;>");
      else if (Mutation == 13)
        replace("Box<TT;>", "Box<+TT;>");
      else if (Mutation == 14)
        replace("Carrier<+Lfixture/Box<+TT;>;>", "Carrier<*>");
      else if (Mutation == 15)
        replace("Box<+TT;>", "Box<*>");
      else if (Mutation == 16)
        replace("Box<+TT;>", "Box<+TU;>");
      else if (Mutation == 17)
        replace("Box<TT;>", "Box<Ljava/lang/Object;>");
      else if (Mutation == 18)
        Input[1].generic_signature.reset();
      else if (Mutation == 19)
        Input[2].generic_signature.reset();
      else if (Mutation == 20)
        Input[1].generic_signature = "<E:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 21)
        Input[2].generic_signature = "<E:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 22)
        Input[1].generic_signature =
            "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 23)
        Input[2].generic_signature =
            "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 24)
        Input[1].name = "Lmissing/Carrier;";
      else if (Mutation == 25)
        Input[2].name = "Lmissing/Box;";
      else if (Mutation == 26)
        Target.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 27)
        Bridge.instructions[0].registers.pop_back();
      else if (Mutation == 28)
        Bridge.instructions[0].registers[0] = 2;
      else if (Mutation == 29)
        Target.generic_signature = "malformed";
      else if (Mutation == 30) {
        Input =
            nestedTransferFixture(false, true, false, false, false, 3, "I", 8);
      } else if (Mutation == 31) {
        Target.reference.parameters[1] = "Ljava/lang/Object;";
        replace(";II)", ";Ljava/lang/Object;I)");
        Bridge.reference.parameters[1] = "Ljava/lang/Object;";
        Bridge.instructions[0].reference = Target.reference;
      } else if (Mutation == 32)
        Input[2].access.erase("public");
      else if (Mutation == 33)
        replace("Box<TT;>", "Box<[TT;>");
      else if (Mutation == 34)
        replace("Box<+TT;>", "Box<+[TT;>");
      else if (Mutation == 35)
        Target.access.insert("bridge");
      else {
        auto Call = Bridge;
        const bool Other = Mutation & 1, Mixed = Mutation >= 38;
        const std::string Name = Other ? "Lother/Client;" : "Lfixture/Client;";
        Call.reference.owner = Name;
        Call.access.erase("synthetic");
        if (!Mixed)
          Owner.methods.pop_back();
        Input.push_back(klass(Name, {Call}));
      }
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> privateContextFactoryFixture(bool Private, bool Range,
                                                bool Frame, bool Alpha,
                                                bool SDKResult,
                                                unsigned Variance) {
  const std::string T = Alpha ? "Original" : "T";
  const std::string R = Alpha ? "Stored" : "R";
  const std::string In = Variance & 1 ? "" : "-";
  const std::string Out = Variance & 2 ? "" : "+";
  auto Make =
      method({"Lfixture/Provider;",
              "make",
              {"Lfixture/Forward;", "Lfixture/Reverse;"},
              "Lfixture/Pair;"},
             Private ? Access{"private", "static"} : Access{"public", "static"},
             3, {op(0, "const/4", {0}), op(1, "return-object", {0})});
  Make.instructions[0].literal = int64_t(0);
  if (Frame)
    Make.access.insert("final");
  Make.generic_signature =
      "<" + T + ":Ljava/lang/Object;" + R +
      ":Ljava/lang/Object;>(Lfixture/Forward<" + In + "Lfixture/Context;" + In +
      "T" + T + ";" + Out + "T" + R + ";>;Lfixture/Reverse<" + In + "T" + R +
      ";" + Out + "T" + T + ";>;)Lfixture/Pair<T" + T + ";T" + R + ";>;";
  auto Call =
      method({"Lfixture/Provider;", "call", Make.reference.parameters,
              Make.reference.returns},
             {"public", "static"}, 3,
             {op(0, Range ? "invoke-static/range" : "invoke-static", {1, 2},
                 Make.reference),
              op(1, "move-result-object", {0}), op(2, "return-object", {0})});
  auto Owner = klass("Lfixture/Provider;", {Make, Call});
  Owner.access.insert("final");
  if (Frame)
    for (auto &M : Owner.methods) {
      M.registers += 3;
      for (auto &I : M.instructions)
        for (auto &Reg : I.registers)
          Reg += 3;
    }
  auto F = klass("Lfixture/Forward;"), B = klass("Lfixture/Reverse;");
  F.access = B.access = {"public", "abstract", "interface"};
  F.generic_signature = "<C:Ljava/lang/Object;A:Ljava/lang/Object;B:Ljava/lang/"
                        "Object;>Ljava/lang/Object;";
  B.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  F.external = B.external = true;
  auto Pair = klass("Lfixture/Pair;");
  Pair.access = {"public", "abstract", "interface"};
  Pair.interfaces = {"Lfixture/Parent;"};
  Pair.generic_signature = "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/"
                           "lang/Object;Lfixture/Parent<TA;TB;>;";
  Pair.external = SDKResult;
  auto Parent = klass("Lfixture/Parent;");
  Parent.access = Pair.access;
  Parent.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  Parent.external = SDKResult;
  auto Context = klass("Lfixture/Context;");
  Context.access = {"public", "abstract", "interface"};
  Context.external = true;
  auto Object = klass("Ljava/lang/Object;");
  Object.superclass.reset();
  Object.external = true;
  return {Owner, F, B, Pair, Parent, Context, Object};
}
} // namespace

TEST(MobileDalvikScopedCarrier,
     PrivateContextFactoriesReuseCompleteOppositeSlots) {
  for (unsigned Variant = 0; Variant < 16; ++Variant)
    for (bool Private : {false, true})
      for (unsigned Variance = 0; Variance < 4; ++Variance)
        for (unsigned Route = 0; Route < 3; ++Route) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{Variant, Private, Variance, Route}));
          auto Input =
              privateContextFactoryFixture(Private, Variant & 1, Variant & 2,
                                           Variant & 4, Variant & 8, Variance);
          const auto Ref = Input[0].methods[0].reference;
          if (Route == 2)
            std::reverse(Input[0].methods.begin(), Input[0].methods.end());
          Budget B;
          auto Classes = classes(Input, Route, B);
          const auto Plans = validateGenericSignatures(Classes, B);
          EXPECT_TRUE(Plans.erased_generic_invocations.contains(Ref));
          const auto Report = recoverJava(Classes, B);
          EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
          EXPECT_FALSE(Report.getArray("generic_signature_projections"));
        }
  for (unsigned Route = 0; Route < 3; ++Route) {
    auto Input =
        privateContextFactoryFixture(true, false, false, false, true, 0);
    const auto Ref = Input[0].methods[0].reference;
    auto Init =
        method({Ref.owner, "<clinit>", {}, "V"}, {"static", "constructor"}, 3,
               {op(0, "const/4", {0}), op(1, "const/4", {1}),
                op(2, "invoke-static", {0, 1}, Ref),
                op(3, "move-result-object", {2}), op(4, "return-void")});
    Init.instructions[0].literal = Init.instructions[1].literal = int64_t(0);
    Input[0].methods[1] = Init;
    Budget B;
    auto Classes = classes(Input, Route, B);
    EXPECT_TRUE(validateGenericSignatures(Classes, B)
                    .erased_generic_invocations.contains(Ref));
    EXPECT_EQ(recoverJava(Classes, B).getInteger("unrecovered_method_count"),
              0);
  }
}

TEST(MobileDalvikScopedCarrier,
     PrivateContextFactoriesRejectBorrowedAuthority) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 36; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input =
          privateContextFactoryFixture(true, true, false, false, true, 0);
      auto &Owner = Input[0];
      auto &Make = Owner.methods[0];
      auto &Call = Owner.methods[1];
      auto replace = [&](std::string From, std::string To) {
        auto &S = *Make.generic_signature;
        const auto At = S.find(From);
        ASSERT_NE(At, S.npos);
        S.replace(At, From.size(), To);
      };
      if (Mutation == 0)
        Owner.access = {"final"};
      else if (Mutation == 1)
        Owner.access.erase("final");
      else if (Mutation == 2)
        Owner.generic_signature = "<X:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 3)
        Owner.interfaces = {"Lmissing/Interface;"};
      else if (Mutation == 4)
        Owner.superclass = "Lmissing/Parent;";
      else if (Mutation == 5)
        Make.access.erase("static");
      else if (Mutation == 6)
        Make.access.insert("varargs");
      else if (Mutation == 7)
        Make.access.insert("synthetic");
      else if (Mutation == 8)
        Make.access.insert("native");
      else if (Mutation == 9)
        Make.declared_throws =
            std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 10)
        replace("R:Ljava/lang/Object;", "R:Ljava/lang/Number;");
      else if (Mutation == 11)
        Input[1].generic_signature.reset();
      else if (Mutation == 12)
        replace("-Lfixture/Context;", "+Lfixture/Context;");
      else if (Mutation == 13)
        replace("-TT;", "+TT;");
      else if (Mutation == 14)
        replace("+TR;", "-TR;");
      else if (Mutation == 15)
        replace("-TR;", "+TR;");
      else if (Mutation == 16)
        replace("+TT;", "-TT;");
      else if (Mutation == 17)
        replace("-TT;", "-TR;");
      else if (Mutation == 18)
        replace("+TT;", "+TR;");
      else if (Mutation == 19)
        replace("Pair<TT;TR;>", "Pair<TT;TT;>");
      else if (Mutation == 20)
        replace("Pair<TT;TR;>", "Pair<TR;TT;>");
      else if (Mutation == 21)
        Input[3].generic_signature =
            "<A:Ljava/lang/Number;B:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 22)
        Input[1].generic_signature =
            "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 23)
        Input[5].generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 24)
        Input[5].access.clear();
      else if (Mutation == 25)
        Input[2].name = "Lmissing/Reverse;";
      else if (Mutation == 26)
        Input[3].generic_signature.reset();
      else if (Mutation == 27)
        replace("Pair<TT;TR;>", "Pair<TT;+TR;>");
      else if (Mutation == 28)
        replace("<T:", "<U:Ljava/lang/Object;T:");
      else if (Mutation == 29)
        Call.instructions[0].registers.pop_back();
      else if (Mutation == 30)
        Make.generic_signature = "malformed";
      else {
        const bool External = Mutation == 31;
        const bool OtherPackage = Mutation & 1, Mixed = Mutation >= 34;
        auto Foreign = Call;
        const std::string Name =
            OtherPackage ? "Lother/Client;" : "Lfixture/Client;";
        Foreign.reference.owner = Name;
        if (External)
          Owner.external = true;
        else if (!Mixed)
          Owner.methods.pop_back();
        Input.push_back(klass(Name, {Foreign}));
      }
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}

namespace {
std::vector<Class> dependentInterfaceFixture(bool Range, bool Frame, bool Alpha,
                                             unsigned Order, bool External) {
  const std::string M = Alpha ? "Carrier" : "M";
  const std::string A = Alpha ? "Input" : "A";
  const std::string B = Alpha ? "Output" : "B";
  std::vector<std::string> Formals{
      M + "::Lfixture/Mapper<T" + A + ";T" + B + ";>;",
      A + ":Ljava/lang/Object;", B + ":Ljava/lang/Object;"};
  std::rotate(Formals.begin(), Formals.begin() + Order, Formals.end());
  std::string G = "<";
  for (const auto &F : Formals)
    G += F;
  G += ">(T" + A + ";T" + M + ";Lfixture/Context;)Ljava/lang/Object;";
  auto Use =
      method({"Lfixture/Core;",
              "use",
              {"Ljava/lang/Object;", "Lfixture/Mapper;", "Lfixture/Context;"},
              "Ljava/lang/Object;"},
             {"public", "static"}, 4,
             {op(0, "const/4", {0}), op(1, "return-object", {0})});
  Use.instructions[0].literal = int64_t(0);
  Use.generic_signature = G;
  auto Call =
      method({External ? "Lfixture/Client;" : "Lfixture/Core;", "call",
              Use.reference.parameters, Use.reference.returns},
             {"public", "static"}, 4,
             {op(0, Range ? "invoke-static/range" : "invoke-static", {1, 2, 3},
                 Use.reference),
              op(1, "move-result-object", {0}), op(2, "return-object", {0})});
  if (Frame)
    for (auto *F : {&Use, &Call}) {
      F->registers += 3;
      for (auto &I : F->instructions)
        for (auto &R : I.registers)
          R += 3;
    }
  if (External) {
    Use.instructions.clear();
    Use.registers = 0;
    Use.code_end = 0;
  }
  auto Owner =
      klass("Lfixture/Core;", External ? std::vector<Method>{Use}
                                       : std::vector<Method>{Use, Call});
  Owner.access.insert("final");
  Owner.external = External;
  auto Mapper = klass("Lfixture/Mapper;");
  Mapper.access = {"public", "abstract", "interface"};
  Mapper.generic_signature =
      "<A:Ljava/lang/Object;B:Ljava/lang/Object;>Ljava/lang/Object;";
  Mapper.external = true;
  auto Context = klass("Lfixture/Context;");
  Context.access = Mapper.access;
  Context.external = true;
  auto Object = klass("Ljava/lang/Object;");
  Object.superclass.reset();
  Object.external = true;
  auto Extra = klass("Lfixture/Extra;");
  Extra.access = Mapper.access;
  Extra.external = true;
  std::vector<Class> Input{Owner, Mapper, Context, Object, Extra};
  if (External)
    Input.push_back(klass(Call.reference.owner, {Call}));
  return Input;
}
} // namespace

TEST(MobileDalvikScopedCarrier, DependentInterfaceBoundsKeepAllValueFormals) {
  for (bool Range : {false, true})
    for (bool Frame : {false, true})
      for (bool Alpha : {false, true})
        for (unsigned Order = 0; Order < 3; ++Order)
          for (bool External : {false, true})
            for (unsigned Route = 0; Route < 3; ++Route) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{Range, Frame, Alpha, Order, External, Route}));
              auto Input = dependentInterfaceFixture(Range, Frame, Alpha, Order,
                                                     External);
              const auto Ref = Input[0].methods[0].reference;
              if (Route == 2)
                std::reverse(Input[0].methods.begin(), Input[0].methods.end());
              Budget B;
              auto Classes = classes(Input, Route, B);
              const auto Plans = validateGenericSignatures(Classes, B);
              EXPECT_TRUE(Plans.erased_generic_invocations.contains(Ref));
              const auto Report = recoverJava(Classes, B);
              EXPECT_EQ(Report.getInteger("unrecovered_method_count"), 0);
              EXPECT_FALSE(Report.getArray("generic_signature_projections"));
            }
}

TEST(MobileDalvikScopedCarrier,
     DependentInterfaceBoundsRejectLostRelationships) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 32; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input = dependentInterfaceFixture(false, false, false, 0, false);
      auto &Use = Input[0].methods[0];
      auto &Call = Input[0].methods[1];
      auto replace = [&](std::string From, std::string To) {
        auto &S = *Use.generic_signature;
        const auto At = S.find(From);
        ASSERT_NE(At, S.npos);
        S.replace(At, From.size(), To);
      };
      if (Mutation == 0)
        replace("M::Lfixture/Mapper<TA;TB;>;",
                "M::Lfixture/Mapper<TA;TB;>;:Lfixture/Extra;");
      else if (Mutation == 1)
        replace("M::", "M:Ljava/lang/Object;:");
      else if (Mutation == 2)
        replace("Mapper<TA;TB;>", "Mapper<+TA;TB;>");
      else if (Mutation == 3)
        replace("Mapper<TA;TB;>", "Mapper<TA;-TB;>");
      else if (Mutation == 4)
        replace("Mapper<TA;TB;>", "Mapper<*TB;>");
      else if (Mutation == 5)
        replace("Mapper<TA;TB;>", "Mapper<TA;*>");
      else if (Mutation == 6)
        replace("Mapper<TA;TB;>", "Mapper<TA;TA;>");
      else if (Mutation == 7)
        replace("Mapper<TA;TB;>", "Mapper<TB;TA;>");
      else if (Mutation == 8)
        replace("Mapper<TA;TB;>", "Mapper<TU;TB;>");
      else if (Mutation == 9)
        replace("Mapper<TA;TB;>", "Mapper<TM;TB;>");
      else if (Mutation == 10)
        replace("(TA;TM;", "(TB;TM;");
      else if (Mutation == 11)
        replace("(TA;TM;", "(TA;TA;");
      else if (Mutation == 12) {
        replace("Lfixture/Context;)", "TB;)");
        Use.reference.parameters[2] = "Ljava/lang/Object;";
        Call.reference.parameters[2] = "Ljava/lang/Object;";
        Call.instructions[0].reference = Use.reference;
      } else if (Mutation == 13)
        replace(")Ljava/lang/Object;", ")TB;");
      else if (Mutation == 14)
        replace("A:Ljava/lang/Object;", "A:Ljava/lang/Number;");
      else if (Mutation == 15)
        replace("B:Ljava/lang/Object;", "B:Ljava/lang/Number;");
      else if (Mutation == 16)
        replace("B:Ljava/lang/Object;", "");
      else if (Mutation == 17)
        replace("<M:", "<X:Ljava/lang/Object;M:");
      else if (Mutation == 18)
        Input[1].access = {"public", "final"};
      else if (Mutation == 19)
        Input[1].generic_signature.reset();
      else if (Mutation == 20)
        Input[1].generic_signature =
            "<A:Ljava/lang/Object;B:Ljava/lang/Object;C:Ljava/lang/"
            "Object;>Ljava/lang/Object;";
      else if (Mutation == 21)
        Input[1].generic_signature =
            "<A:Ljava/lang/Object;B:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 22)
        Input[1].name = "Lmissing/Mapper;";
      else if (Mutation == 23)
        Input[2].generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 24)
        Input[2].access.clear();
      else if (Mutation == 25)
        Use.access.erase("static");
      else if (Mutation == 26)
        Use.access.insert("varargs");
      else if (Mutation == 27)
        Use.access.insert("synthetic");
      else if (Mutation == 28)
        Use.declared_throws = std::vector<std::string>{"Ljava/lang/Exception;"};
      else if (Mutation == 29)
        Call.instructions[0].registers.pop_back();
      else if (Mutation == 30)
        Use.generic_signature = "malformed";
      else
        Input[0].generic_signature =
            "<T:Ljava/lang/Object;T:Ljava/lang/Object;>Ljava/lang/Object;";
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}

TEST(MobileDalvikScopedCarrier, PackageRawReceiverKeepsMethodScopedArray) {
  const std::string array = "[Ljava/lang/Object;";
  auto provider = klass("Lfixture/Copies;");
  provider.access = {"final"};
  provider.generic_signature = "<T:Ljava/lang/Object;>Ljava/lang/Object;";
  const MethodRef ref{provider.name, "transfer", {array}, array};
  auto transfer = method(ref, {"public"}, 2, {op(0, "return-object", {1})});
  transfer.generic_signature = "<T:Ljava/lang/Object;>([TT;)[TT;";
  provider.methods.push_back(transfer);
  auto root = klass("Ljava/lang/Object;");
  root.superclass.reset();
  root.external = true;
  for (bool sdk : {false, true}) {
    if (sdk) {
      provider.external = true;
      provider.methods.front().instructions.clear();
      provider.methods.front().registers = provider.methods.front().code_end = 0;
    }
    for (bool samePackage : {false, true}) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{sdk, samePackage}));
      auto caller = klass(samePackage ? "Lfixture/Caller;" : "Lelsewhere/Caller;");
      caller.methods.push_back(method(
          {caller.name, "copy", {provider.name, array}, array},
          {"public", "static"}, 3,
          {op(0, "invoke-virtual", {1, 2}, ref), op(1, "move-result-object", {0}),
           op(2, "return-object", {0})}));
      Budget b;
      auto input = linkClasses({provider, caller, root}, b);
      if (samePackage) {
        EXPECT_TRUE(validateGenericSignatures(input, b)
                        .raw_receiver_invocations.contains(ref));
        EXPECT_EQ(recoverJava(input, b).getInteger("unrecovered_method_count"), 0);
      } else {
        EXPECT_THROW(recoverJava(input, b), std::runtime_error);
      }
    }
  }
}
