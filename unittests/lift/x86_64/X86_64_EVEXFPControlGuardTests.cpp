//===- X86_64_EVEXFPControlGuardTests.cpp - FP control safety contract ----===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"

#include <array>
#include <cstdint>
#include <functional>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kAddress = 0x1000;
using Encoding = std::array<uint8_t, 7>;
using Mutation = std::function<void(DecodedInsn &)>;

struct FamilyCases {
  const char *Name;
  Encoding Immediate12;
  Encoding Immediate34;
  Encoding Scalar;
  Encoding Broadcast;
  Encoding SuppressAllExceptions;
};

const std::array<FamilyCases, 1> kFamilies = {{
    {"Range",
     {0x62, 0xf3, 0x65, 0xca, 0x50, 0xcc, 0x12},
     {0x62, 0xf3, 0x65, 0xca, 0x50, 0xcc, 0x34},
     {0x62, 0xf3, 0xe5, 0x8a, 0x51, 0xcc, 0x56},
     {0x62, 0xf3, 0xe5, 0x5a, 0x50, 0x08, 0x78},
     {0x62, 0xf3, 0x65, 0xda, 0x50, 0xcc, 0x12}},
}};

void expectFailClosed(const Encoding &Bytes, const Mutation &Mutate = {}) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  DecodedInsn I{};
  ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, I),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(I.Raw, nullptr);
  ASSERT_NE(I.Raw->detail, nullptr);
  if (Mutate)
    Mutate(I);

  std::vector<LowOp> Ops;
  EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST(X86EVEXFPControlGuard,
     DistinctImmediateControlsNeverCollapseToApproximateIR) {
  for (const FamilyCases &Family : kFamilies) {
    SCOPED_TRACE(Family.Name);
    expectFailClosed(Family.Immediate12);
    expectFailClosed(Family.Immediate34);
  }
}

TEST(X86EVEXFPControlGuard,
     MaskedScalarPassThroughAndPackedBroadcastRemainFailClosed) {
  for (const FamilyCases &Family : kFamilies) {
    SCOPED_TRACE(Family.Name);
    expectFailClosed(Family.Scalar);
    expectFailClosed(Family.Broadcast);
  }
}

TEST(X86EVEXFPControlGuard,
     SuppressAllExceptionsControlCannotReuseOrdinaryApproximation) {
  for (const FamilyCases &Family : kFamilies) {
    SCOPED_TRACE(Family.Name);
    expectFailClosed(Family.SuppressAllExceptions);
  }
}

TEST(X86EVEXFPControlGuard, RawAndDetailMutationsNeverLeavePartialIR) {
  for (const FamilyCases &Family : kFamilies) {
    SCOPED_TRACE(Family.Name);
    expectFailClosed(Family.Immediate12,
                     [](DecodedInsn &I) { I.Raw->bytes[4] ^= 1; });
    expectFailClosed(Family.Immediate12, [](DecodedInsn &I) {
      cs_x86 &X86 = I.Raw->detail->x86;
      ASSERT_GT(X86.op_count, 0);
      ASSERT_EQ(X86.operands[X86.op_count - 1].type, X86_OP_IMM);
      X86.operands[X86.op_count - 1].imm ^= 1;
    });
    expectFailClosed(Family.Immediate12, [](DecodedInsn &I) {
      cs_x86 &X86 = I.Raw->detail->x86;
      ASSERT_GE(X86.op_count, 2);
      ASSERT_EQ(X86.operands[1].type, X86_OP_REG);
      X86.operands[1].reg = X86_REG_K3;
      X86.operands[1].avx_zero_opmask = false;
    });
  }
}

} // namespace
