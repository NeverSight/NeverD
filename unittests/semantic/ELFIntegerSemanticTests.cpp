//===- ELFIntegerSemanticTests.cpp - Scalar semantic roundtrips -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compare register and condition results of physical machine instructions with
/// the same instructions lifted into LowIR.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/IntegerRegisters.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <bit>
#include <memory>
#include <optional>
#include <unicorn/unicorn.h>
#include <vector>

using namespace neverd;
namespace {
#include "PowerPCArithmeticFixture.inc"

class ELFIntegerSemantic : public ::testing::TestWithParam<Arch> {};

TEST_P(ELFIntegerSemantic, ScalarInstructionsMatchUnicornAfterEveryStep) {
  const Arch A = GetParam();
  const bool RV = isRISCV(A), Wide = A == Arch::RISCV64 || isPowerPC64(A);
  constexpr va_t PC = 0x10000;
  BinaryImage Img;
  Img.Arch = A;
  Img.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
  Img.RISCV = {.Multiply = true, .Atomic = true, .Compressed = true};
  if (!RV) {
    Segment &Data = Img.Segments.emplace_back();
    Data.VA = 0x2000;
    Data.Size = Data.FileSz = 256;
    Data.Data.assign(Data.Size, 0x91);
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  }
  Decoder D;
  ASSERT_TRUE(D.init(Img));

  // Use distinct destinations so every result can be compared independently.
  std::vector<uint32_t> Words;
  if (RV) {
    unsigned Dst = 10;
    for (unsigned F = 0; F < 8; ++F) // MUL, MULH, MULHSU, MULHU, DIV/U, REM/U
      Words.push_back(0x02000033u | (Dst++ << 7) | F << 12 | 6 << 15 | 7 << 20);
    for (unsigned F = 0; F < 8; ++F) // ADD, SLL, SLT/U, XOR, SRL, OR, AND
      Words.push_back(0x33u | (Dst++ << 7) | F << 12 | 6 << 15 | 7 << 20);
    if (Wide) {
      Words.push_back(0x40735d3b); // sraw s10,t1,t2
      Words.push_back(0x02734dbb); // divw s11,t1,t2
    }
  } else {
    Words = {0x7d063a15, // add. r8,r6,r7
             0x7d263850, // subf r9,r6,r7
             0x7cca3a78, // xor r10,r6,r7
             0x54cb287e, // rlwinm r11,r6,5,1,31
             0x50cc6f04, // rlwimi r12,r6,13,28,2
             0x7ccd3830, // slw r13,r6,r7
             0x2d060001, // cmpwi cr2,r6,1
             0x60ce0123, // ori r14,r6,0x123
             0x3de6ffff, // addis r15,r6,-1
             31u << 26 | 6 << 21 | 255 << 12 | 144 << 1, // mtcrf 255,r6
             31u << 26 | 16 << 21 | 19 << 1,             // mfcr r16
             31u << 26 | 6 << 21 | 17 << 16 | 7 << 11 |
                 792 << 1, // sraw r17,r6,r7
             Wide ? (31u << 26 | 18 << 21 | 6 << 16 |
                     202 << 1) // addze r18,r6 observes CA
                  : (31u << 26 | 18 << 21 | 1 << 16 | 339 << 1)}; // mfxer r18
    if (Wide)
      Words.push_back(31u << 26 | 6 << 21 | 19 << 16 | 7 << 11 |
                      794 << 1); // srad r19,r6,r7
    auto XO = [&](unsigned Operation, unsigned Dest, unsigned Second) {
      Words.push_back(31u << 26 | Dest << 21 | 6 << 16 | Second << 11 |
                      Operation << 1 | 1);
    };
    XO(Wide ? 9 : 11, 20, 7);  // mulhdu. / mulhwu.
    XO(Wide ? 73 : 75, 21, 7); // mulhd. / mulhw.
    XO(10, 22, 7);             // addc.
    XO(138 + 512, 23, 7);      // addeo.
    XO(136 + 512, 24, 7);      // subfeo.
    XO(234, 25, 0);            // addme.
    XO(232, 26, 0);            // subfme.
    XO(200, 27, 0);            // subfze.
    XO(202, 28, 0);            // addze.
    // Clang-assembled GPR and CR forms use different XO fields. Compare
    // both destinations and all CR bits with independent target execution.
    Words.insert(Words.end(), {0x7cc53839, 0x4d8a8a02,   // and. / crand
                               0x7cc53879, 0x4d8a8902,   // andc. / crandc
                               0x7cc53bb9, 0x4d8a89c2,   // nand. / crnand
                               0x7cc53b79, 0x4d8a8b82,   // or. / cror
                               0x7cc53b39, 0x4d8a8b42,   // orc. / crorc
                               0x7cc538f9, 0x4d8a8842,   // nor. / crnor
                               0x7cc53a79, 0x4d8a8982,   // xor. / crxor
                               0x7cc53a39, 0x4d8a8a42}); // eqv. / creqv
    Words.insert(Words.end(), {MultiplyWord, MultiplyWordRecord});
    if (Wide)
      for (auto Form : PopulationForms)
        Words.push_back(Form.Word);
    // Read back stores through different addressing forms and widths.
    Words.insert(Words.end(), {0x3ba02080,   // li r29,0x2080
                               0x3bc00008,   // li r30,8
                               0x9cfdfff8,   // stbu r7,-8(r29)
                               0x8ffd0000,   // lbzu r31,0(r29)
                               0x7cddf36e,   // sthux r6,r29,r30
                               0x7ffdf2ee,   // lhaux r31,r29,r30
                               0x87fdfff8,   // lwzu r31,-8(r29)
                               0x7cddf12e,   // stwx r6,r29,r30
                               0x7ffdf06e}); // lwzux r31,r29,r30
    if (Wide)
      Words.insert(Words.end(), {0xf8fdfff9,   // stdu r7,-8(r29)
                                 0xebfd0001,   // ldu r31,0(r29)
                                 0xebfd0002,   // lwa r31,0(r29)
                                 0x7ffdf2ea}); // lwaux r31,r29,r30
  }
  std::vector<uint8_t> Bytes;
  for (auto W : Words)
    for (unsigned I = 0; I < 4; ++I)
      Bytes.push_back(W >> (8 * (archLittleEndian(A) ? I : 3 - I)));

  constexpr uint64_t Values[] = {0,
                                 1,
                                 UINT64_MAX,
                                 UINT64_C(0x8000000000000000),
                                 UINT64_C(0xffffffff80000000),
                                 UINT64_C(0x789abcde76543210)};
  for (uint64_t L : Values)
    for (uint64_t R : Values) {
      SCOPED_TRACE(::testing::Message() << "lhs=" << L << " rhs=" << R);
      uc_engine *Engine = nullptr;
      auto Mode = RV ? (Wide ? UC_MODE_RISCV64 : UC_MODE_RISCV32)
                     : uc_mode(UC_MODE_BIG_ENDIAN |
                               (Wide ? UC_MODE_PPC64 : UC_MODE_PPC32));
      ASSERT_EQ(uc_open(RV ? UC_ARCH_RISCV : UC_ARCH_PPC, Mode, &Engine),
                UC_ERR_OK);
      std::unique_ptr<uc_engine, decltype(&uc_close)> Owner(Engine, uc_close);
      // Use the default POWER10's full 64-bit registers with a flat fixture VA
      // map.
      if (!RV && Wide)
        ASSERT_EQ(uc_ctl_tlb_mode(Engine, UC_TLB_VIRTUAL), UC_ERR_OK);
      ASSERT_EQ(uc_mem_map(Engine, PC, 4096, UC_PROT_ALL), UC_ERR_OK);
      ASSERT_EQ(uc_mem_write(Engine, PC, Bytes.data(), Bytes.size()),
                UC_ERR_OK);
      if (!RV) {
        const Segment &Data = Img.Segments.front();
        ASSERT_EQ(uc_mem_map(Engine, Data.VA, 4096, UC_PROT_ALL), UC_ERR_OK);
        ASSERT_EQ(uc_mem_write(Engine, Data.VA, Data.Data.data(), Data.Size),
                  UC_ERR_OK);
        // Unicorn opens PowerPC in big-endian mode. MSR.LE sets the actual
        // instruction/data order, and MSR.SF selects 64-bit integer execution.
        uint64_t MSR =
            (Wide ? uint64_t(1) << 63 : 0) | uint64_t(archLittleEndian(A));
        ASSERT_EQ(uc_reg_write(Engine, UC_PPC_REG_MSR, &MSR), UC_ERR_OK);
      }
      const int First = RV ? int(UC_RISCV_REG_X0) : int(UC_PPC_REG_0);
      NdOpEmulator Emu(Img);
      Emu.setStrictMode(true);
      Emu.setRegister(integerreg::ppc::XER, 0);
      for (unsigned I = RV ? 1 : 0; I < 32; ++I) {
        uint64_t V = I == 6 ? L : I == 7 ? R : I * UINT64_C(0x1234567);
        if (!Wide)
          V = uint32_t(V);
        ASSERT_EQ(uc_reg_write(Engine, First + I, &V), UC_ERR_OK);
        Emu.setRegister(integerreg::gpr(I), V);
      }
      for (size_t I = 0; I < Words.size(); ++I) {
        SCOPED_TRACE(::testing::Message() << "instruction " << I);
        uint64_t At = PC + 4 * I;
        DecodedInsn Insn;
        ASSERT_EQ(D.decodeOneForLift(Bytes.data() + 4 * I, 4, At, Insn), 4);
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
        for (auto &Op : Ops)
          ASSERT_TRUE(Emu.step(Op)) << ndOpName(Op.Opcode);
        auto Status = uc_emu_start(Engine, At, At + 4, 0, 1);
        ASSERT_EQ(Status, UC_ERR_OK) << uc_strerror(Status);
        for (unsigned N = RV ? 1 : 0; N < 32; ++N) {
          uint64_t V = 0;
          ASSERT_EQ(uc_reg_read(Engine, First + N, &V), UC_ERR_OK);
          ASSERT_EQ(Emu.getRegister(integerreg::gpr(N)), V)
              << Insn.Raw->mnemonic << " r" << N;
        }
        for (unsigned Field = 0; !RV && Field < 8; ++Field) {
          // add. defines CR0, cmpwi defines CR2 and mtcrf defines all eight.
          if (I < 9 && Field != 0 && !(Field == 2 && I >= 6))
            continue;
          uint32_t CR = 0;
          ASSERT_EQ(uc_reg_read(Engine, UC_PPC_REG_CR0 + Field, &CR),
                    UC_ERR_OK);
          for (unsigned B = 0; B < 4; ++B)
            EXPECT_EQ(Emu.getRegister(integerreg::ppc::crBit(4 * Field + B)),
                      (CR >> (3 - B)) & 1);
        }
      }
    }
}
TEST_P(ELFIntegerSemantic, OrderedAtomicsAndReservations) {
  const Arch A = GetParam();
  const bool RV = isRISCV(A), Wide = A == Arch::RISCV64 || isPowerPC64(A);
  constexpr uint64_t Address = 0x2000;
  BinaryImage Img;
  Img.Arch = A;
  Img.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
  Img.RISCV.Atomic = true;
  Segment Data;
  Data.VA = Address;
  Data.Size = Data.FileSz = 16;
  Data.Data.assign(16, 0xff);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(Data);
  Decoder D;
  ASSERT_TRUE(D.init(Img));
  NdOpEmulator Emu(Img);
  Emu.setStrictMode(true);
  Emu.setRegister(integerreg::gpr(6), Address);
  Emu.setRegister(integerreg::gpr(7), 0x80000002);
  Emu.setRegister(integerreg::ppc::XER, 0);
  auto Run = [&](uint32_t Word) {
    uint8_t Code[4];
    for (unsigned I = 0; I < 4; ++I)
      Code[archLittleEndian(A) ? I : 3 - I] = Word >> (8 * I);
    DecodedInsn Insn;
    ASSERT_EQ(D.decodeOneForLift(Code, 4, 0x1000, Insn), 4);
    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
    for (const auto &Op : Ops)
      ASSERT_TRUE(Emu.step(Op)) << ndOpName(Op.Opcode);
  };
  auto Atomic = [](unsigned Fn, unsigned Order, unsigned Rs) {
    return Fn << 27 | Order << 25 | Rs << 20 | 6 << 15 | 2 << 12 | 10 << 7 |
           0x2f;
  };
  uint32_t Reserve =
      RV ? Atomic(2, 2, 0) : 0x7d403028; // lr.w.aq / lwarx r10,0,r6
  uint32_t Store =
      RV ? Atomic(3, 1, 7) : 0x7ce0312d; // sc.w.rl / stwcx. r7,0,r6
  Run(Reserve);
  EXPECT_EQ(Emu.getRegister(integerreg::gpr(10)),
            RV && Wide ? UINT64_MAX : UINT32_MAX);
  Run(Store);
  auto Success = RV ? integerreg::gpr(10) : integerreg::ppc::crBit(2);
  EXPECT_EQ(Emu.getRegister(Success), RV ? 0 : 1);
  Run(Store);
  EXPECT_EQ(Emu.getRegister(Success), RV ? 1 : 0);
  Run(Reserve);
  EXPECT_EQ(Emu.getRegister(integerreg::gpr(10)),
            RV && Wide ? UINT64_C(0xffffffff80000002) : UINT64_C(0x80000002));
  Run(RV ? 0x00732023 : 0x90e60000); // sw r7,0(r6) / stw r7,0(r6)
  Run(Store);
  EXPECT_EQ(Emu.getRegister(Success), RV ? 1 : 0);
  if (RV) {
    for (unsigned Fn : {0u, 1u, 4u, 8u, 12u, 16u, 20u, 24u, 28u}) {
      for (unsigned Order = 0; Order < 4; ++Order) {
        Emu.setRegister(integerreg::gpr(7), 0x80000002);
        Run(0x00732023);
        Emu.setRegister(integerreg::gpr(7), 7);
        Run(Atomic(Fn, Order, 7));
        EXPECT_EQ(Emu.getRegister(integerreg::gpr(10)),
                  Wide ? UINT64_C(0xffffffff80000002) : UINT64_C(0x80000002));
        Run(0x00032503); // lw a0,0(t1)
        uint32_t Expected = Fn == 0                           ? 0x80000009
                            : Fn == 1 || Fn == 20 || Fn == 24 ? 7
                            : Fn == 4                         ? 0x80000005
                            : Fn == 8                         ? 0x80000007
                            : Fn == 12                        ? 2
                                                              : 0x80000002;
        EXPECT_EQ(Emu.getRegister(integerreg::gpr(10)),
                  Wide ? uint64_t(int64_t(int32_t(Expected))) : Expected);
      }
    }
  }
}

TEST_P(ELFIntegerSemantic, IndirectCallUsesOldLinkAndPhysicalReturnAddress) {
  const Arch A = GetParam();
  const bool RV = isRISCV(A);
  BinaryImage Img;
  Img.Arch = A;
  Img.Bits =
      A == Arch::RISCV64 || isPowerPC64(A) ? Bitness::Bits64 : Bitness::Bits32;
  Img.RISCV.Compressed = RV;
  Decoder D;
  ASSERT_TRUE(D.init(Img));
  const auto Link = RV ? integerreg::gpr(1) : integerreg::ppc::LR;
  const unsigned Bytes = RV ? 2 : 4;
  const uint32_t Encoding = RV ? 0x9082 : 0x4e800021; // c.jalr ra / blrl
  uint8_t Code[4]{};
  for (unsigned I = 0; I < Bytes; ++I)
    Code[archLittleEndian(A) ? I : Bytes - 1 - I] = Encoding >> (8 * I);
  DecodedInsn Insn;
  ASSERT_EQ(D.decodeOneForLift(Code, Bytes, 0x1000, Insn), Bytes);
  std::vector<LowOp> Ops;
  D.liftToLow(Insn, Ops);
  ASSERT_FALSE(Ops.empty());
  ASSERT_EQ(Ops.back().Opcode, NdOp::INDIR_CALL);
  ASSERT_TRUE(Ops.back().Inputs[0].isTemp());
  NdOpEmulator Emu(Img);
  Emu.setStrictMode(true);
  Emu.setRegister(Link, 0x2003);
  for (const LowOp &Op : llvm::ArrayRef(Ops).drop_back())
    ASSERT_TRUE(Emu.step(Op)) << ndOpName(Op.Opcode);
  EXPECT_EQ(Emu.getRegister(Link), 0x1000u + Bytes);
  EXPECT_EQ(Emu.getRegister(Ops.back().Inputs[0].Offset),
            RV ? 0x2002u : 0x2000u);
}

TEST(PowerPCInteger, PopulationCountsKeepEachLaneAndRejectReservedFields) {
  for (Arch A : {Arch::PPC32, Arch::PPC32LE, Arch::PPC64, Arch::PPC64LE}) {
    BinaryImage Image;
    Image.Arch = A;
    const unsigned Bytes = isPowerPC64(A) ? 8 : 4;
    Image.Bits = Bytes == 8 ? Bitness::Bits64 : Bitness::Bits32;
    Decoder D;
    ASSERT_TRUE(D.init(Image));
    for (auto [Word, Lane] : PopulationForms)
      for (unsigned Reserved : {0U, 1U, 1U << 11}) {
        uint8_t Code[4];
        llvm::support::endian::write32(Code, Word | Reserved,
                                       archLittleEndian(A)
                                           ? llvm::endianness::little
                                           : llvm::endianness::big);
        DecodedInsn I;
        const bool Admitted = !Reserved && Lane <= Bytes;
        if (!D.decodeOneForLift(Code, sizeof(Code), 0x1000, I)) {
          EXPECT_FALSE(Admitted);
          continue;
        }
        std::vector<LowOp> Ops;
        if (!Admitted) {
          EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
          continue;
        }
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
        for (uint64_t Input :
             {UINT64_C(0), UINT64_MAX, UINT64_C(0xff00aa12cccc777f)}) {
          NdOpEmulator Emu(Image);
          Emu.setStrictMode(true);
          Emu.setRegister(integerreg::gpr(6),
                          Bytes == 8 ? Input : uint32_t(Input));
          for (const LowOp &Op : Ops)
            ASSERT_TRUE(Emu.step(Op)) << ndOpName(Op.Opcode);
          const uint64_t Mask = UINT64_MAX >> (64 - 8 * Lane);
          uint64_t Expected = 0;
          for (unsigned Off = 0; Off < Bytes; Off += Lane)
            Expected |= uint64_t(std::popcount((Input >> (8 * Off)) & Mask))
                        << (8 * Off);
          EXPECT_EQ(Emu.getRegister(integerreg::gpr(5)), Expected);
        }
      }
  }
}

TEST(PowerPCDivision, QuotientDefinedBitsAndStickyOverflow) {
  struct Encoding {
    unsigned XO;
    unsigned Bytes;
    bool Signed;
  };
  constexpr Encoding Divides[] = {
#define NEVERD_PPC_DIVIDE(Id, Value, RecordId, RecordValue, XO, Bytes, Signed, \
                          Mnemonic)                                            \
  {XO, Bytes, Signed},
#include "neverd/ir/intrinsics/PowerPC/PowerPCDivision.def"
  };
  for (Arch A : {Arch::PPC32, Arch::PPC32LE, Arch::PPC64, Arch::PPC64LE}) {
    const unsigned Width = isPowerPC64(A) ? 8 : 4;
    BinaryImage Img;
    Img.Arch = A;
    Img.Bits = Width == 8 ? Bitness::Bits64 : Bitness::Bits32;
    Decoder D;
    ASSERT_TRUE(D.init(Img));
    for (const Encoding F : Divides) {
      if (F.Bytes > Width)
        continue;
      const uint64_t Min = uint64_t(1) << (F.Bytes * 8 - 1);
      const uint64_t Mask = Min | (Min - 1);
      struct Inputs {
        uint64_t Left, Right;
        std::optional<uint64_t> Quotient;
      };
      const Inputs Cases[] = {
          {63, 9, 7},
          {0, 7, 0},
          {1, 0, std::nullopt},
          {Min, Mask, F.Signed ? std::nullopt : std::optional<uint64_t>(0)},
          {Mask - 62, 9, F.Signed ? (Mask - 6) : (Mask - 62) / 9}};
      for (unsigned Variant = 0; Variant != 4; ++Variant) {
        const bool OE = Variant & 2, Record = Variant & 1;
        uint8_t Code[4];
        // The destination aliases RA. The overflow test observes its old value.
        const uint32_t Word = 31u << 26 | 6 << 21 | 6 << 16 | 7 << 11 |
                              unsigned(OE) << 10 | F.XO << 1 | unsigned(Record);
        llvm::support::endian::write32(Code, Word,
                                       archLittleEndian(A)
                                           ? llvm::endianness::little
                                           : llvm::endianness::big);
        DecodedInsn I;
        ASSERT_EQ(D.decodeOneForLift(Code, sizeof(Code), 0x1000, I), 4);
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
        for (const auto &C : Cases) {
          SCOPED_TRACE(::testing::Message()
                       << I.Raw->mnemonic << " " << C.Left << "/" << C.Right);
          for (bool OldSO : {false, true}) {
            NdOpEmulator Emu(Img);
            Emu.setStrictMode(true);
            Emu.setRegister(integerreg::gpr(6), C.Left);
            Emu.setRegister(integerreg::gpr(7), C.Right);
            const uint64_t OldXER = uint64_t(OldSO) << 31 | uint64_t(3) << 29;
            Emu.setRegister(integerreg::ppc::XER, OldXER);
            for (unsigned I = 0; I != 4; ++I)
              Emu.setRegister(integerreg::ppc::crBit(I), 1);
            ASSERT_EQ(Emu.run(Ops), Ops.size());
            const bool Overflow = !C.Quotient.has_value();
            const uint64_t XER = uint64_t(OldSO || Overflow) << 31 |
                                 uint64_t(Overflow) << 30 | uint64_t(1) << 29;
            EXPECT_EQ(Emu.getRegister(integerreg::ppc::XER), OE ? XER : OldXER);
            EXPECT_EQ(Emu.getRegister(integerreg::gpr(6)),
                      Width == F.Bytes ? C.Quotient : std::nullopt);
            for (unsigned I = 0; I != 3; ++I) {
              std::optional<uint64_t> Bit = 1;
              if (Record) {
                if (Width != F.Bytes || !C.Quotient)
                  Bit.reset();
                else {
                  const bool Negative = (*C.Quotient & Min) != 0;
                  Bit = I == 0   ? Negative
                        : I == 1 ? (!Negative && *C.Quotient != 0)
                                 : *C.Quotient == 0;
                }
              }
              EXPECT_EQ(Emu.getRegister(integerreg::ppc::crBit(I)), Bit);
            }
            EXPECT_EQ(Emu.getRegister(integerreg::ppc::crBit(3)),
                      Record ? OldSO || (OE && Overflow) : true);
            if (Width != F.Bytes) {
              LowOp Low;
              Low.Opcode = NdOp::SUBBYTES;
              Low.Output = NdVar::reg(integerreg::gpr(8), F.Bytes);
              Low.Inputs[0] = NdVar::reg(integerreg::gpr(6), Width);
              Low.Inputs[1] = NdVar::scalar(0, 4);
              Low.NumInputs = 2;
              ASSERT_TRUE(Emu.step(Low));
              EXPECT_EQ(Emu.getRegister(integerreg::gpr(8)), C.Quotient);
            }
          }
        }
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(Profiles, ELFIntegerSemantic,
                         ::testing::Values(Arch::RISCV32, Arch::RISCV64,
                                           Arch::PPC32, Arch::PPC32LE,
                                           Arch::PPC64, Arch::PPC64LE),
                         [](const ::testing::TestParamInfo<Arch> &P) {
                           return getArchName(P.param);
                         });
} // namespace
