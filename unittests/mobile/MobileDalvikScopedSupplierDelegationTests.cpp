//===- MobileDalvikScopedSupplierDelegationTests.cpp - Scoped calls
//---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify scoped provider results and checked casts before this delegation.
//===----------------------------------------------------------------------===//

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
Class klass(std::string Name) {
  Class C;
  C.name = std::move(Name);
  C.superclass = "Ljava/lang/Object;";
  C.access = {"public"};
  C.source_id = "owned-fixture";
  return C;
}
std::vector<Class> fixture(bool SDK, bool Interface, bool Range, bool Copy,
                           bool Alpha) {
  const std::string Object = "Ljava/lang/Object;",
                    CarrierName = "Lfixture/Carrier;",
                    ProviderName = "Lfixture/Provider;",
                    K = Alpha ? "Key" : "K", V = Alpha ? "Value" : "V";
  auto Root = klass(Object);
  Root.superclass.reset();
  Root.external = true;
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  auto Carrier = klass(CarrierName);
  Carrier.external = SDK;
  Carrier.access = {"public", "interface", "abstract"};
  Carrier.generic_signature =
      "<Left:Ljava/lang/Object;Right:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Provider = klass(ProviderName);
  Provider.external = SDK;
  Provider.access = Interface ? Access{"public", "interface", "abstract"}
                              : Access{"public", "abstract"};
  Provider.generic_signature = "<Result:Ljava/lang/Object;>Ljava/lang/Object;";
  auto Getter =
      method({ProviderName, "read", {}, Object}, {"public", "abstract"}, 0, {});
  Getter.generic_signature = "()TResult;";
  Provider.methods.push_back(Getter);
  auto Owner = klass("Lfixture/Core;");
  Owner.generic_signature = "<" + K + ":Ljava/lang/Object;" + V +
                            ":Ljava/lang/Object;>Ljava/lang/Object;";
  const auto Use = "Lfixture/Carrier<T" + K + ";T" + V + ";>;",
             KeyUse = "Lfixture/Carrier<Ljava/lang/Object;T" + K + ";>;",
             ValueUse = "Lfixture/Carrier<Ljava/lang/Object;T" + V + ";>;";
  auto Target = method(
      {Owner.name, "<init>", {CarrierName, CarrierName, CarrierName}, "V"},
      {"public", "constructor"}, 4,
      {op(0, "invoke-direct", {0}, Root.methods[0].reference),
       op(1, "return-void")});
  Target.generic_signature = "(" + Use + KeyUse + ValueUse + ")V";
  std::vector<Instruction> Code;
  auto Append = [&](Instruction I) {
    I.pc = Code.size();
    Code.push_back(std::move(I));
  };
  if (Copy)
    Append(op(0, "move-object", {0, 6}));
  Append(op(0,
            Interface ? (Range ? "invoke-interface/range" : "invoke-interface")
                      : (Range ? "invoke-virtual/range" : "invoke-virtual"),
            {Copy ? 0u : 6u}, Getter.reference));
  Append(op(0, "move-result-object", {0}));
  Append(op(0, "check-cast", {0}, CarrierName));
  if (Range) {
    Append(op(0, "move-object", {1, 5}));
    Append(op(0, "move-object", {2, 0}));
    Append(op(0, "move-object", {3, 7}));
    Append(op(0, "move-object", {4, 8}));
  }
  Append(op(0, Range ? "invoke-direct/range" : "invoke-direct",
            Range ? std::vector<unsigned>{1, 2, 3, 4}
                  : std::vector<unsigned>{5, 0, 7, 8},
            Target.reference));
  Append(op(0, "return-void"));
  auto Caller = method(
      {Owner.name, "<init>", {ProviderName, CarrierName, CarrierName}, "V"},
      {"public", "constructor"}, 9, Code);
  Caller.generic_signature =
      "(Lfixture/Provider<" + Use + ">;" + KeyUse + ValueUse + ")V";
  Owner.methods = {Target, Caller};
  return {Owner, Provider, Carrier, Root};
}
ClassMap input(std::vector<Class> Classes, unsigned Route, Budget &B) {
  if (Route == 2)
    std::reverse(Classes.begin(), Classes.end());
  if (Route)
    return linkClasses(Classes, B);
  ClassMap Out;
  for (auto &C : Classes)
    Out.emplace(C.name, std::move(C));
  return Out;
}
} // namespace

TEST(MobileDalvikScopedSupplierDelegation,
     PreserveScopedResultCastsAndIndependentCallbackInputs) {
  for (bool SDK : {false, true})
    for (bool Interface : {false, true})
      for (bool Range : {false, true})
        for (bool Copy : {false, true})
          for (bool Alpha : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              SCOPED_TRACE(::testing::PrintToString(
                  std::tuple{SDK, Interface, Range, Copy, Alpha, Route}));
              ASSERT_NO_THROW(([&] {
                Budget B;
                auto Classes = input(
                    fixture(SDK, Interface, Range, Copy, Alpha), Route, B);
                const auto &Owner = Classes.at("Lfixture/Core;");
                const auto Target = Owner.methods[0], Caller = Owner.methods[1];
                const auto Plans = validateGenericSignatures(Classes, B);
                EXPECT_TRUE(Plans.constructor_calls.contains(
                    {Caller.reference, Target.reference}));
                const auto R = recoverJava(Classes, B);
                EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                EXPECT_EQ(R.getInteger("minimum_java_release").value_or(8), 25);
                EXPECT_FALSE(R.getArray("generic_signature_projections"));
                EXPECT_EQ(Owner.methods[1].generic_signature,
                          Caller.generic_signature);
              })());
            }
}
TEST(MobileDalvikScopedSupplierDelegation,
     RejectMissingCastsChangedScopesAndUnprovedReceiverResults) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, true, false, false, false);
        auto &Owner = Classes[0], &Provider = Classes[1];
        auto &Target = Owner.methods[0], &Caller = Owner.methods[1],
             &Getter = Provider.methods[0];
        const std::string Source = *Caller.generic_signature;
        switch (Mutation) {
        case 0:
          Caller.generic_signature =
              "(Lfixture/Provider;Lfixture/Carrier<Ljava/lang/"
              "Object;TK;>;Lfixture/Carrier<Ljava/lang/Object;TV;>;)V";
          break;
        case 1:
          Caller.generic_signature =
              "(Lfixture/Provider<+Lfixture/Carrier<TK;TV;>;>;Lfixture/"
              "Carrier<Ljava/lang/Object;TK;>;Lfixture/Carrier<Ljava/lang/"
              "Object;TV;>;)V";
          break;
        case 2:
          Caller.generic_signature =
              "(Lfixture/Provider<Lfixture/Carrier<TV;TK;>;>;Lfixture/"
              "Carrier<Ljava/lang/Object;TK;>;Lfixture/Carrier<Ljava/lang/"
              "Object;TV;>;)V";
          break;
        case 3:
          Getter.generic_signature = "()Ljava/lang/Object;";
          break;
        case 4:
          Getter.generic_signature = "<Result:Ljava/lang/Object;>()TResult;";
          break;
        case 5:
          Getter.generic_signature = "()TMissing;";
          break;
        case 6:
          Caller.instructions[2] = op(2, "nop");
          break;
        case 7:
          Caller.instructions[2].reference = std::string("Ljava/lang/Object;");
          break;
        case 8:
          Caller.instructions[2] = op(2, "const/4", {0});
          Caller.instructions[2].literal = int64_t{1};
          break;
        case 9:
          std::swap(Caller.instructions[3].registers[2],
                    Caller.instructions[3].registers[3]);
          break;
        case 10:
          Caller.instructions[0].opcode = "invoke-virtual";
          break;
        case 11:
          Getter.access.insert("synthetic");
          break;
        case 12:
          Getter.access.insert("bridge");
          break;
        case 13:
          Getter.access.erase("abstract");
          Getter.access.insert("native");
          break;
        case 14:
          Getter.declared_throws =
              std::vector<std::string>{"Ljava/lang/Throwable;"};
          break;
        case 15:
          Provider.generic_signature =
              "<Other:Ljava/lang/Object;>Ljava/lang/Object;";
          break;
        case 16:
          Target.generic_signature =
              "<Extra:Ljava/lang/Object;>(Lfixture/Carrier<TK;TV;>;Lfixture/"
              "Carrier<Ljava/lang/Object;TK;>;Lfixture/Carrier<Ljava/lang/"
              "Object;TV;>;)V";
          break;
        case 17:
          Caller.generic_signature =
              Source.substr(0, Source.size() - 1) + "Ljava/lang/Object;";
          break;
        case 18:
          Caller.instructions[0].registers = {5};
          break;
        case 19:
          Caller.generic_signature =
              "(Lfixture/Provider<Lfixture/Carrier<Ljava/lang/Object;Ljava/"
              "lang/Object;>;>;Lfixture/Carrier<Ljava/lang/"
              "Object;TK;>;Lfixture/Carrier<Ljava/lang/Object;TV;>;)V";
          break;
        }
        try {
          Budget B;
          auto Linked = input(std::move(Classes), Route, B);
          const auto &C = Linked.at("Lfixture/Core;");
          const auto Plans = validateGenericSignatures(Linked, B);
          EXPECT_FALSE(Plans.constructor_calls.contains(
              {C.methods[1].reference, C.methods[0].reference}));
        } catch (const std::runtime_error &) {
        }
      }
}
