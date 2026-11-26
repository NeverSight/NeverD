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

#include <bit>
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

void expectRISCVResult(const BinaryImage &Img, Decoder &D, uint32_t Word,
                       uint64_t X, uint64_t Y, uint64_t Expected,
                       unsigned Result = 10) {
  const uint64_t Mask = Img.Arch == Arch::RISCV64 ? UINT64_MAX : UINT32_MAX;
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
  EXPECT_EQ(Emu.getRegister(integerreg::gpr(Result)), Expected & Mask);
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
          expectRISCVResult(Img, D, Word, X, Y, Expected);
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
TEST(RISCVBasicBitManipulation, CountsLanesAliasesAndWordRotates) {
  using namespace rv_fixture;
  for (Arch A : {Arch::RISCV32, Arch::RISCV64}) {
    const bool Wide = A == Arch::RISCV64;
    BinaryImage Img;
    Img.Arch = A;
    Img.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
    Img.RISCV.Zbb = true;
    Decoder D;
    ASSERT_TRUE(D.init(Img));
    for (uint64_t Input :
         {UINT64_C(0), UINT64_MAX, UINT64_C(0x010080ff807f0001),
          UINT64_C(0x8100000080000000), UINT64_C(0x8000000000000000)}) {
      const uint64_t X = Wide ? Input : uint32_t(Input);
      uint64_t Bytes = 0;
      for (unsigned I = 0; I < 64; I += 8)
        if ((X >> I) & UINT8_MAX)
          Bytes |= uint64_t(UINT8_MAX) << I;
      expectRISCVResult(Img, D, OrcB, X, 0, Bytes);
      expectRISCVResult(Img, D, Ctz, X, 0,
                        Wide ? std::countr_zero(X)
                             : std::countr_zero(uint32_t(X)));
      for (unsigned N : {0u, 1u, 31u, 32u, 63u, 64u}) {
        expectRISCVResult(Img, D, RolIntoRs2, X, N,
                          Wide ? std::rotl(X, N) : std::rotl(uint32_t(X), N),
                          11);
        expectRISCVResult(Img, D, RorFromZero, X, N, 0);
        expectRISCVResult(Img, D, RolToZero, X, N, X); // a0 survives rd=x0.
        if (Wide)
          expectRISCVResult(Img, D, RorW, X, N,
                            llvm::SignExtend64<32>(std::rotr(uint32_t(X), N)));
      }
      if (Wide) {
        expectRISCVResult(Img, D, CtzW, X, 0, std::countr_zero(uint32_t(X)));
        expectRISCVResult(Img, D, ClzW, X, 0, std::countl_zero(uint32_t(X)));
      }
    }
    // The other XLEN's encodings, nonzero rs2 on zext.h, and the reserved
    // high immediate bit on roriw must not alias an admitted instruction.
    for (uint32_t Word :
         {Wide ? Rev8_32 : Rev8_64, Wide ? ZextH32 : ZextH64, ReservedRoriW,
          (Wide ? ZextH64 : ZextH32) | (1u << 20),
          Wide ? ReservedUnary : Rori63}) {
      uint8_t Code[4];
      llvm::support::endian::write32le(Code, Word);
      DecodedInsn I;
      std::vector<LowOp> Ops;
      if (D.decodeOneForLift(Code, sizeof(Code), 0x1000, I))
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction)
            << std::hex << Word;
      EXPECT_TRUE(Ops.empty());
    }
  }
}

TEST(PowerPCReversedTransfers, WidthByteOrderAndReservedFields) {
  for (Arch A : {Arch::PPC32, Arch::PPC32LE, Arch::PPC64, Arch::PPC64LE,
                 Arch::PPC64ELFv1, Arch::PPC64LEELFv1}) {
    BinaryImage Img;
    Img.Arch = A;
    const unsigned Width = isPowerPC64(A) ? 8 : 4;
    Img.Bits = Width == 8 ? Bitness::Bits64 : Bitness::Bits32;
    auto &Data = Img.Segments.emplace_back();
    Data.VA = 0x2000;
    Data.Data = {0xa5, 0xf2, 0x83, 0x94, 0x25, 0x36, 0x47, 0x58, 0xe9, 0x5a};
    Data.Size = Data.FileSz = Data.Data.size();
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    const bool Little = archLittleEndian(A);
    const auto Order =
        Little ? llvm::endianness::little : llvm::endianness::big;
    Decoder D;
    ASSERT_TRUE(D.init(Img));
    for (auto F : ppc_fixture::ReversedTransfers) {
      for (unsigned Extra : {0u, 1u, 1u << 6}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(A) << " " << std::hex << (F.Word | Extra));
        uint8_t Code[4];
        llvm::support::endian::write32(Code, F.Word | Extra, Order);
        DecodedInsn I;
        const int Size = D.decodeOneForLift(Code, sizeof(Code), 0x1000, I);
        std::vector<LowOp> Ops;
        if (Extra || F.Bytes > Width) {
          if (Size)
            EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
          continue;
        }
        ASSERT_EQ(Size, 4);
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
        NdOpEmulator Emu(Img);
        Emu.setStrictMode(true);
        const uint64_t Value =
            Width == 8 ? UINT64_C(0xfedcba9876543210) : UINT32_C(0xfedcba98);
        Emu.setRegister(integerreg::gpr(3), Value);
        Emu.setRegister(integerreg::gpr(4), Data.VA - 1);
        Emu.setRegister(integerreg::gpr(5), 2);
        for (const LowOp &Op : Ops)
          ASSERT_TRUE(Emu.step(Op));
        uint64_t Loaded = 0;
        for (unsigned N = 0; N < Data.Data.size(); ++N) {
          uint8_t Expected = Data.Data[N];
          if (N && N <= F.Bytes) {
            const unsigned Shift = 8 * (Little ? F.Bytes - N : N - 1);
            Loaded |= uint64_t(Expected) << Shift;
            if (F.Store)
              Expected = uint8_t(Value >> Shift);
          }
          LowOp Read;
          Read.Opcode = NdOp::LOAD;
          Read.Output = NdVar::reg(integerreg::gpr(7), 1);
          Read.addInput(NdVar::dataAddress(Data.VA + N, Width));
          ASSERT_TRUE(Emu.step(Read));
          EXPECT_EQ(Emu.getRegister(integerreg::gpr(7)), Expected);
        }
        EXPECT_EQ(Emu.getRegister(integerreg::gpr(3)),
                  F.Store ? Value : Loaded);
        EXPECT_EQ(Emu.getRegister(integerreg::gpr(4)), Data.VA - 1);
      }
    }
  }
}
} // namespace
