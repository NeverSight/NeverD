//===- AArch64DecodeValidityTests.cpp - Decode validity and lift admission ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Illegal instructions remain inspectable, but never acquire invented LowIR.
/// Reused full, light and native decoder buffers must not carry validity state.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/AArch64/AArch64Lifter.h"
#include "neverd/support/BinaryEncoding.h"

#include <cstdint>
#include <vector>

using namespace neverd;

TEST(AArch64DecodeValidity, IllegalOperandsRefuseSemanticsAcrossDecodePaths) {
  // LDR writeback overlaps its destination; LDP duplicates a GPR or FP
  // destination; LDP/STP writeback overlaps one of their transferred registers.
  for (const uint32_t Word :
       {0xf8408c00u, 0xa9400020u, 0xad400020u, 0xa9c10400u, 0xa9810400u})
    for (unsigned Route = 0; Route != 3; ++Route) {
      SCOPED_TRACE(Word);
      SCOPED_TRACE(Route);
      Decoder D;
      ASSERT_TRUE(D.init(Arch::AArch64));
      const auto Decode = [&](uint32_t Encoding, DecodedInsn &I) {
        uint8_t Bytes[4];
        writeLE<uint32_t>(Bytes, Encoding);
        return Route == 0 ? D.decodeOne(Bytes, sizeof(Bytes), 0x1000, I)
               : Route == 1
                   ? D.decodeOneForLift(Bytes, sizeof(Bytes), 0x1000, I)
                   : D.decodeOneLight(Bytes, sizeof(Bytes), 0x1000, I);
      };
      // ADD uses the native fast path; MRS falls back to the full decoder.
      for (const uint32_t Valid : {0x91000400u, 0xd53b4200u}) {
        DecodedInsn I;
        ASSERT_EQ(Decode(Word, I), 4);
        ASSERT_TRUE(I.Raw->illegal);
        std::vector<LowOp> Ops;
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
        AArch64Lifter Direct(Arch::AArch64);
        Direct.setStrict(false);
        EXPECT_THROW(Direct.lift(I.Raw, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
        ASSERT_EQ(Decode(Valid, I), 4);
        EXPECT_FALSE(I.Raw->illegal);
        EXPECT_NO_THROW(D.liftToLow(I, Ops));
        EXPECT_FALSE(Ops.empty());
      }
    }
}
