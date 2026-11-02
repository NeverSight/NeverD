#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/APInt.h"

using namespace neverd;

TEST(AArch64Division, DefinedExceptionalResultsAndAliasedDestinations) {
  for (bool Signed : {false, true})
    for (unsigned Bits : {32u, 64u})
      for (unsigned Destination : {0u, 1u, 2u})
        for (bool FastDecode : {false, true}) {
          SCOPED_TRACE(Signed);
          SCOPED_TRACE(Bits);
          SCOPED_TRACE(Destination);
          SCOPED_TRACE(FastDecode);
          Decoder Dec;
          ASSERT_TRUE(Dec.init(Arch::AArch64));
          Dec.setStrict(true);
          const uint32_t Word = (Bits == 64 ? 0x9AC00800u : 0x1AC00800u) |
                                (Signed ? 0x400u : 0u) | (2u << 16) |
                                (1u << 5) | Destination;
          uint8_t Bytes[4];
          writeLE<uint32_t>(Bytes, Word);
          DecodedInsn Insn;
          ASSERT_EQ(FastDecode ? Dec.decodeOneForLift(Bytes, 4, 0x1000, Insn)
                               : Dec.decodeOne(Bytes, 4, 0x1000, Insn),
                    4);
          std::vector<LowOp> Ops;
          ASSERT_NO_THROW(Dec.liftToLow(Insn, Ops));
          const uint64_t Sign = UINT64_C(1) << (Bits - 1);
          const uint64_t Mask = Bits == 64 ? UINT64_MAX : UINT32_MAX;
          const uint64_t Values[] = {0,        1,    2,        7,        19,
                                     Sign - 1, Sign, Sign + 1, Mask - 1, Mask};
          for (uint64_t A : Values)
            for (uint64_t B : Values) {
              SCOPED_TRACE(A);
              SCOPED_TRACE(B);
              uint64_t Expected = 0;
              if (B != 0) {
                if (!Signed)
                  Expected = A / B;
                else if (A == Sign && B == Mask)
                  Expected = Sign;
                else
                  Expected = llvm::APInt(Bits, A)
                                 .sdiv(llvm::APInt(Bits, B))
                                 .getZExtValue();
              }
              BinaryImage Image;
              Image.Arch = Arch::AArch64;
              Image.Bits = Bitness::Bits64;
              NdOpEmulator Emu(Image);
              Emu.setStrictMode(true);
              // A W destination must replace an existing full-width value.
              Emu.setRegister(a64reg::X0, UINT64_MAX);
              Emu.setRegister(a64reg::X1, A);
              Emu.setRegister(a64reg::X2, B);
              for (unsigned I = 0; I < 4; ++I)
                Emu.setRegister(a64reg::NFLAG + I, I & 1);
              ASSERT_EQ(Emu.run(Ops), Ops.size());
              EXPECT_FALSE(Emu.skips().any());
              EXPECT_EQ(Emu.getRegister(Destination * 8), Expected);
              if (Destination != 1)
                EXPECT_EQ(Emu.getRegister(a64reg::X1), A);
              if (Destination != 2)
                EXPECT_EQ(Emu.getRegister(a64reg::X2), B);
              for (unsigned I = 0; I < 4; ++I)
                EXPECT_EQ(Emu.getRegister(a64reg::NFLAG + I), I & 1);
            }
        }
}
