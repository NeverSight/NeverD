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
std::vector<Class> fixture(bool SDK, bool Range, bool Wide, bool Copy,
                           bool Alpha, bool Covariant, bool CarrierFirst,
                           int64_t Bits) {
  const std::string Object = "Ljava/lang/Object;",
                    CarrierName = "Lfixture/Carrier;",
                    Scalar = Wide ? "D" : "F", Integer = Wide ? "J" : "I",
                    K = Alpha ? "Key" : "K", V = Alpha ? "Value" : "V",
                    Variance = Covariant ? "+" : "";
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
  const std::string Use = "Lfixture/Carrier<" + Variance + "T" + K + ";" +
                          Variance + "T" + V + ";>;";
  auto Owner = klass("Lfixture/Core;");
  Owner.generic_signature = "<" + K + ":Ljava/lang/Object;" + V +
                            ":Ljava/lang/Object;>Ljava/lang/Object;";
  auto Target =
      method({Owner.name, "<init>",
              CarrierFirst ? std::vector<std::string>{CarrierName, Scalar}
                           : std::vector<std::string>{Scalar, CarrierName},
              "V"},
             {"public", "constructor"}, Wide ? 4 : 3,
             {op(0, "invoke-direct", {0}, Root.methods[0].reference),
              op(1, "return-void")});
  Target.generic_signature =
      "(" + (CarrierFirst ? Use + Scalar : Scalar + Use) + ")V";
  auto Other = Target;
  Other.reference.parameters[CarrierFirst ? 1 : 0] = Integer;
  Other.generic_signature =
      "(" + (CarrierFirst ? Use + Integer : Integer + Use) + ")V";
  const unsigned ScalarSlot = Range ? (CarrierFirst ? 2 : 1) : 0,
                 CarrierSlot = Range ? (CarrierFirst ? 1
                                        : Wide       ? 3
                                                     : 2)
                                     : 7,
                 Definition = Copy ? (Range ? 4 : 2) : ScalarSlot;
  std::vector<Instruction> Code;
  auto Append = [&](Instruction I) {
    I.pc = Code.size();
    Code.push_back(std::move(I));
  };
  auto Constant = op(0, Wide ? "const-wide" : "const", {Definition});
  Constant.literal = Bits;
  Append(Constant);
  if (Copy)
    Append(op(0, Wide ? "move-wide" : "move", {ScalarSlot, Definition}));
  if (Range) {
    Append(op(0, "move-object", {0, 6}));
    Append(op(0, "move-object", {CarrierSlot, 7}));
  }
  std::vector<unsigned> Arguments{Range ? 0u : 6u};
  if (CarrierFirst)
    Arguments.push_back(CarrierSlot);
  Arguments.push_back(ScalarSlot);
  if (Wide)
    Arguments.push_back(ScalarSlot + 1);
  if (!CarrierFirst)
    Arguments.push_back(CarrierSlot);
  Append(op(0, Range ? "invoke-direct/range" : "invoke-direct", Arguments,
            Target.reference));
  Append(op(0, "return-void"));
  auto Caller = method({Owner.name, "<init>", {CarrierName}, "V"},
                       {"public", "constructor"}, 8, Code);
  Caller.generic_signature = "(" + Use + ")V";
  Owner.methods = {Target, Caller, Other};
  return {Owner, Carrier, Root};
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

TEST(MobileDalvikFloatingConstructorDefault,
     KeepDecodedFloatingBitsCompleteScopedInputsAndExactOverload) {
  for (bool SDK : {false, true})
    for (bool Range : {false, true})
      for (bool Wide : {false, true})
        for (bool Copy : {false, true})
          for (bool Alpha : {false, true})
            for (bool Covariant : {false, true})
              for (bool CarrierFirst : {false, true})
                for (int64_t Bits :
                     Wide ? std::vector<int64_t>{0, INT64_MIN, 1,
                                                 INT64_C(4603804719079489536),
                                                 INT64_C(9221120237041095220),
                                                 INT64_C(-4503599627370496)}
                          : std::vector<int64_t>{0, INT32_MIN, 1, 1059061760,
                                                 2143294004, -8388608})
                  for (unsigned Route : {0u, 1u, 2u}) {
                    SCOPED_TRACE(::testing::PrintToString(
                        std::tuple{SDK, Range, Wide, Copy, Alpha, Covariant,
                                   CarrierFirst, Bits, Route}));
                    ASSERT_NO_THROW(([&] {
                      Budget B;
                      auto Classes =
                          input(fixture(SDK, Range, Wide, Copy, Alpha,
                                        Covariant, CarrierFirst, Bits),
                                Route, B);
                      const auto &Owner = Classes.at("Lfixture/Core;");
                      const auto Target = Owner.methods[0],
                                 Caller = Owner.methods[1];
                      const auto Plans = validateGenericSignatures(Classes, B);
                      EXPECT_TRUE(Plans.constructor_calls.contains(
                          {Caller.reference, Target.reference}));
                      const auto R = recoverJava(Classes, B);
                      EXPECT_EQ(R.getInteger("unrecovered_method_count"), 0);
                      EXPECT_EQ(
                          R.getInteger("minimum_java_release").value_or(8), 8);
                      EXPECT_FALSE(R.getArray("generic_signature_projections"));
                      EXPECT_EQ(Owner.methods[1].generic_signature,
                                Caller.generic_signature);
                    })());
                  }
}

TEST(MobileDalvikFloatingConstructorDefault,
     RejectWidthChangesIntegerInputsFieldsStalePairsAndForeignScopes) {
  for (bool SDK : {false, true})
    for (unsigned Mutation = 0; Mutation < 20; ++Mutation)
      for (unsigned Route : {0u, 1u, 2u}) {
        SCOPED_TRACE(
            ::testing::PrintToString(std::tuple{SDK, Mutation, Route}));
        auto Classes = fixture(SDK, false, true, false, false, true, false, 0);
        auto &Owner = Classes[0];
        auto &Target = Owner.methods[0], &Caller = Owner.methods[1];
        auto &Constant = Caller.instructions[0], &Call = Caller.instructions[1];
        switch (Mutation) {
        case 0:
          Constant.opcode = "const-unknown";
          break;
        case 1:
          Constant.reference = std::string("Ljava/lang/Object;");
          break;
        case 2:
          Constant.literal = std::string("owned malformed constant");
          break;
        case 3:
          Constant.registers[0] = 6;
          break;
        case 4:
          Constant.opcode = "const";
          break;
        case 5:
          Call.registers[2] = Call.registers[1];
          break;
        case 6:
          Target.reference.parameters[0] = "F";
          Target.generic_signature = "(FLfixture/Carrier<+TK;+TV;>;)V";
          Call.reference = Target.reference;
          break;
        case 7:
          Caller.generic_signature = "(Lfixture/Carrier<+TV;+TK;>;)V";
          break;
        case 8:
          Caller.generic_signature = "(Lfixture/Carrier<+TMissing;+TV;>;)V";
          break;
        case 9:
          Target.generic_signature =
              "<X:Ljava/lang/Object;>(DLfixture/Carrier<+TK;+TV;>;)V";
          break;
        case 10:
          Target.access.insert("static");
          break;
        case 11:
          Target.access.insert("synthetic");
          break;
        case 12:
          Target.declared_throws =
              std::vector<std::string>{"Ljava/lang/Exception;"};
          break;
        case 13:
          Owner.methods[2].generic_signature =
              "<X:Ljava/lang/Object;>(JLfixture/Carrier<+TK;+TV;>;)V";
          break;
        case 14:
          Caller.reference.parameters.push_back("J");
          Caller.generic_signature = "(Lfixture/Carrier<+TK;+TV;>;J)V";
          Caller.registers = 10;
          Call.registers = {6, 8, 9, 7};
          break;
        case 15: {
          FieldRef F{Owner.name, "bits", "J"};
          Owner.fields.push_back({F, {"public", "static"}, {}});
          Constant.opcode = "sget-wide";
          Constant.literal = std::monostate{};
          Constant.reference = F;
          break;
        }
        case 16:
          Caller.instructions.insert(Caller.instructions.begin() + 1,
                                     op(1, "move", {1, 0}));
          break;
        case 17:
          Constant.target = 1;
          break;
        case 18:
          Caller.generic_signature = "(Lfixture/Carrier<-TK;+TV;>;)V";
          break;
        case 19:
          Call.registers[3] = 6;
          break;
        }
        for (size_t At = 0; At < Caller.instructions.size(); ++At)
          Caller.instructions[At].pc = At;
        Caller.code_end = Caller.instructions.size();
        EXPECT_THROW(([&] {
                       Budget B;
                       auto C = input(Classes, Route, B);
                       recoverJava(C, B);
                     })(),
                     std::runtime_error);
      }
}
