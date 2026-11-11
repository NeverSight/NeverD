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
std::vector<Class> fixture(bool SDK, bool Range, bool Alpha, bool Self,
                           bool Leading, bool Copy) {
  const std::string Object = "Ljava/lang/Object;", T = Alpha ? "Value" : "T",
                    Name = "Lfixture/Link;",
                    Carrier = Self ? Name : "Lfixture/Peer;";
  auto Root = klass(Object, true);
  Root.superclass.reset();
  Root.methods.push_back(
      method({Object, "<init>", {}, "V"}, {"public", "constructor"}, 0, {}));
  auto Link = klass(Name);
  Link.generic_signature = "<" + T + ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Peer = klass("Lfixture/Peer;", SDK);
  Peer.access = {"public", "interface", "abstract"};
  Peer.generic_signature = "<E:Ljava/lang/Object;>Ljava/lang/Object;";
  if (!Self) {
    Link.interfaces = {Peer.name};
    *Link.generic_signature += "Lfixture/Peer<T" + T + ";>;";
  }
  const std::string CarrierType =
      Carrier.substr(0, Carrier.size() - 1) + "<T" + T + ";>;";
  auto Target = method({Name, "<init>", {Object, Object, Carrier}, "V"},
                       {"public", "constructor"}, 4,
                       {op(0, "invoke-direct", {0}, Root.methods[0].reference),
                        op(1, "return-void")});
  Target.generic_signature = "(T" + T + ";T" + T + ";" + CarrierType + ")V";
  auto Scalar =
      method({Name, "<init>", {Object, Object, Carrier, "Z", "Z"}, "V"},
             {"public", "constructor"}, 6,
             {op(0, "invoke-direct", {0}, Root.methods[0].reference),
              op(1, "return-void")});
  Scalar.generic_signature = "(T" + T + ";T" + T + ";" + CarrierType + "ZZ)V";
  Method Caller;
  unsigned Null = 0, This = 1, First = 2, Next = 3;
  if (Range) {
    This = 0;
    First = Leading ? 2 : 1;
    Null = Leading ? 1 : 2;
    Next = 3;
    Caller = method(
        {Name, "<init>", {Object, Carrier}, "V"}, {"public", "constructor"}, 7,
        {op(0, "move-object", {0, 4}), op(1, "move-object", {First, 5}),
         op(2, "const/4", {Null}), op(3, "move-object", {3, 6}),
         op(4, "invoke-direct/range", {0, 1, 2, 3}, Target.reference),
         op(5, "return-void")});
    Caller.instructions[2].literal = int64_t(0);
  } else {
    if (Copy) {
      This = 3;
      First = 4;
      Next = 5;
    }
    auto Zero = op(0, "const/4", {0});
    Zero.literal = int64_t(0);
    std::vector<Instruction> Code{Zero};
    if (Copy) {
      Code.push_back(op(1, "move-object", {1, 0}));
      Null = 1;
    }
    Code.push_back(op(Code.size(), "invoke-direct",
                      Leading ? std::vector<unsigned>{This, Null, First, Next}
                              : std::vector<unsigned>{This, First, Null, Next},
                      Target.reference));
    Code.push_back(op(Code.size(), "return-void"));
    Caller = method({Name, "<init>", {Object, Carrier}, "V"},
                    {"public", "constructor"}, Copy ? 6 : 4, Code);
  }
  Caller.generic_signature = "(T" + T + ";" + CarrierType + ")V";
  Link.methods = {Caller, Target, Scalar};
  return {Link, Peer, Root, klass("Ljava/lang/String;", true),
          klass("Ljava/lang/Number;", true)};
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

TEST(MobileDalvikScopedNullDelegation,
     KeepDecodedNullSeparateFromScopedForwardedSeedsAndCarriers) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Alpha : {false, true})
        for (bool Self : {false, true})
          for (bool Leading : {false, true})
            for (bool Copy : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(::testing::PrintToString(
                    std::tuple{SDK, Range, Alpha, Self, Leading, Copy, Route}));
                ASSERT_NO_THROW(([&] {
                  Budget B;
                  auto Classes =
                      input(fixture(SDK, Range, Alpha, Self, Leading, Copy),
                            Route, B);
                  const auto Caller = Classes.at("Lfixture/Link;").methods[0],
                             Target = Classes.at("Lfixture/Link;").methods[1];
                  const auto Plans = validateGenericSignatures(Classes, B);
                  EXPECT_TRUE(
                      Plans.constructor_delegations.contains(Caller.reference));
                  EXPECT_TRUE(Plans.constructor_calls.contains(
                      {Caller.reference, Target.reference}));
                  const auto R = recoverJava(Classes, B);
                  EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                  EXPECT_FALSE(R.getArray("generic_signature_projections"));
                  EXPECT_EQ(Classes.at(Target.reference.owner)
                                .methods[0]
                                .generic_signature,
                            Caller.generic_signature);
                  EXPECT_EQ(Classes.at(Target.reference.owner)
                                .methods[1]
                                .generic_signature,
                            Target.generic_signature);
                })());
              }
}

TEST(
    MobileDalvikScopedNullDelegation,
    RejectNonzeroWideUnknownScopesMismatchedErasuresAndInvalidDelegationWords) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 16; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, false, false, false, false, false);
        auto &Link = Classes[0];
        auto &Caller = Link.methods[0], &Target = Link.methods[1];
        switch (Mutation) {
        case 0:
          Caller.instructions[0].literal = int64_t(1);
          break;
        case 1:
          Caller.instructions[0].opcode = "const-wide/16";
          break;
        case 2:
          Target.generic_signature = "(TT;TMissing;Lfixture/Peer<TT;>;)V";
          break;
        case 3:
          Target.generic_signature =
              "(TT;Ljava/lang/String;Lfixture/Peer<TT;>;)V";
          break;
        case 4:
          Caller.generic_signature = "(TT;Lfixture/Peer<*>;)V";
          break;
        case 5:
          Caller.generic_signature = "(TT;Lfixture/Peer<+TT;>;)V";
          break;
        case 6:
          Caller.instructions[1].registers.pop_back();
          break;
        case 7:
          Caller.instructions[1].registers[0] = 0;
          break;
        case 8:
          Caller.instructions[1].registers[1] = 3;
          break;
        case 9:
          Caller.instructions[1].registers.back() = 4;
          break;
        case 10:
          Caller.instructions[0].registers[0] = 1;
          break;
        case 11:
          Target.access.insert("static");
          break;
        case 12:
          Target.access.insert("native");
          break;
        case 13:
          Target.generic_signature = "(TT;TT;Lfixture/Peer<TMissing;>;)V";
          break;
        case 14:
          Link.generic_signature =
              "<T:Ljava/lang/Number;>Ljava/lang/Object;Lfixture/Peer<TT;>;";
          break;
        case 15:
          Caller.instructions[1].opcode = "invoke-virtual";
          break;
        }
        EXPECT_THROW(([&] {
                       Budget B;
                       recoverJava(input(Classes, Route, B), B);
                     }()),
                     std::runtime_error);
      }
}
