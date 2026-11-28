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
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/IntegerRegisters.h"
#include "neverd/lift/PowerPC/PowerPCVMXRegisters.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <bit>
#include <cstdint>
#include <iomanip>
#include <vector>

using namespace neverd;
namespace {
#include "../lift/fixtures/CycleVMX.inc"
#include "../lift/fixtures/RISCVConditionalZero.inc"
#include "../lift/fixtures/RISCVPack.inc"
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
  EXPECT_EQ(Emu.getRegister(integerreg::gpr(0)).value_or(0), 0u);
}

TEST(RISCVPacking, OperandOrderAliasesAndExtensionSelection) {
  using namespace pack_fixture;
  constexpr uint64_t X = UINT64_C(0xaabbccdd12349955);
  constexpr uint64_t Y = UINT64_C(0x11223344dead80ab);
  for (Arch A : {Arch::RISCV32, Arch::RISCV64}) {
    const bool Wide = isRISCV64(A);
    const uint64_t Packed = Wide ? UINT64_C(0xdead80ab12349955) : 0x80ab9955;
    for (bool Zbkb : {false, true})
      for (bool Zbb : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(A) << " Zbkb=" << Zbkb << " Zbb=" << Zbb);
        BinaryImage Img;
        Img.Arch = A;
        Img.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
        Img.RISCV = {.Zbb = Zbb, .Zbkb = Zbkb};
        Decoder D;
        ASSERT_TRUE(D.init(Img));
        const auto Check = [&](uint32_t Word, bool Enabled, uint64_t Value,
                               unsigned Dst = 10) {
          if (Enabled) {
            expectRISCVResult(Img, D, Word, X, Y, Value, Dst);
            return;
          }
          uint8_t Code[4];
          llvm::support::endian::write32le(Code, Word);
          DecodedInsn I;
          if (D.decodeOneForLift(Code, sizeof(Code), 0x1000, I)) {
            std::vector<LowOp> Ops;
            EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
            EXPECT_TRUE(Ops.empty());
          }
        };
        Check(Pack, Zbkb, Packed);
        Check(PackIntoHigh, Zbkb, Packed, 11);
        Check(PackDiscard, Zbkb, X);
        Check(PackH, Zbkb, 0xab55);
        Check(PackHZeroLow, Zbkb, 0xab00);
        Check(PackW, Wide && Zbkb, UINT64_C(0xffffffff80ab9955));
        Check(PackZeroHigh, Zbkb || (!Wide && Zbb),
              Wide ? uint32_t(X) : uint16_t(X));
        Check(PackWZeroHigh, Wide && (Zbkb || Zbb), uint16_t(X));
        Check(Brev8, false, 0);
      }
  }
}

TEST(RISCVCounter, SamplesOncePerReadAcrossRV32Rollover) {
  for (Arch A :
       {Arch::RISCV32, Arch::RISCV32D, Arch::RISCV64, Arch::RISCV64D}) {
    const bool Wide = isRISCV64(A);
    BinaryImage Img;
    Img.Arch = A;
    Img.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
    Img.RISCV.Zicntr = true;
    Decoder D;
    ASSERT_TRUE(D.init(Img));
    for (const auto &C : cycle_fixture::Counters) {
      NdOpEmulator Emu(Img);
      const uint64_t Samples[] = {0x12fffffffe, 0x12ffffffff, 0x1300000000,
                                  0x1300000001, 0x1300000002, 0x1300000003};
      size_t Next = 0;
      for (const auto &Source : cycle_fixture::Counters) {
        Emu.setRISCVCounterReader(
            Source.Kind, [&, Kind = Source.Kind]() -> std::optional<uint64_t> {
              EXPECT_EQ(Kind, C.Kind);
              if (Next == std::size(Samples))
                return std::nullopt;
              return Samples[Next++];
            });
      }
      for (size_t I = 0; I <= std::size(Samples); ++I) {
        const bool High = !Wide && I % 3 != 1;
        const bool Discard = I == std::size(Samples) || (Wide && I == 3);
        const uint32_t Word = Discard ? C.Discard : High ? C.High : C.Low;
        uint8_t Code[4];
        llvm::support::endian::write32le(Code, Word);
        DecodedInsn Insn;
        ASSERT_EQ(D.decodeOneForLift(Code, sizeof(Code), 0x1000, Insn), 4);
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
        ASSERT_EQ(Ops.size(), 1u);
        LowOp Malformed = Ops.front();
        Malformed.Inputs[0].Size = 1;
        EXPECT_FALSE(Emu.step(Malformed));
        Malformed = Ops.front();
        Malformed.MemoryOrdering = NdMemoryOrdering::Acquire;
        EXPECT_FALSE(Emu.step(Malformed));
        Malformed = Ops.front();
        Malformed.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        EXPECT_FALSE(Emu.step(Malformed));
        Malformed = Ops.front();
        Malformed.Inputs[0].Offset += UINT64_C(1) << 16;
        EXPECT_FALSE(Emu.step(Malformed));
        NdOpEmulator Unavailable(Img);
        Unavailable.setRegister(integerreg::gpr(10), 7);
        Unavailable.setRegister(integerreg::gpr(11), 9);
        for (bool Strict : {false, true}) {
          Unavailable.setStrictMode(Strict);
          EXPECT_FALSE(Unavailable.step(Ops.front()));
        }
        EXPECT_EQ(Unavailable.getRegister(integerreg::gpr(10)), 7u);
        EXPECT_EQ(Unavailable.getRegister(integerreg::gpr(11)), 9u);
        unsigned CycleReads = 0;
        Unavailable.setRISCVCycleReader([&] {
          ++CycleReads;
          return 1u;
        });
        EXPECT_EQ(Unavailable.step(Ops.front()),
                  C.Kind == riscv::Counter::Cycle);
        EXPECT_EQ(CycleReads, unsigned(C.Kind == riscv::Counter::Cycle));
        EXPECT_EQ(Emu.step(Ops.front()), I < std::size(Samples));
        EXPECT_EQ(Next, std::min(I + 1, std::size(Samples)));
        if (Discard)
          EXPECT_EQ(Emu.getRegister(integerreg::gpr(0)).value_or(0), 0u);
        else
          EXPECT_EQ(Emu.getRegister(integerreg::gpr(High ? 11 : 10)),
                    Wide   ? Samples[I]
                    : High ? Samples[I] >> 32
                           : uint32_t(Samples[I]));
      }
    }
  }
}

TEST(RISCVCounter, FloatingStatusAndCountersHaveIndependentState) {
  for (Arch A : {Arch::RISCV32D, Arch::RISCV64D}) {
    BinaryImage Img;
    Img.Arch = A;
    Img.Bits = isRISCV32(A) ? Bitness::Bits32 : Bitness::Bits64;
    Img.RISCV.SingleFloat = Img.RISCV.DoubleFloat = Img.RISCV.Zicntr = true;
    Decoder D;
    ASSERT_TRUE(D.init(Img));
    for (const auto &C : cycle_fixture::Counters) {
      unsigned Reads = 0;
      NdOpEmulator Emu(Img);
      Emu.setStrictMode(true);
      Emu.setRISCVFCSR(0x43);
      Emu.setRISCVCounterReader(C.Kind, [&] { return ++Reads; });
      for (uint32_t Word :
           {cycle_fixture::ReadFCSR, C.Low, cycle_fixture::ReadFCSR}) {
        uint8_t Code[4];
        llvm::support::endian::write32le(Code, Word);
        DecodedInsn Insn;
        ASSERT_EQ(D.decodeOneForLift(Code, sizeof(Code), 0x1000, Insn), 4);
        std::vector<LowOp> Ops;
        D.liftToLow(Insn, Ops);
        ASSERT_EQ(Ops.size(), 1u);
        ASSERT_TRUE(Emu.step(Ops.front()));
      }
      EXPECT_EQ(Reads, 1u);
      EXPECT_EQ(Emu.getRegister(integerreg::gpr(10)), 1u);
      EXPECT_EQ(Emu.getRegister(integerreg::gpr(12)), 0x43u);
      EXPECT_EQ(Emu.getRISCVFCSR(), 0x43u);
    }
  }
}

TEST(PowerPCVMX, LanesWrapAndOverlappingOperandsReadTheirOldValues) {
  for (Arch A : {Arch::PPC32, Arch::PPC32LE, Arch::PPC64, Arch::PPC64LE,
                 Arch::PPC64ELFv1, Arch::PPC64LEELFv1}) {
    BinaryImage Img;
    Img.Arch = A;
    Img.Bits = isPowerPC64(A) ? Bitness::Bits64 : Bitness::Bits32;
    const auto Order =
        archLittleEndian(A) ? llvm::endianness::little : llvm::endianness::big;
    Decoder D;
    ASSERT_TRUE(D.init(Img));
    const auto &TRI = getTargetRegInfo(A);
    EXPECT_EQ(TRI.FPReturnReg, ppc::vr(2));
    for (unsigned N = 0; N < ppc::VectorCount; ++N) {
      EXPECT_TRUE(TRI.isVectorReg(ppc::vr(N)));
      EXPECT_EQ(TRI.isFPArgReg(ppc::vr(N), BinaryFormat::ELF),
                N >= 2 && N <= 13);
      EXPECT_EQ(TRI.isCallPreserved(ppc::vr(N), ppc::VectorBytes), N >= 20);
    }
    for (const auto &Form : vmx_fixture::Operations) {
      SCOPED_TRACE(::testing::Message()
                   << unsigned(A) << " " << std::hex << Form.Word);
      uint8_t Code[4];
      llvm::support::endian::write32(Code, Form.Word, Order);
      DecodedInsn I;
      ASSERT_EQ(D.decodeOneForLift(Code, sizeof(Code), 0x1000, I), 4);
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(I, Ops));
      ASSERT_EQ(Ops.size(), 1u);
      NdOpEmulator Emu(Img);
      Emu.setStrictMode(true);
      std::vector<uint8_t> Left(ppc::VectorBytes), Right(ppc::VectorBytes),
          Expected(ppc::VectorBytes);
      for (unsigned N = 0; N < Left.size(); ++N) {
        Left[N] = 255 - 7 * N;
        Right[N] = Form.A == Form.B ? Left[N] : 1 + 19 * N;
      }
      int Carry = 0;
      for (unsigned N = 0; N < Left.size(); ++N) {
        if (N % Form.LaneBytes == 0)
          Carry = 0;
        int Result = Left[N] + Carry + (Form.Subtract ? -Right[N] : Right[N]);
        Expected[N] = static_cast<uint8_t>(Result);
        Carry = Result < 0 ? -1 : Result >> 8;
      }
      Emu.setRegisterBytes(ppc::vr(Form.B), Right);
      Emu.setRegisterBytes(ppc::vr(Form.A), llvm::ArrayRef(Left).drop_back());
      const auto Before = Emu.getRegisterBytes(ppc::vr(Form.Dst));
      EXPECT_FALSE(Emu.step(Ops.front()));
      EXPECT_EQ(Emu.getRegisterBytes(ppc::vr(Form.Dst)), Before);
      Emu.setRegisterBytes(ppc::vr(Form.A), Left);
      Emu.setRegisterBytes(ppc::vr(Form.B), Right);
      const auto Original = Emu.getRegisterBytes(ppc::vr(Form.Dst));
      LowOp Malformed = Ops.front();
      Malformed.Inputs[0].Size = 1;
      EXPECT_FALSE(Emu.step(Malformed));
      Malformed = Ops.front();
      Malformed.Inputs[0].Offset += UINT64_C(1) << 16;
      EXPECT_FALSE(Emu.step(Malformed));
      Malformed = Ops.front();
      Malformed.MemoryOrdering = NdMemoryOrdering::Acquire;
      EXPECT_FALSE(Emu.step(Malformed));
      EXPECT_EQ(Emu.getRegisterBytes(ppc::vr(Form.Dst)), Original);
      Emu.setRegister(integerreg::ppc::XER, 0x80000000);
      Emu.setRegister(integerreg::ppc::crBit(0), 1);
      ASSERT_TRUE(Emu.step(Ops.front()));
      EXPECT_EQ(Emu.getRegisterBytes(ppc::vr(Form.Dst)), Expected);
      EXPECT_EQ(Emu.getRegister(integerreg::ppc::XER), 0x80000000u);
      EXPECT_EQ(Emu.getRegister(integerreg::ppc::crBit(0)), 1u);
    }
    for (uint32_t Word : vmx_fixture::Other) {
      uint8_t Code[4];
      llvm::support::endian::write32(Code, Word, Order);
      DecodedInsn I;
      if (D.decodeOneForLift(Code, sizeof(Code), 0x1000, I)) {
        std::vector<LowOp> Ops;
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
    }
  }
}

TEST(RISCVConditionalZero, FullWidthConditionsOverlapsAndZeroRegister) {
  using namespace zicond_fixture;
  for (Arch A : {Arch::RISCV32, Arch::RISCV64}) {
    BinaryImage Img;
    Img.Arch = A;
    Img.Bits = A == Arch::RISCV64 ? Bitness::Bits64 : Bitness::Bits32;
    Img.RISCV.Zicond = true;
    Decoder D;
    ASSERT_TRUE(D.init(Img));
    const uint64_t Mask = A == Arch::RISCV64 ? UINT64_MAX : UINT32_MAX;
    const uint64_t X = UINT64_C(0xfedcba9880000001);
    for (uint64_t Y : {UINT64_C(0), UINT64_C(1), UINT64_C(0x80000000),
                       UINT64_C(0x100000000), UINT64_C(1) << 63, UINT64_MAX}) {
      const uint64_t IfNonzero = (Y & Mask) ? X : 0;
      const uint64_t IfZero = (Y & Mask) ? 0 : X;
      expectRISCVResult(Img, D, Eqz, X, Y, IfNonzero);
      expectRISCVResult(Img, D, Nez, X, Y, IfZero);
      expectRISCVResult(Img, D, EqzIntoCond, X, Y, IfNonzero, 11);
      expectRISCVResult(Img, D, NezIntoCond, X, Y, IfZero, 11);
      expectRISCVResult(Img, D, EqzZeroCond, X, Y, 0);
      expectRISCVResult(Img, D, NezZeroCond, X, Y, X);
      expectRISCVResult(Img, D, EqzZeroData, X, Y, 0);
      expectRISCVResult(Img, D, NezDiscard, X, Y, X);
    }
    for (uint32_t Word : NonZicond) {
      uint8_t Code[4];
      llvm::support::endian::write32le(Code, Word);
      DecodedInsn I;
      if (D.decodeOneForLift(Code, sizeof(Code), 0x1000, I)) {
        std::vector<LowOp> Ops;
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
    }
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
      const unsigned Bits = Wide ? 64 : 32;
      uint64_t Reverse = 0;
      for (unsigned I = 0; I < Bits; I += 8)
        Reverse = (Reverse << 8) | ((X >> I) & UINT8_MAX);
      expectRISCVResult(Img, D, Wide ? Rev8_64 : Rev8_32, X, 0, Reverse);
      expectRISCVResult(Img, D, Wide ? ZextH64 : ZextH32, X, 0, uint16_t(X));
      expectRISCVResult(Img, D, SextH, X, 0, llvm::SignExtend64<16>(X));
      expectRISCVResult(Img, D, SextB, X, 0, llvm::SignExtend64<8>(X));
      expectRISCVResult(Img, D, Clz, X, 0,
                        Wide ? std::countl_zero(X)
                             : std::countl_zero(uint32_t(X)));
      expectRISCVResult(Img, D, Cpop, X, 0, std::popcount(X));
      expectRISCVResult(Img, D, Wide ? Rori63 : Rori31, X, 0,
                        Wide ? std::rotl(X, 1) : std::rotl(uint32_t(X), 1));
      for (uint64_t Y : {UINT64_C(0), X, UINT64_C(1) << (Bits - 1)}) {
        const bool SignedLess =
            llvm::SignExtend64(X, Bits) < llvm::SignExtend64(Y, Bits);
        expectRISCVResult(Img, D, AndN, X, Y, X & ~Y);
        expectRISCVResult(Img, D, OrN, X, Y, X | ~Y);
        expectRISCVResult(Img, D, Xnor, X, Y, ~(X ^ Y));
        expectRISCVResult(Img, D, Min, X, Y, SignedLess ? X : Y);
        expectRISCVResult(Img, D, Max, X, Y, SignedLess ? Y : X);
        expectRISCVResult(Img, D, MinU, X, Y, X < Y ? X : Y);
        expectRISCVResult(Img, D, MaxU, X, Y, X < Y ? Y : X);
      }
      expectRISCVResult(Img, D, OrcB, X, 0, Bytes);
      expectRISCVResult(Img, D, Ctz, X, 0,
                        Wide ? std::countr_zero(X)
                             : std::countr_zero(uint32_t(X)));
      for (unsigned N : {0u, 1u, 31u, 32u, 63u, 64u}) {
        expectRISCVResult(Img, D, Ror, X, N,
                          Wide ? std::rotr(X, N) : std::rotr(uint32_t(X), N));
        expectRISCVResult(Img, D, RolIntoRs2, X, N,
                          Wide ? std::rotl(X, N) : std::rotl(uint32_t(X), N),
                          11);
        expectRISCVResult(Img, D, RorFromZero, X, N, 0);
        expectRISCVResult(Img, D, RolToZero, X, N, X); // a0 survives rd=x0.
        if (Wide) {
          expectRISCVResult(Img, D, RolW, X, N,
                            llvm::SignExtend64<32>(std::rotl(uint32_t(X), N)));
          expectRISCVResult(Img, D, RorW, X, N,
                            llvm::SignExtend64<32>(std::rotr(uint32_t(X), N)));
        }
      }
      if (Wide) {
        expectRISCVResult(Img, D, RoriW31, X, 0,
                          llvm::SignExtend64<32>(std::rotl(uint32_t(X), 1)));
        expectRISCVResult(Img, D, CpopW, X, 0, std::popcount(uint32_t(X)));
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
    Data.VA = 0x1fff; // The transferred value starts at the next, aligned byte.
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

TEST(PowerPCTrailingZeros, WidthAndCR0WithAndWithoutRecording) {
  using namespace integerreg::ppc;
  for (Arch A : {Arch::PPC32, Arch::PPC32LE, Arch::PPC64, Arch::PPC64LE}) {
    BinaryImage Img;
    Img.Arch = A;
    Img.Bits = isPowerPC64(A) ? Bitness::Bits64 : Bitness::Bits32;
    const auto Endian =
        archLittleEndian(A) ? llvm::endianness::little : llvm::endianness::big;
    Decoder D;
    ASSERT_TRUE(D.init(Img));
    for (bool Double : {false, true})
      for (unsigned Extra : {0u, 1u, 1u << 11}) {
        const uint32_t Word =
            Double ? ppc_fixture::CnttzdR3R4 : ppc_fixture::CnttzwR3R4;
        SCOPED_TRACE(::testing::Message()
                     << unsigned(A) << " " << std::hex << (Word | Extra));
        uint8_t Code[4];
        llvm::support::endian::write32(Code, Word | Extra, Endian);
        DecodedInsn I;
        const int Size = D.decodeOneForLift(Code, sizeof(Code), 0x1000, I);
        std::vector<LowOp> Ops;
        if ((Double && !isPowerPC64(A)) || Extra > 1) {
          if (Size)
            EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
          continue;
        }
        ASSERT_EQ(Size, 4);
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
        for (uint64_t X : {UINT64_C(0), UINT64_C(1), UINT64_C(0x80000000),
                           UINT64_C(0x8000000000000000)})
          for (unsigned SO : {0u, 1u}) {
            NdOpEmulator Emu(Img);
            Emu.setStrictMode(true);
            const auto Source = isPowerPC64(A) ? X : uint32_t(X);
            const uint64_t Count =
                Double ? std::countr_zero(X) : std::countr_zero(uint32_t(X));
            Emu.setRegister(integerreg::gpr(4), Source);
            Emu.setRegister(XER, uint64_t(SO) << XERSO);
            for (unsigned Bit = 0; Bit < 32; ++Bit)
              Emu.setRegister(crBit(Bit), Bit % 2);
            for (const auto &Op : Ops)
              ASSERT_TRUE(Emu.step(Op));
            EXPECT_EQ(Emu.getRegister(integerreg::gpr(3)), Count);
            EXPECT_EQ(Emu.getRegister(integerreg::gpr(4)), Source);
            EXPECT_EQ(Emu.getRegister(XER), uint64_t(SO) << XERSO);
            const unsigned Flags[] = {0, Count != 0, Count == 0, SO};
            for (unsigned Bit = 0; Bit < 32; ++Bit)
              EXPECT_EQ(Emu.getRegister(crBit(Bit)),
                        Extra && Bit < 4 ? Flags[Bit] : Bit % 2);
          }
      }
  }
}
} // namespace
