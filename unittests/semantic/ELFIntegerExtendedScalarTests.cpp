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
#include "llvm/Support/MathExtras.h"

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

TEST(PowerPCPrefixed, MemoryWidthsAndSignedLoadsInBothByteOrders) {
  for (Arch A : {Arch::PPC64, Arch::PPC64LE}) {
    BinaryImage Img;
    Img.Arch = A;
    Img.Bits = Bitness::Bits64;
    Segment &Data = Img.Segments.emplace_back();
    Data.VA = 0x2000;
    Data.Data = {0x81, 0x92, 0xa3, 0xb4, 0xc5, 0xd6, 0xe7, 0xf8, 0x5a};
    Data.Size = Data.FileSz = Data.Data.size();
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    const bool Little = archLittleEndian(A);
    const auto Endian =
        Little ? llvm::endianness::little : llvm::endianness::big;
    Decoder D;
    ASSERT_TRUE(D.init(Img));
    for (auto Form : ppc_fixture::MemoryForms) {
      SCOPED_TRACE(::testing::Message()
                   << std::hex << Form.Suffix << " " << Little);
      uint8_t Code[8];
      llvm::support::endian::write32(Code, Form.Prefix, Endian);
      llvm::support::endian::write32(Code + 4, Form.Suffix, Endian);
      DecodedInsn I;
      ASSERT_EQ(D.decodeOneForLift(Code, sizeof(Code), 0x1000, I), 8);
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(I, Ops));
      NdOpEmulator Emu(Img);
      Emu.setStrictMode(true);
      const uint64_t Source = UINT64_C(0x1032547698badcfe);
      Emu.setRegister(integerreg::gpr(3), Source);
      Emu.setRegister(integerreg::gpr(4), Data.VA + 1);
      for (const LowOp &Op : Ops)
        ASSERT_TRUE(Emu.step(Op));
      uint64_t Loaded = 0;
      for (unsigned N = 0; N < Data.Data.size(); ++N) {
        uint8_t Byte = Data.Data[N];
        if (N < Form.Bytes) {
          const unsigned Shift = 8 * (Little ? N : Form.Bytes - 1 - N);
          Loaded |= uint64_t(Byte) << Shift;
          if (Form.Store)
            Byte = uint8_t(Source >> Shift);
        }
        LowOp Read;
        Read.Opcode = NdOp::LOAD;
        Read.Output = NdVar::reg(integerreg::gpr(5), 1);
        Read.addInput(NdVar::dataAddress(Data.VA + N, 8));
        ASSERT_TRUE(Emu.step(Read));
        EXPECT_EQ(Emu.getRegister(integerreg::gpr(5)), Byte);
      }
      if (Form.Signed)
        Loaded = llvm::SignExtend64(Loaded, Form.Bytes * 8);
      EXPECT_EQ(Emu.getRegister(integerreg::gpr(3)),
                Form.Store ? Source : Loaded);
      EXPECT_EQ(Emu.getRegister(integerreg::gpr(4)), Data.VA + 1);
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

TEST(RISCVBitManipulation, ReservedRV32EncodingsDoNotAcquireRV64Semantics) {
  BinaryImage Img;
  Img.Arch = Arch::RISCV32;
  Img.Bits = Bitness::Bits32;
  Img.RISCV = {.Zba = true, .Zbs = true};
  Decoder D;
  ASSERT_TRUE(D.init(Img));
  for (uint32_t Word : rv_fixture::ReservedRV32) {
    uint8_t Code[4];
    llvm::support::endian::write32le(Code, Word);
    DecodedInsn I;
    std::vector<LowOp> Ops;
    if (D.decodeOneForLift(Code, sizeof(Code), 0x1000, I))
      EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
}

TEST(RISCVBitManipulation, WordInputsAndBitIndicesUseTheirArchitecturalWidths) {
  using namespace rv_fixture;
  for (Arch A : {Arch::RISCV32, Arch::RISCV64}) {
    const bool Wide = A == Arch::RISCV64;
    BinaryImage Img;
    Img.Arch = A;
    Img.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
    Img.RISCV = {.Zba = true, .Zbs = true};
    Decoder D;
    ASSERT_TRUE(D.init(Img));
    const uint64_t Mask = Wide ? UINT64_MAX : UINT32_MAX;
    for (uint64_t X : {UINT64_C(0x13579bdf80000001), UINT64_MAX})
      for (uint64_t Y : {UINT64_C(0), UINT64_C(31), UINT64_C(32), UINT64_C(63),
                         UINT64_C(64), UINT64_MAX}) {
        const unsigned N = Y & (Wide ? 63 : 31);
        const uint64_t Bit = UINT64_C(1) << N;
        const uint64_t U = uint32_t(X);
        // a0 is both rs1 and rd, a1 supplies rs2. Immediate forms use the
        // largest index and register forms also receive out-of-range values.
        auto Check = [&](uint32_t Word, uint64_t Expected) {
          SCOPED_TRACE(::testing::Message()
                       << std::hex << Word << " " << X << " " << Y);
          uint8_t Code[4];
          llvm::support::endian::write32le(Code, Word);
          DecodedInsn I;
          ASSERT_EQ(D.decodeOneForLift(Code, sizeof(Code), 0x1000, I), 4);
          std::vector<LowOp> Ops;
          ASSERT_NO_THROW(D.liftToLow(I, Ops));
          NdOpEmulator Emu(Img);
          Emu.setStrictMode(true);
          Emu.setRegister(integerreg::gpr(10), X & Mask);
          Emu.setRegister(integerreg::gpr(11), Y & Mask);
          for (const auto &Op : Ops)
            ASSERT_TRUE(Emu.step(Op));
          EXPECT_EQ(Emu.getRegister(integerreg::gpr(10)), Expected & Mask);
        };
        Check(SH1ADD, 2 * X + Y);
        Check(SH2ADD, 4 * X + Y);
        Check(SH3ADD, 8 * X + Y);
        Check(SH3ADDZero, Y); // sh3add a0,zero,a1
        if (Wide) {
          Check(AddUW, U + Y);
          Check(SH1ADDUW, 2 * U + Y);
          Check(SH2ADDUW, 4 * U + Y);
          Check(SH3ADDUW, 8 * U + Y);
          for (unsigned Shift : {0u, 31u, 32u, 63u})
            Check(SLLIUW | Shift << 20, U << Shift);
        }
        Check(BCLR, X & ~Bit);
        Check(BEXT, ((X & Mask) >> N) & 1);
        Check(BINV, X ^ Bit);
        Check(BSET, X | Bit);
        Check(BSETZero, X | 1); // bset a0,a0,zero
        const unsigned Shift = Wide ? 63 : 31;
        const uint64_t High = UINT64_C(1) << Shift;
        Check(BCLRI | Shift << 20, X & ~High);
        Check(BEXTI | Shift << 20, bool(X & High));
        Check(BINVI | Shift << 20, X ^ High);
        Check(BSETI | Shift << 20, X | High);
      }
  }
}
} // namespace
