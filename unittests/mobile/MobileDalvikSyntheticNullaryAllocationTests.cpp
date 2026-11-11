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
std::vector<Class> fixture(bool SDK, bool CarrierSDK, bool Range, bool Alpha,
                           bool ClassSynthetic, bool Wide) {
  const std::string Object = "Ljava/lang/Object;", Scalar = Wide ? "D" : "F",
                    E = Alpha ? "Element" : "E", BaseName = "Lfixture/Base;",
                    SlotName = "Lfixture/Slot;", MakerName = "Lfixture/Maker;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  MethodRef RootInit{Object, "<init>", {}, "V"};
  Root.methods = {method(RootInit, {"public", "constructor"}, 0, {})};
  auto Base = klass(BaseName, CarrierSDK), Slot = klass(SlotName, CarrierSDK),
       Maker = klass(MakerName, CarrierSDK);
  Base.generic_signature = "<B:Ljava/lang/Object;>Ljava/lang/Object;";
  Slot.generic_signature = "<S:Ljava/lang/Object;>Ljava/lang/Object;";
  Maker.access = {"public", "abstract", "interface"};
  Maker.generic_signature = "<R:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Make =
      method({MakerName, "make", {}, Object}, {"public", "abstract"}, 0, {});
  Make.generic_signature = "()TR;";
  Maker.methods.push_back(Make);
  auto Fresh = klass("Lfixture/Fresh;", SDK);
  Fresh.access.insert("final");
  if (ClassSynthetic)
    Fresh.access.insert("synthetic");
  Fresh.interfaces = {MakerName};
  auto Init = method(
      {Fresh.name, "<init>", {}, "V"}, {"public", "constructor", "synthetic"},
      SDK ? 0 : 1,
      SDK ? std::vector<Instruction>{}
          : std::vector<Instruction>{op(0, "invoke-direct", {0}, RootInit),
                                     op(3, "return-void")});
  auto Zero = op(0, "const/4", {0});
  Zero.literal = int64_t(0);
  Fresh.methods = {Init, method({Fresh.name, "make", {}, Object}, {"public"},
                                SDK ? 0 : 1,
                                SDK ? std::vector<Instruction>{}
                                    : std::vector<Instruction>{
                                          Zero, op(1, "return-object", {0})})};
  auto Core = klass("Lfixture/Core;");
  Core.generic_signature = "<" + E + ":Ljava/lang/Object;>Ljava/lang/Object;";
  MethodRef Selected{Core.name, "<init>", {Scalar, BaseName, MakerName}, "V"};
  auto Target =
      method(Selected, {"public", "constructor"}, Wide ? 5 : 4,
             {op(0, "invoke-direct", {0}, RootInit), op(3, "return-void")});
  const auto Nested = "Lfixture/Slot<T" + E + ";>;";
  Target.generic_signature = "(" + Scalar + "Lfixture/Base<" + Nested +
                             ">;Lfixture/Maker<" + Nested + ">;)V";
  auto Other = Target;
  Other.reference.parameters.back() = Object;
  Other.generic_signature =
      "(" + Scalar + "Lfixture/Base<" + Nested + ">;Ljava/lang/Object;)V";
  const unsigned MakerLocal = Wide ? 4 : 3, BaseLocal = Wide ? 3 : 2;
  std::vector<unsigned> Words{0, 1};
  if (Wide)
    Words.push_back(2);
  Words.push_back(BaseLocal);
  Words.push_back(MakerLocal);
  auto Caller = method(
      {Core.name, "<init>", {Scalar, BaseName}, "V"}, {"public", "constructor"},
      Wide ? 9 : 8,
      {op(0, "new-instance", {MakerLocal}, Fresh.name),
       op(2, Range ? "invoke-direct/range" : "invoke-direct", {MakerLocal},
          Init.reference),
       op(5, "move-object", {0, 5}), op(6, Wide ? "move-wide" : "move", {1, 6}),
       op(7, "move-object", {BaseLocal, Wide ? 8u : 7u}),
       op(8, Range ? "invoke-direct/range" : "invoke-direct", Words, Selected),
       op(11, "return-void")});
  Caller.generic_signature = "(" + Scalar + "Lfixture/Base<" + Nested + ">;)V";
  if (ClassSynthetic) {
    Caller.instructions.insert(Caller.instructions.begin() + 2,
                               op(4, "check-cast", {MakerLocal}, MakerName));
  }
  Core.methods = {Caller, Target, Other};
  return {Core, Fresh, Base, Slot, Maker, Root};
}
std::vector<Class> primitiveFixture(bool SDK, bool CarrierSDK, bool Range,
                                    bool Wide, const std::string &Capture,
                                    bool Covariant) {
  auto Input = fixture(SDK, CarrierSDK, Range, Capture == "D", true, Wide);
  auto &Core = Input[0], &Fresh = Input[1];
  auto &Caller = Core.methods[0], &Target = Core.methods[1],
       &Init = Fresh.methods[0];
  const unsigned CaptureWords = Capture == "J" || Capture == "D" ? 2 : 1;
  Field CaptureField;
  CaptureField.reference = {Fresh.name, "captured", Capture};
  CaptureField.access = {"public", "final"};
  Fresh.fields.push_back(CaptureField);
  Init.reference.parameters = {Capture};
  if (!SDK) {
    Init.registers = 1 + CaptureWords;
    Init.instructions = {op(0, "invoke-direct", {0},
                            MethodRef{"Ljava/lang/Object;", "<init>", {}, "V"}),
                         op(3,
                            CaptureWords == 2 ? "iput-wide"
                            : Capture == "Z"  ? "iput-boolean"
                                              : "iput",
                            {1, 0}, CaptureField.reference),
                         op(5, "return-void")};
    Init.code_end = 6;
  }
  auto Marker = klass("Lfixture/Marker;", CarrierSDK);
  Marker.access = {"public", "abstract", "interface"};
  Fresh.interfaces.push_back(Marker.name);
  Caller.reference.parameters.insert(Caller.reference.parameters.begin(),
                                     Capture);
  Caller.generic_signature->insert(1, Capture);
  Caller.registers = 7 + Caller.incomingWords();
  const unsigned ScalarInput = 8 + CaptureWords,
                 BaseInput = ScalarInput + (Wide ? 2 : 1),
                 BaseLocal = Wide ? 3 : 2, MakerLocal = Wide ? 4 : 3;
  std::vector<unsigned> Allocation{3, 4}, Words{0, 1};
  if (CaptureWords == 2)
    Allocation.push_back(5);
  if (Wide)
    Words.push_back(2);
  Words.push_back(BaseLocal);
  Words.push_back(MakerLocal);
  Caller.instructions = {
      op(0, CaptureWords == 2 ? "move-wide" : "move", {4, 8}),
      op(1, "new-instance", {3}, Fresh.name),
      op(3, Range ? "invoke-direct/range" : "invoke-direct", Allocation,
         Init.reference),
      op(6, "move-object", {6, 3}),
      op(7, "move-object", {0, 7}),
      op(8, Wide ? "move-wide" : "move", {1, ScalarInput}),
      op(9, "move-object", {BaseLocal, BaseInput}),
      op(10, "move-object", {MakerLocal, 6}),
      op(11, Range ? "invoke-direct/range" : "invoke-direct", Words,
         Target.reference),
      op(14, "return-void")};
  Caller.code_end = 15;
  if (Covariant) {
    auto &Signature = *Target.generic_signature;
    const std::string Marker = "Lfixture/Maker<";
    Signature.insert(Signature.find(Marker) + Marker.size(), "+");
  }
  Input.push_back(std::move(Marker));
  return Input;
}
} // namespace

TEST(MobileDalvikSyntheticNullaryAllocation,
     RetainExactInitializerFlagsNestedScopedInputsAndSelectedDelegation) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool ClassSynthetic : {false, true})
            for (bool Wide : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, CarrierSDK, Range, Alpha, ClassSynthetic,
                               Wide, Route}));
                auto Input = fixture(SDK, CarrierSDK, Range, Alpha,
                                     ClassSynthetic, Wide);
                const auto Original = Input;
                const auto Caller = Input[0].methods[0].reference,
                           Target = Input[0].methods[1].reference;
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
                EXPECT_TRUE(Plans.constructor_calls.contains({Caller, Target}));
                const auto R = recoverJava(Classes, B);
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
                for (const auto &C : Original) {
                  const auto &After = Classes.at(C.name);
                  EXPECT_EQ(After.access, C.access);
                  EXPECT_EQ(After.generic_signature, C.generic_signature);
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

TEST(MobileDalvikSyntheticNullaryAllocation,
     RejectChangedInitializerRolesCarrierBindingsScopesAndPhysicalWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 30; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = fixture(SDK, true, true, false, false, true);
      auto &Caller = Input[0].methods[0], &Target = Input[0].methods[1],
           &Init = Input[1].methods[0];
      if (Mutation == 0) {
        Init.access.erase("public");
        Init.access.insert("private");
      } else if (Mutation == 1)
        Init.access.insert("static");
      else if (Mutation == 2)
        Init.access.insert("native");
      else if (Mutation == 3)
        Init.access.insert("abstract");
      else if (Mutation == 4)
        Init.access.insert("varargs");
      else if (Mutation == 5)
        Init.access.insert("bridge");
      else if (Mutation == 6)
        Init.declared_throws =
            std::vector<std::string>{"Ljava/lang/RuntimeException;"};
      else if (Mutation == 7)
        Init.generic_signature = "<Extra:Ljava/lang/Object;>()V";
      else if (Mutation == 8)
        Input[1].access.insert("abstract");
      else if (Mutation == 9)
        Input[1].access.insert("interface");
      else if (Mutation == 10)
        Input[1].interfaces.clear();
      else if (Mutation == 11)
        Input[4].generic_signature = "<R:Ljava/lang/Number;>Ljava/lang/Object;";
      else if (Mutation == 12)
        Input[4].generic_signature =
            "<R:Ljava/lang/Object;S:Ljava/lang/Object;>Ljava/lang/Object;";
      else if (Mutation == 13)
        Target.generic_signature =
            "(DLfixture/Base<Lfixture/Slot<TE;>;>;Lfixture/Maker<-Lfixture/"
            "Slot<TE;>;>;)V";
      else if (Mutation == 14)
        Target.generic_signature =
            "(DLfixture/Base<Lfixture/Slot<TE;>;>;Lfixture/Maker<Lfixture/"
            "Slot<TMissing;>;>;)V";
      else if (Mutation == 15)
        Caller.generic_signature =
            "(DLfixture/Base<Lfixture/Slot<TMissing;>;>;)V";
      else if (Mutation == 16)
        Caller.instructions[5].registers.pop_back();
      else if (Mutation == 17)
        Caller.instructions[5].registers[0] = 4;
      else if (Mutation == 18)
        Caller.instructions[0].registers[0] = 5;
      else if (Mutation == 19)
        Input.erase(Input.begin() + 1);
      else if (Mutation == 20)
        Target.generic_signature =
            "<Extra:Ljava/lang/Object;>(DLfixture/Base<Lfixture/"
            "Slot<TE;>;>;Lfixture/Maker<Lfixture/Slot<TE;>;>;)V";
      else if (Mutation == 21)
        Target.access.insert("varargs");
      else if (Mutation == 22)
        Caller.instructions[5].registers[2] = 1;
      else if (Mutation == 23)
        Input[1].enclosing_method =
            MethodRef{"Lfixture/Unknown;", "scope", {}, "V"};
      else if (Mutation == 24) {
        Input[1].access.erase("public");
        Input[1].access.insert("private");
      } else if (Mutation == 25)
        Input.erase(Input.begin() + 4);
      else if (Mutation == 26) {
        Input.push_back(klass("Ljava/lang/String;", true));
        Caller.instructions.insert(
            Caller.instructions.begin() + 2,
            op(4, "check-cast", {4}, "Ljava/lang/String;"));
      } else if (Mutation == 27)
        Caller.instructions.insert(
            Caller.instructions.begin() + 2,
            op(4, "check-cast", {4}, "Lfixture/Missing;"));
      else if (Mutation == 28)
        Input[1].generic_signature =
            "Ljava/lang/Object;Lfixture/Maker<Ljava/lang/Object;>;";
      else
        Target.generic_signature =
            "(DLfixture/Base<Lfixture/Slot<TE;>;>;Lfixture/Maker<*>;)V";
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}

TEST(MobileDalvikSyntheticNullaryAllocation,
     RetainCovariantAllocatedCarrierWithoutInferringContents) {
  for (bool SDK : {false, true})
    for (bool CarrierSDK : {false, true})
      for (bool Range : {false, true})
        for (bool Alpha : {false, true})
          for (bool Wide : {false, true})
            for (bool Nested : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    SDK, CarrierSDK, Range, Alpha, Wide, Nested, Route}));
                auto Input = fixture(SDK, CarrierSDK, Range, Alpha, true, Wide);
                auto &Signature = *Input[0].methods[1].generic_signature;
                const auto Maker = Signature.find("Lfixture/Maker<") +
                                   std::string("Lfixture/Maker<").size();
                Signature.insert(Maker, "+");
                if (Nested)
                  Signature.insert(Signature.find("Lfixture/Slot<", Maker) +
                                       std::string("Lfixture/Slot<").size(),
                                   "+");
                const auto Original = Input;
                const auto Caller = Input[0].methods[0].reference,
                           Target = Input[0].methods[1].reference;
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
                ASSERT_TRUE(Plans.constructor_calls.contains({Caller, Target}));
                const auto R = recoverJava(Classes, B);
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
                for (const auto &C : Original) {
                  const auto &After = Classes.at(C.name);
                  EXPECT_EQ(After.access, C.access);
                  EXPECT_EQ(After.generic_signature, C.generic_signature);
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

TEST(MobileDalvikSyntheticNullaryAllocation,
     RetainSyntheticPrimitiveCaptureWordsAndOriginalInitializer) {
  for (const std::string Capture : {"Z", "I", "J", "D"})
    for (bool SDK : {false, true})
      for (bool CarrierSDK : {false, true})
        for (bool Range : {false, true})
          for (bool Wide : {false, true})
            for (bool Covariant : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(std::tuple{
                    Capture, SDK, CarrierSDK, Range, Wide, Covariant, Route}));
                auto Input = primitiveFixture(SDK, CarrierSDK, Range, Wide,
                                              Capture, Covariant);
                const auto Original = Input;
                const auto Caller = Input[0].methods[0].reference,
                           Target = Input[0].methods[1].reference;
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
                ASSERT_TRUE(Plans.constructor_calls.contains({Caller, Target}));
                const auto R = recoverJava(Classes, B);
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
                for (const auto &C : Original) {
                  const auto &After = Classes.at(C.name);
                  EXPECT_EQ(After.access, C.access);
                  EXPECT_EQ(After.generic_signature, C.generic_signature);
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

TEST(MobileDalvikSyntheticNullaryAllocation,
     RejectUnprovedSyntheticPrimitiveCapturesScopesAndWidePairs) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 19; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{SDK, Mutation}));
      auto Input = primitiveFixture(SDK, true, true, true, "D", true);
      auto &Caller = Input[0].methods[0], &Init = Input[1].methods[0];
      if (Mutation == 0) {
        Init.access.erase("public");
        Init.access.insert("private");
      } else if (Mutation == 1)
        Init.access.insert("static");
      else if (Mutation == 2)
        Init.access.insert("native");
      else if (Mutation == 3)
        Init.access.insert("abstract");
      else if (Mutation == 4)
        Init.access.insert("bridge");
      else if (Mutation == 5)
        Init.access.insert("varargs");
      else if (Mutation == 6)
        Init.declared_throws = {"Ljava/lang/RuntimeException;"};
      else if (Mutation == 7)
        Init.generic_signature = "<P:Ljava/lang/Object;>(D)V";
      else if (Mutation == 8)
        Input[1].generic_signature = "<P:Ljava/lang/Object;>Ljava/lang/"
                                     "Object;Lfixture/Maker;Lfixture/Marker;";
      else if (Mutation == 9)
        Caller.instructions[2].registers.pop_back();
      else if (Mutation == 10)
        Caller.instructions[2].registers[2] = 4;
      else if (Mutation == 11)
        Caller.instructions[0].registers[1] = 12;
      else if (Mutation == 12) {
        Init.reference.parameters = {"Ljava/lang/Object;"};
        Caller.instructions[2].reference = Init.reference;
      } else if (Mutation == 13)
        Caller.instructions[1].registers[0] = 7;
      else if (Mutation == 14)
        Input[1].interfaces.erase(Input[1].interfaces.begin());
      else if (Mutation == 15)
        Input.erase(Input.begin() + 4);
      else if (Mutation == 16)
        Input[1].access.insert("abstract");
      else if (Mutation == 17)
        Caller.declared_throws = {"Ljava/lang/RuntimeException;"};
      else {
        auto Overwrite = op(0, "const/4", {4});
        Overwrite.literal = int64_t(2);
        Caller.instructions[0] = Overwrite;
      }
      Budget B;
      EXPECT_THROW(recoverJava(linkClasses(Input, B), B), std::runtime_error);
    }
}
