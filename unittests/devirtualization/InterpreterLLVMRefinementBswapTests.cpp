//===- InterpreterLLVMRefinementBswapTests.cpp - Fresh byte-swap premises -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "InterpreterLLVMRefinementTest.h"

namespace neverd::analysis::llvm_refinement_test {
namespace {
std::string byteSwapSource(unsigned Bits, bool WrongValue = false,
                           bool ZeroUpperWord = true) {
  const auto T = "i" + std::to_string(Bits);
  std::string Body = "%x = load " + T +
                     ", ptr %state, align 1\n"
                     "%v = call " +
                     T + " @llvm.bswap." + T + "(" + T + " %x)\n";
  std::string Value = "%v";
  if (WrongValue) {
    Body += "%wrong = xor " + T + " %v, 1\n";
    Value = "%wrong";
  }
  if (Bits == 32 && ZeroUpperWord) {
    Body += "%wide = zext i32 " + Value +
            " to i64\n"
            "store i64 %wide, ptr %state, align 1\n";
  } else {
    Body += "store " + T + " " + Value + ", ptr %state, align 1\n";
  }
  Body += "ret i64 0";
  return module(Body) + "declare " + T + " @llvm.bswap." + T + "(" + T + ")\n";
}
} // namespace

TEST(InterpreterLLVMRefinement, ByteSwapRequiresBothFreshPremises) {
  for (unsigned Bits : {16U, 32U, 64U}) {
    SCOPED_TRACE(Bits);
    // xchg al,ah / bswap eax / bswap rax, each followed by ret.
    Program P(Bits == 16 ? std::initializer_list<uint8_t>{0x86, 0xc4, 0xc3}
              : Bits == 32
                  ? std::initializer_list<uint8_t>{0x0f, 0xc8, 0xc3}
                  : std::initializer_list<uint8_t>{0x48, 0x0f, 0xc8, 0xc3});
    auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    const auto Good = P.check(Recovery.Residual, byteSwapSource(Bits));
    ASSERT_TRUE(Good.proved()) << Good.Diagnostic;
    EXPECT_TRUE(Good.Native.proved());
    EXPECT_EQ(Good.LLVM.Status, Status::Proved);
    EXPECT_TRUE(Good.Certificate);
    const auto Wrong = P.check(Recovery.Residual, byteSwapSource(Bits, true));
    rejected(Wrong, Stage::LLVM);
    EXPECT_TRUE(Wrong.Native.proved());
    EXPECT_EQ(Wrong.LLVM.Status, Status::Different);
    if (Bits == 32) {
      const auto Upper =
          P.check(Recovery.Residual, byteSwapSource(Bits, false, false));
      rejected(Upper, Stage::LLVM);
      EXPECT_TRUE(Upper.Native.proved());
      EXPECT_EQ(Upper.LLVM.Status, Status::Different);
    }
  }
}
} // namespace neverd::analysis::llvm_refinement_test
