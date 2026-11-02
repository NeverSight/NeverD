//===- X86_64_TerminatorTests.cpp - x86 function terminators --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/X86/X86Interrupts.h"

#include <cstdint>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kAddress = 0x401000;

/// Whether the decoder calls \p Bytes a function terminator, decoded as
/// FuncDetector's entry check does: without the lift path's fixups, and with
/// operand detail as \p Detail says.
bool terminates(Decoder &Dec, bool Detail, const std::vector<uint8_t> &Bytes) {
  Dec.setDetail(Detail);
  DecodedInsn Insn{};
  if (Dec.decodeOneLight(Bytes.data(), Bytes.size(), kAddress, Insn) !=
      static_cast<int>(Bytes.size()))
    return false;
  EXPECT_EQ(Insn.Id, X86_INS_INT);
  return Dec.isFunctionTerminator(Insn);
}

TEST(X86Terminator, AnInterruptEndsAFunctionWithOrWithoutOperandDetail) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Architecture));
    for (unsigned Vector = 0; Vector < 256; ++Vector) {
      SCOPED_TRACE(::testing::Message() << "int " << Vector);
      const std::vector<uint8_t> Bytes = {0xCD, static_cast<uint8_t>(Vector)};
      // A resumable breakpoint also delimits a function probe; this is not a
      // claim that the interrupt can never return to its continuation.
      const bool Expected = isX86NoReturnInterrupt(Vector) || Vector == 3;
      EXPECT_EQ(terminates(Dec, true, Bytes), Expected);
      EXPECT_EQ(terminates(Dec, false, Bytes), Expected);
    }
  }
}

TEST(X86Terminator, OperandDetailOfAnEarlierDecodeDecidesNothing) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  // `int 0x29` fills the detail; `int 0x2e` without detail must not read the
  // vector it left behind.
  EXPECT_TRUE(terminates(Dec, true, {0xCD, 0x29}));
  EXPECT_FALSE(terminates(Dec, false, {0xCD, 0x2E}));
  // A prefix does not move the vector from the last byte.
  EXPECT_TRUE(terminates(Dec, false, {0x66, 0xCD, 0x29}));
}

} // namespace
