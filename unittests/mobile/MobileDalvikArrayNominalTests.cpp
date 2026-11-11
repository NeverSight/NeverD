#include "MobileDalvik.h"
#include "MobileDalvikSignature.h"
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
std::vector<Class> fixture(bool Range, bool External, bool Public, bool Facade,
                           bool Frame, bool Strings, bool Header, bool Shadow) {
  const std::string Base = "Lfixture/api/Backend;",
                    View = "Lfixture/api/Facade;", Client = "Lclient/Client;",
                    Result = "Lfixture/Receipt;", Object = "Ljava/lang/Object;";
  auto Target = method({Base, "collect", {"[Ljava/lang/Object;"}, Result},
                       {"public", "static", "final"}, 2,
                       {op(0, "const/4", {0}), op(1, "return-object", {0})});
  Target.instructions[0].literal = int64_t(0);
  Target.generic_signature = "<T:Ljava/lang/Object;>([TT;)Lfixture/Receipt;";
  auto Provider = klass(Base, {Target}, External);
  Provider.access = Public ? Access{"public"} : Access{};
  if (External) {
    Provider.methods[0].instructions.clear();
    Provider.methods[0].registers = Provider.methods[0].code_end = 0;
  }
  if (Shadow)
    Provider.generic_signature = "<T:Ljava/lang/Number;>Ljava/lang/Object;";
  auto PublicView = klass(View);
  PublicView.superclass = Base;
  auto CallRef = Target.reference;
  CallRef.owner = Facade ? View : Base;
  auto Caller =
      method({Client,
              "call",
              {Strings ? "[Ljava/lang/String;" : "[Ljava/lang/Object;"},
              Result},
             {"public", "static"}, Frame ? 5 : 3,
             {op(0, Range ? "invoke-static/range" : "invoke-static",
                 {Frame ? 4u : 2u}, CallRef),
              op(1, "move-result-object", {0}), op(2, "return-object", {0})});
  auto Receipt = klass(Result);
  auto Marker = klass("Lfixture/Marker;");
  Marker.access = {"public", "interface", "abstract"};
  Marker.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  if (Header) {
    Receipt.interfaces = {Marker.name};
    Receipt.generic_signature =
        "Ljava/lang/Object;Lfixture/Marker<Ljava/lang/String;>;";
  }
  auto Root = klass(Object, {}, true);
  Root.access = {"public"};
  Root.superclass.reset();
  auto String = klass("Ljava/lang/String;", {}, true);
  auto Number = klass("Ljava/lang/Number;", {}, true);
  Number.access = {"public"};
  return {Provider, PublicView, klass(Client, {Caller}), Receipt, Marker, Root,
          String,   Number};
}
std::vector<Class> projectedFixture(bool External, bool Second, bool Shadow) {
  auto Input =
      fixture(false, External, false, true, false, false, false, Shadow);
  const std::string Comparable = "Ljava/lang/Comparable;";
  const std::string Character = "Ljava/lang/Character;";
  auto &Result = Input[3];
  auto Api = klass("Lfixture/ComparableApi;");
  Api.access = {"public", "interface", "abstract"};
  Api.external = External;
  Api.generic_signature = "<T::Ljava/lang/Comparable<-TT;>;>Ljava/lang/Object;";
  auto Contract = method({Api.name, "contains", {Comparable}, "Z"},
                         {"public", "abstract"}, 0);
  Contract.generic_signature = "(TT;)Z";
  Api.methods = {Contract};
  auto Target = method({Result.name, "contains", {"C"}, "Z"}, {"public"}, 3,
                       {op(0, "const/4", {0}), op(1, "return", {0})});
  Target.instructions[0].literal = int64_t(1);
  auto Bridge = method(
      {Result.name, "contains", {Comparable}, "Z"},
      {"public", "bridge", "synthetic"}, 3,
      {op(0, "check-cast", {2}, Character),
       op(2, "invoke-virtual", {2}, MethodRef{Character, "charValue", {}, "C"}),
       op(5, "move-result", {0}),
       op(6, "invoke-virtual", {1, 0}, Target.reference),
       op(9, "move-result", {0}), op(10, "return", {0})});
  Result.methods = {Target, Bridge};
  Result.interfaces = {Api.name};
  Result.generic_signature =
      "Ljava/lang/Object;Lfixture/ComparableApi<Ljava/lang/Character;>;";
  auto Box =
      klass(Character,
            {method({Character, "charValue", {}, "C"}, {"public"}, 0)}, true);
  Box.interfaces = {Comparable};
  Box.generic_signature =
      "Ljava/lang/Object;Ljava/lang/Comparable<Ljava/lang/Character;>;";
  auto Bound = klass(Comparable, {}, true);
  Bound.access = {"public", "interface", "abstract"};
  Bound.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  Input.push_back(Api);
  Input.push_back(Box);
  Input.push_back(Bound);
  if (Second) {
    Api.name = "Lfixture/OtherComparableApi;";
    Api.methods[0].reference.owner = Api.name;
    Input[3].interfaces.push_back(Api.name);
    *Input[3].generic_signature +=
        "Lfixture/OtherComparableApi<Ljava/lang/Character;>;";
    Input.push_back(Api);
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
TEST(MobileDalvikArrayNominal,
     OwnArrayComponentKeepsFixedResultAndFacadeLookup) {
  for (bool Range : {false, true})
    for (bool External : {false, true})
      for (bool Public : {false, true})
        for (bool Facade : {false, true})
          for (bool Frame : {false, true})
            for (bool Strings : {false, true})
              for (bool Header : {false, true})
                for (bool Shadow : {false, true})
                  for (unsigned Route = 0; Route < 3; ++Route) {
                    if (!Public && !Facade)
                      continue;
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{Range, External, Public, Facade, Frame,
                                   Strings, Header, Shadow, Route}));
                    auto Input = fixture(Range, External, Public, Facade, Frame,
                                         Strings, Header, Shadow);
                    const auto Target = Input[0].methods[0].reference;
                    Budget B;
                    auto Classes = classes(Input, Route, B);
                    auto Plans = validateGenericSignatures(Classes, B);
                    EXPECT_TRUE(
                        Plans.erased_generic_invocations.contains(Target));
                    auto R = recoverJava(Classes, B);
                    EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                    EXPECT_EQ(
                        R.getInteger("projected_method_count").value_or(0), 0);
                    EXPECT_FALSE(R.getArray("generic_signature_projections"));
                  }
}
TEST(MobileDalvikArrayNominal, WrongBoundsDescriptorsDispatchAndAccessFail) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation) {
      SCOPED_TRACE(::testing::PrintToString(std::tuple{Route, Mutation}));
      auto Input =
          fixture(false, false, false, true, false, false, false, false);
      auto &Target = Input[0].methods[0];
      auto &Call = Input[2].methods[0].instructions[0];
      switch (Mutation) {
      case 0:
        Target.generic_signature =
            "<T:Ljava/lang/Number;>([TT;)Lfixture/Receipt;";
        break;
      case 1:
        Target.generic_signature =
            "<T:Ljava/lang/Object;>([TU;)Lfixture/Receipt;";
        break;
      case 2:
        Target.generic_signature =
            "<T:Ljava/lang/Object;>(TT;)Lfixture/Receipt;";
        break;
      case 3:
        Target.generic_signature =
            "<T:Ljava/lang/Object;>([TT;)Ljava/lang/Object;";
        break;
      case 4:
        Target.generic_signature =
            "<T:Ljava/lang/Object;>([TT;I)Lfixture/Receipt;";
        break;
      case 5:
        Target.access.erase("static");
        break;
      case 6:
        Target.access.insert("bridge");
        break;
      case 7:
        Target.access.insert("synthetic");
        break;
      case 8:
        Target.access.insert("varargs");
        break;
      case 9:
        Target.access.erase("public");
        Target.access.insert("private");
        break;
      case 10:
        std::get<MethodRef>(Call.reference).owner = Input[0].name;
        break;
      case 11:
        Call.opcode = "invoke-interface";
        break;
      case 12:
        Call.registers.clear();
        break;
      case 13:
        std::get<MethodRef>(Call.reference).parameters[0] =
            "Ljava/lang/Object;";
        break;
      case 14:
        Input[3].access.clear();
        break;
      case 15:
        Target.generic_signature =
            "<T:Ljava/lang/Object;>([TT;)Lfixture/Receipt<TT;>;";
        break;
      case 16:
        Target.access.insert("native");
        break;
      case 17:
        Target.instructions.clear();
        Target.code_end = 0;
        break;
      case 18:
        Target.generic_signature =
            "<T::Lfixture/Missing;>([TT;)Lfixture/Receipt;";
        break;
      case 19:
        Target.reference.parameters[0] = "[I";
        break;
      }
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}

TEST(MobileDalvikArrayNominal, ProjectedResultKeepsItsOriginalNominalHeader) {
  for (bool External : {false, true})
    for (bool Second : {false, true})
      for (bool Shadow : {false, true})
        for (unsigned Route = 0; Route < 3; ++Route) {
          SCOPED_TRACE(::testing::PrintToString(
              std::tuple{External, Second, Shadow, Route}));
          auto Input = projectedFixture(External, Second, Shadow);
          Budget B;
          auto Classes = classes(Input, Route, B);
          auto Plans = validateGenericSignatures(Classes, B);
          ASSERT_TRUE(Plans.inheritance_contracts.contains(Input[3].name));
          EXPECT_EQ(Plans.classes.at(Input[3].name).source_projections.size(),
                    Second ? 2u : 1u);
          EXPECT_TRUE(Plans.erased_generic_invocations.contains(
              Input[0].methods[0].reference));
          auto R = recoverJava(Classes, B);
          EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
          EXPECT_NE(R.getArray("generic_signature_projections"), nullptr);
        }
}
TEST(MobileDalvikArrayNominal,
     ProjectedResultCannotExcuseIncompleteDeclarations) {
  for (unsigned Route = 0; Route < 3; ++Route)
    for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
      auto Input = projectedFixture(false, true, true);
      if (Mutation == 0)
        Input[3].interfaces.pop_back();
      else if (Mutation == 1)
        Input[3].generic_signature =
            "<E:Ljava/lang/Object;>Ljava/lang/Object;Lfixture/"
            "ComparableApi<Ljava/lang/Character;>;Lfixture/"
            "OtherComparableApi<Ljava/lang/Character;>;";
      else if (Mutation == 2)
        Input[3].methods[1].instructions[1].registers.push_back(0);
      else
        Input[3].access.clear();
      Budget B;
      EXPECT_THROW(recoverJava(classes(Input, Route, B), B),
                   std::runtime_error);
    }
}
