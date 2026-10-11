//===- LLVMScalarBswapTests.cpp - Independent scalar byte-order proofs ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "LLVMScalarEquivalenceTest.h"

namespace neverd::analysis::scalar_test {
namespace {
std::string swapped(unsigned Bits) {
  const auto T = "i" + std::to_string(Bits);
  return "declare " + T + " @llvm.bswap." + T + "(" + T +
         ")\n"
         "define " +
         T + " @f(" + T +
         " noundef %x) {\n"
         "%v = call " +
         T + " @llvm.bswap." + T + "(" + T +
         " %x)\n"
         "ret " +
         T + " %v\n}";
}
// Express each destination byte independently with ordinary integer operations.
std::string explicitBytes(unsigned Bits, bool WrongHighBit) {
  const auto T = "i" + std::to_string(Bits);
  std::string IR = "define " + T + " @f(" + T + " noundef %x) {\n";
  std::string Previous = "0";
  for (unsigned N = 0; N < Bits / 8; ++N) {
    const auto S = std::to_string(N);
    IR +=
        "%shift" + S + " = lshr " + T + " %x, " + std::to_string(8 * N) + "\n";
    IR += "%byte" + S + " = and " + T + " %shift" + S + ", 255\n";
    IR += "%place" + S + " = shl " + T + " %byte" + S + ", " +
          std::to_string(Bits - 8 * (N + 1)) + "\n";
    IR += "%join" + S + " = or " + T + " " + Previous + ", %place" + S + "\n";
    Previous = "%join" + S;
  }
  if (WrongHighBit) {
    IR += "%wrong = xor " + T + " " + Previous + ", " +
          std::to_string(uint64_t{1} << (Bits - 1)) + "\n";
    Previous = "%wrong";
  }
  return IR + "ret " + T + " " + Previous + "\n}";
}
} // namespace

TEST(LLVMScalarByteSwap, IndependentExpressionsRetainEveryInputBit) {
  for (unsigned Bits : {16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    const auto T = "i" + std::to_string(Bits);
    const auto Identity =
        "define " + T + " @f(" + T + " noundef %x) { ret " + T + " %x }";
    auto Twice = swapped(Bits);
    const auto Return = "ret " + T + " %v";
    Twice.replace(Twice.find(Return), Return.size(),
                  "%again = call " + T + " @llvm.bswap." + T + "(" + T +
                      " %v)\nret " + T + " %again");
    const auto RoundTrip = check(Twice, Identity);
    EXPECT_EQ(RoundTrip.Status, Status::Proved) << RoundTrip.Diagnostic;
    EXPECT_EQ(RoundTrip.CompletedPartitions, 1U);
    EXPECT_TRUE(RoundTrip.ControlBits.empty());
    // Independent constant oracle, with no host-width shifts or signed math.
    for (uint64_t Word :
         {uint64_t{0}, uint64_t{1}, UINT64_MAX, uint64_t{0x0123456789abcdef}}) {
      Word &= UINT64_MAX >> (64 - Bits);
      uint64_t Reversed = 0, Rest = Word;
      for (unsigned Byte = 0; Byte < Bits / 8; ++Byte) {
        Reversed = (Reversed << 8) | (Rest & 255);
        Rest >>= 8;
      }
      auto Constant = swapped(Bits);
      const auto Argument = "(" + T + " %x)";
      Constant.replace(Constant.find(Argument), Argument.size(),
                       "(" + T + " " + std::to_string(Word) + ")");
      const auto Reference = "define " + T + " @f(" + T +
                             " noundef %x) { ret " + T + " " +
                             std::to_string(Reversed) + " }";
      const auto Good = check(Constant, Reference);
      EXPECT_EQ(Good.Status, Status::Proved) << Good.Diagnostic;
      EXPECT_EQ(Good.CompletedPartitions, 1U);
      EXPECT_TRUE(Good.ControlBits.empty());
    }
    // The bounded scalar algebra does not equate a shift/or DAG with a
    // concatenation DAG. This refusal is not a semantic counterexample;
    // the independent complete-state oracle proves the whole permutation.
    const auto Conservative = check(swapped(Bits), explicitBytes(Bits, false));
    EXPECT_EQ(Conservative.Status, Status::Unproved) << Conservative.Diagnostic;
    EXPECT_EQ(Conservative.Diagnostic,
              "symbolic returns differ or remain unproved");
    const auto Bad = check(swapped(Bits), explicitBytes(Bits, true));
    EXPECT_EQ(Bad.Status, Status::Unproved) << Bad.Diagnostic;
  }
}

TEST(LLVMScalarByteSwap, DoubleSwapsDoNotEraseExecutedPoison) {
  const char *Identity = R"(
    define i32 @f(i32 noundef %x) { ret i32 %x }
  )";
  const std::string Guarded = R"(
    declare i32 @llvm.bswap.i32(i32)
    define i32 @f(i32 noundef %x) {
      %small = and i32 %x, 1
      %bad = add nuw i32 %small, -1
      %a = call i32 @llvm.bswap.i32(i32 %bad)
      %b = call i32 @llvm.bswap.i32(i32 %a)
      ret i32 %x
    }
  )";
  const auto R = check(Guarded, Identity);
  EXPECT_EQ(R.Status, Status::Unproved) << R.Diagnostic;
  EXPECT_EQ(R.Diagnostic, "executed source operation is not defined");
  EXPECT_EQ(R.ControlBits, (std::vector<LLVMScalarControlBit>{{0, 0}}));
  auto Defined = Guarded;
  const std::string Overflow = "add nuw i32 %small, -1";
  Defined.replace(Defined.find(Overflow), Overflow.size(),
                  "add nuw i32 %small, 0");
  const auto Safe = check(Defined, Identity);
  EXPECT_EQ(Safe.Status, Status::Proved) << Safe.Diagnostic;
  EXPECT_EQ(Safe.CompletedPartitions, 1U);
  EXPECT_TRUE(Safe.ControlBits.empty());
}
} // namespace neverd::analysis::scalar_test
