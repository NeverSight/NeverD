//===- ELFIntegerExtendedScalarTests.cpp - Extended integer forms --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decode assembled instructions and compare their lifted XLEN and D34
/// semantics at register, fetch-block and architecture-profile boundaries.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/IntegerRegisters.h"

#include "llvm/Support/Endian.h"

#include <cstdint>
#include <iomanip>
#include <vector>

using namespace neverd;
namespace {
#include "ELFIntegerExtendedFixture.inc"

TEST(PowerPCPrefixed, SignedD34AndFetchBoundaries) {
  for (Arch A : {Arch::PPC64, Arch::PPC64LE, Arch::PPC64ELFv1,
                 Arch::PPC64LEELFv1, Arch::PPC32, Arch::PPC32LE}) {
    BinaryImage Img;
    Img.Arch = A;
    Img.Bits = isPowerPC64(A) ? Bitness::Bits64 : Bitness::Bits32;
    const auto Endian =
        archLittleEndian(A) ? llvm::endianness::little : llvm::endianness::big;
    Decoder D;
    ASSERT_TRUE(D.init(Img));
    uint8_t Code[8];
    for (bool Relative : {false, true})
      for (unsigned RA : {0u, 3u})
        for (int64_t Disp : {-INT64_C(0x200000000), -INT64_C(0x10001),
                             INT64_C(0), INT64_C(0x1ffffffff)}) {
          // paddi r3,rA,D34,R, emitted as two target-endian words.
          llvm::support::endian::write32(Code,
                                         ppc_fixture::AddPrefix |
                                             unsigned(Relative) << 20 |
                                             ((uint64_t(Disp) >> 16) & 0x3ffff),
                                         Endian);
          llvm::support::endian::write32(Code + 4,
                                         ppc_fixture::AddR3Suffix | RA << 16 |
                                             (uint64_t(Disp) & 0xffff),
                                         Endian);
          DecodedInsn I;
          for (uint64_t PC : {0x1038, 0x103c, 0x1040}) {
            SCOPED_TRACE(::testing::Message()
                         << unsigned(A) << " " << PC << " " << Disp);
            EXPECT_EQ(D.decodeOneForLift(Code, 4, PC, I), 0);
            EXPECT_EQ(D.decodeOneForLift(Code, 7, PC, I), 0);
            const int Size = D.decodeOneForLift(Code, sizeof(Code), PC, I);
            if (!isPowerPC64(A) || PC == 0x103c) {
              EXPECT_EQ(Size, 0);
              continue;
            }
            std::vector<LowOp> Ops;
            if (Relative && RA) {
              if (Size)
                EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
              EXPECT_TRUE(Ops.empty());
              continue;
            }
            ASSERT_EQ(Size, 8);
            ASSERT_NO_THROW(D.liftToLow(I, Ops));
            ASSERT_FALSE(Ops.empty());
            if (!RA)
              EXPECT_EQ(Ops.back().Inputs[0].Provenance,
                        Relative ? ConstantAddressProvenance::Address
                                 : ConstantAddressProvenance::Scalar);
            NdOpEmulator Emu(Img);
            Emu.setStrictMode(true);
            constexpr uint64_t Input = UINT64_MAX - 100;
            Emu.setRegister(integerreg::gpr(0), 0x9999);
            Emu.setRegister(integerreg::gpr(3), Input);
            for (const auto &Op : Ops)
              ASSERT_TRUE(Emu.step(Op));
            EXPECT_EQ(Emu.getRegister(integerreg::gpr(3)), (Relative ? PC
                                                            : RA     ? Input
                                                                     : 0) +
                                                               Disp);
          }
        }
  }
}

TEST(PowerPCPrefixed, ReservedAndMismatchedPrefixFormats) {
  for (Arch A : {Arch::PPC64, Arch::PPC64LE}) {
    Decoder D;
    ASSERT_TRUE(D.init(A));
    const auto Endian =
        archLittleEndian(A) ? llvm::endianness::little : llvm::endianness::big;
    for (uint32_t Prefix : ppc_fixture::ReservedPrefixes) {
      uint8_t Code[8];
      llvm::support::endian::write32(Code, Prefix, Endian);
      llvm::support::endian::write32(Code + 4, ppc_fixture::AddR3R4One, Endian);
      DecodedInsn I;
      std::vector<LowOp> Ops;
      if (D.decodeOneForLift(Code, sizeof(Code), 0x1000, I))
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
  }
}
} // namespace
