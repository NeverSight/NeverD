//===- MIPSLegacyFPTests.cpp - Pre-R6 scalar and paired floating point
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare independently encoded classic COP1 instructions with lifted effects,
/// including legacy NaNs, rounding, FCSR and faulting destination suppression.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/MIPS/MIPSScalarFPOperations.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/lift/MIPS/MicroMIPSLegacyFloat.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"

#include "neverd/emulation/arch/mips/ScalarFP.h"
#endif

namespace neverd {
namespace {
struct MicroFloatFixture {
  const char *Name;
  uint32_t Classic, Micro;
  unsigned Bytes;
};
constexpr MicroFloatFixture MicroFloatFixtures[] = {
#define NEVERD_TEST_MICRO_LEGACY_FP(Name, Classic, Micro, Bytes)               \
  {Name, Classic, Micro, Bytes},
#include "MIPSMicroLegacyFPFixtures.def"
};

class MIPSLegacyFP : public ::testing::TestWithParam<Arch> {
protected:
  BinaryImage Image;
  Decoder Decode;
  DecodedExecutionUnit Unit;
  std::vector<LowOp> Ops;
  void SetUp() override {
    Image.Arch = GetParam();
    Image.Bits = isMIPS64(GetParam()) ? Bitness::Bits64 : Bitness::Bits32;
    Image.MIPS.emplace();
    Image.MIPS->ABI = isMIPS64(GetParam()) ? MIPSABI::N64 : MIPSABI::O32;
    Image.MIPS->FPABI = isMIPS64(GetParam()) ? 1 : 6;
    ASSERT_TRUE(Decode.init(Image));
  }
  bool micro() const { return Image.Mode == InstructionMode::MicroMIPS; }
  std::array<uint8_t, 4> encoded(uint32_t Word) const {
    std::array<uint8_t, 4> Bytes;
    const auto Order = archLittleEndian(GetParam()) ? llvm::endianness::little
                                                    : llvm::endianness::big;
    if (!micro()) {
      llvm::support::endian::write32(Bytes.data(), Word, Order);
      return Bytes;
    }
    for (const auto &F : MicroFloatFixtures) {
      const uint32_t CCMask = F.Bytes ? 0 : 0x700;
      if ((Word & ~CCMask) != F.Classic)
        continue;
      const auto Physical = F.Micro | ((Word & CCMask) << 5);
      llvm::support::endian::write16(Bytes.data(), Physical >> 16, Order);
      llvm::support::endian::write16(Bytes.data() + 2, Physical, Order);
      return Bytes;
    }
    throw std::logic_error("missing independent microMIPS FP fixture");
  }
  void lift(uint32_t Word) {
    const auto Bytes = encoded(Word);
    Ops.clear();
    ASSERT_EQ(Decode.decodeUnitForLift(Bytes.data(), 4, 0x10000, Unit), 4);
    Decode.liftUnitToLow(Unit, Ops);
  }
  void compareArithmetic(bool PairedRegisters);
  void compareMemory(bool PairedRegisters);
  bool execute(NdOpEmulator &E, MIPSSystemContext &C) {
    for (const auto &Op : Ops) {
      if (!step(E, C, Op))
        return false;
    }
    return true;
  }
  bool step(NdOpEmulator &E, MIPSSystemContext &C, const LowOp &Op) {
    const auto Id = Op.Opcode == NdOp::INTRINSIC
                        ? static_cast<Intrinsic>(Op.Inputs[0].Offset)
                        : Intrinsic::None;
    return mips::isScalarFPArithmetic(Id) || mips::isScalarFPControl(Id)
               ? E.stepMIPSSystem(Op, C)
               : E.step(Op);
  }
};

class MIPSHistoricalFP : public MIPSLegacyFP {};

class MIPSFR0 : public MIPSLegacyFP {
protected:
  void SetUp() override {
    MIPSLegacyFP::SetUp();
    Image.MIPS->FPABI = 1;
    ASSERT_TRUE(Decode.init(Image));
  }
};

class MIPSMicroLegacyFP : public MIPSLegacyFP {
  void SetUp() override {
    MIPSLegacyFP::SetUp();
    Image.Mode = InstructionMode::MicroMIPS;
    Image.MIPS->HasMicroMIPS = true;
    ASSERT_TRUE(Decode.init(Image));
  }
};

class MIPSMicroFR0 : public MIPSFR0 {
  void SetUp() override {
    MIPSFR0::SetUp();
    Image.Mode = InstructionMode::MicroMIPS;
    Image.MIPS->HasMicroMIPS = true;
    ASSERT_TRUE(Decode.init(Image));
  }
};

TEST_P(MIPSMicroLegacyFP, EncodingsMatchIndependentClassicAndMicroWords) {
  for (const auto &F : MicroFloatFixtures) {
    SCOPED_TRACE(F.Name);
    EXPECT_EQ(mips::microLegacyFloat(F.Micro), F.Classic);
    const auto Fmt = F.Classic >> 21 & 31;
    if (!isMIPS64(GetParam()) && F.Classic >> 26 == 17 &&
        (Fmt == 1 || Fmt == 5)) {
      EXPECT_THROW(lift(F.Classic), UnliftedInstruction);
      continue;
    }
    ASSERT_NO_FATAL_FAILURE(lift(F.Classic));
    EXPECT_EQ(Unit.Head.Size, 4u);
    const auto Bytes = encoded(F.Classic);
    EXPECT_TRUE(std::equal(Bytes.begin(), Bytes.end(), Unit.Head.Raw->bytes));
  }
}

TEST_P(MIPSLegacyFP, RequiresExplicitFPProfileAndRejectsRelease6Arithmetic) {
  lift(0x46062080); // add.s f2,f4,f6
  ASSERT_FALSE(Ops.empty());
  Image.MIPS->FPABI.reset();
  ASSERT_TRUE(
      Decode.init(Image)); // Reused decoder must discard FP profile evidence.
  EXPECT_THROW(lift(0x46062080), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
  Image.MIPS->FPABI = isMIPS64(GetParam()) ? 1 : 6;
  ASSERT_TRUE(Decode.init(Image));
  // MADDF, MSUBF, RINT, MIN/MAX and the R6 mask compares are not legacy forms.
  for (uint32_t Word :
       {0x46062098u, 0x46062099u, 0x4600209au, 0x4606209cu, 0x46862082u}) {
    cs_insn Raw{};
    Raw.address = 0x10000;
    Raw.size = 4;
    llvm::support::endian::write32(Raw.bytes, Word,
                                   archLittleEndian(GetParam())
                                       ? llvm::endianness::little
                                       : llvm::endianness::big);
    MIPSLifter L(GetParam());
    L.setScalarFPEnabled(true);
    Ops.clear();
    EXPECT_THROW(L.lift(Raw, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
}

TEST_P(MIPSFR0, RejectsOddDoubleRegistersAndFR1OnlyFormats) {
  // Odd double inputs/destinations, long fixed point and paired-single.
  for (uint32_t Word :
       {0x46262880u, 0x46272080u, 0x462620c0u, 0x46262031u | 1u << 11,
        0xd4830000u, 0xf4830000u, 0x44641800u, 0x44e41800u, 0x46a020a0u,
        0x4c850085u, 0x4c85100du, 0x460020a5u, 0x46002088u, 0x46c62080u,
        0x460620a6u, 0x4c4620a6u}) {
    cs_insn Raw{};
    Raw.address = 0x10000;
    Raw.size = 4;
    llvm::support::endian::write32(Raw.bytes, Word,
                                   archLittleEndian(GetParam())
                                       ? llvm::endianness::little
                                       : llvm::endianness::big);
    MIPSLifter L(GetParam());
    L.setScalarFPEnabled(true, true);
    Ops.clear();
    EXPECT_THROW(L.lift(Raw, Ops), UnliftedInstruction) << std::hex << Word;
    EXPECT_TRUE(Ops.empty());
  }
}

TEST_P(MIPSFR0, WordTransfersAndOddSingleWritesAliasDoublePairs) {
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setRegister(mipsreg::w(4), 0x12345678);
  E.setRegister(mipsreg::w(5), 0xabcdef01);
  E.setRegister(mipsreg::A0, 0x87654321);
  E.setRegister(mipsreg::w(2), 0x11223344);
  E.setRegister(mipsreg::w(3), 0x55667788);
  MIPSSystemContext C;
  lift(0x44842800); // mtc1 a0,f5 leaves f4 alone.
  ASSERT_TRUE(execute(E, C));
  EXPECT_EQ(E.getRegister(mipsreg::w(4)), 0x12345678u);
  EXPECT_EQ(E.getRegister(mipsreg::w(5)), 0x87654321u);
  lift(0x46202086); // mov.d f2,f4 reads both words.
  ASSERT_TRUE(execute(E, C));
  EXPECT_EQ(E.getRegister(mipsreg::w(2)), 0x12345678u);
  EXPECT_EQ(E.getRegister(mipsreg::w(3)), 0x87654321u);
  lift(0x44621000); // mfhc1 v0,f2 reads f3.
  ASSERT_TRUE(execute(E, C));
  EXPECT_EQ(E.getRegister(mipsreg::V0), 0x87654321u);
  lift(0x44e41000); // mthc1 a0,f2 preserves f2 and writes f3.
  E.setRegister(mipsreg::A0, 0xfedcba98);
  ASSERT_TRUE(execute(E, C));
  EXPECT_EQ(E.getRegister(mipsreg::w(2)), 0x12345678u);
  EXPECT_EQ(E.getRegister(mipsreg::w(3)), 0xfedcba98u);
  lift(0x460020c6); // mov.s f3,f4 changes only the high word of the pair.
  ASSERT_TRUE(execute(E, C));
  EXPECT_EQ(E.getRegister(mipsreg::w(2)), 0x12345678u);
  EXPECT_EQ(E.getRegister(mipsreg::w(3)), 0x12345678u);
}

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
TEST_P(MIPSLegacyFP, ConditionalMovesPreserveUntakenFullRegisterAndFCSR) {
  auto FPU = emulation::createScalarFPExecutor(GetParam(), 2);
  ASSERT_TRUE(bool(FPU));
  const uint64_t Source = 0x7ff800007fc00000, Previous = 0xfedcba9876543210;
  for (unsigned CC = 0; CC < 8; ++CC)
    for (unsigned Set = 0; Set < 2; ++Set)
      for (unsigned True = 0; True < 2; ++True)
        for (unsigned Format : {0u, 16u, 17u}) {
          SCOPED_TRACE(CC);
          SCOPED_TRACE(Set);
          SCOPED_TRACE(True);
          SCOPED_TRACE(Format);
          const bool Taken = Set == True;
          const uint32_t Word =
              Format ? 0x44002091 | Format << 21 | (CC * 4 + True) << 16
                     : 0x00801001 | (CC * 4 + True) << 16;
          lift(Word); // MOVF/T f2,f4 or MOVF/T v0,a0.
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setRegister(Format ? mipsreg::w(4) : mipsreg::A0, Source);
          E.setRegister(Format ? mipsreg::w(2) : mipsreg::V0, Previous);
          MIPSSystemContext C;
          const uint32_t CSR = Set ? 1u << (CC ? CC + 24 : 23) : 0;
          C.FCSR = CSR;
          C.ScalarFPU = FPU->get();
          ASSERT_TRUE(execute(E, C));
          EXPECT_EQ(C.FCSR, CSR);
          const auto Register = Format ? mipsreg::w(2) : mipsreg::V0;
          if ((!Taken && Format) || Format == 17 ||
              (!Format && isMIPS64(GetParam())))
            EXPECT_EQ(E.getRegister(Register), Taken ? Source : Previous);
          LowOp Read;
          Read.Opcode = NdOp::COPY;
          Read.Output = NdVar::reg(mipsreg::V1, 4);
          Read.addInput(NdVar::reg(Register, 4));
          ASSERT_TRUE(E.step(Read));
          EXPECT_EQ(E.getRegister(mipsreg::V1),
                    uint32_t(Taken ? Source : Previous));
        }
  // MOVZ/N on n64 must test the complete GPR, including its high word.
  for (uint64_t Value : {uint64_t(0), uint64_t(1), uint64_t(1) << 32})
    for (unsigned Nonzero = 0; Nonzero < 2; ++Nonzero) {
      lift(0x46252092 | Nonzero); // movz/n.d f2,f4,a1
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setRegister(mipsreg::w(4), Source);
      E.setRegister(mipsreg::w(2), Previous);
      E.setRegister(mipsreg::A1, Value);
      MIPSSystemContext C;
      ASSERT_TRUE(execute(E, C));
      const bool Set = isMIPS64(GetParam()) ? Value != 0 : uint32_t(Value) != 0;
      EXPECT_EQ(E.getRegister(mipsreg::w(2)),
                Set == bool(Nonzero) ? Source : Previous);
    }
}

TEST_P(MIPSLegacyFP, BranchSnapshotsConditionAndAnnulsLikelySlot) {
  auto FPU = emulation::createScalarFPExecutor(GetParam(), 2);
  ASSERT_TRUE(bool(FPU));
  for (unsigned CC = 0; CC < 8; ++CC)
    for (unsigned Kind = 0; Kind < 4; ++Kind)
      for (unsigned Set = 0; Set < 2; ++Set) {
        const uint32_t Words[] = {0x45000003 | (CC * 4 + Kind) << 16,
                                  0x44c4c800}; // ctc1 a0,FCCR in the slot
        uint8_t Bytes[8];
        for (unsigned I = 0; I < 2; ++I)
          llvm::support::endian::write32(Bytes + I * 4, Words[I],
                                         archLittleEndian(GetParam())
                                             ? llvm::endianness::little
                                             : llvm::endianness::big);
        ASSERT_EQ(Decode.decodeUnitForLift(Bytes, 8, 0x10000, Unit), 8);
        ASSERT_TRUE(Unit.HasDelaySlot);
        Ops.clear();
        Decode.liftUnitToLow(Unit, Ops);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(mipsreg::A0, Set ? 0 : 255);
        MIPSSystemContext C;
        const uint32_t CSR = Set ? 1u << (CC ? CC + 24 : 23) : 0;
        C.FCSR = CSR;
        C.ScalarFPU = FPU->get();
        uint64_t Next = 0x10008;
        for (const auto &Op : Ops) {
          if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::COND_BR) {
            if (Op.Opcode == NdOp::BRANCH ||
                E.getRegister(Op.Inputs[1].Offset) == 1) {
              Next = Op.Inputs[0].Offset;
              break;
            }
          } else
            ASSERT_TRUE(step(E, C, Op));
        }
        const bool Taken = Set == (Kind & 1), Slot = Taken || !(Kind & 2);
        EXPECT_EQ(Next, Taken ? 0x10010u : 0x10008u);
        EXPECT_EQ(C.FCSR, Slot ? (Set ? 0u : 0xfe800000u) : CSR);
      }
}

void MIPSLegacyFP::compareMemory(bool PairedRegisters) {
  struct Form {
    uint32_t Word;
    unsigned Bytes;
    bool Store, IgnoreLowBits;
  };
  constexpr Form Forms[] = {
      {0xc4820000, 4, false, false}, {0xd4820000, 8, false, false},
      {0xe4820000, 4, true, false},  {0xf4820000, 8, true, false},
      {0x4c850080, 4, false, false}, {0x4c850081, 8, false, false},
      {0x4c851008, 4, true, false},  {0x4c851009, 8, true, false},
      {0x4c850085, 8, false, true},  {0x4c85100d, 8, true, true}};
  uc_engine *Raw = nullptr;
  const bool Wide = isMIPS64(GetParam()), Little = archLittleEndian(GetParam());
  ASSERT_EQ(uc_open(UC_ARCH_MIPS,
                    uc_mode((Wide ? UC_MODE_MIPS64 : UC_MODE_MIPS32) |
                            (Little ? 0 : UC_MODE_BIG_ENDIAN)),
                    &Raw),
            UC_ERR_OK);
  std::unique_ptr<uc_engine, decltype(&uc_close)> CPU(Raw, uc_close);
  ASSERT_EQ(uc_ctl_set_cpu_model(
                Raw, micro() ? (Wide ? int(UC_CPU_MIPS64_MICROMIPS64R2_GENERIC)
                                     : int(UC_CPU_MIPS32_MICROMIPS32R2_GENERIC))
                             : (Wide ? int(UC_CPU_MIPS64_MIPS64R2_GENERIC)
                                     : int(UC_CPU_MIPS32_74KF))),
            UC_ERR_OK);
  uint64_t Status = 0;
  ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_CP0_STATUS, &Status), UC_ERR_OK);
  Status =
      (Status & ~0x04000000ull) | (PairedRegisters ? 0x20000000 : 0x24000000);
  ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_CP0_STATUS, &Status), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x10000, 4096, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x20000, 4096, UC_PROT_ALL), UC_ERR_OK);
  Segment Data;
  Data.VA = 0x20000;
  Data.Size = Data.FileSz = 16;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  for (unsigned I = 0; I < 16; ++I)
    Data.Data.push_back(0x91 + I);
  Image.Segments.push_back(Data);
  for (unsigned I = 0; I < std::size(Forms); ++I) {
    const auto &F = Forms[I];
    if (PairedRegisters && F.IgnoreLowBits)
      continue; // LUXC1/SUXC1 require FR=1, checked by the refusal test.
    SCOPED_TRACE(I);
    lift(F.Word);
    const auto Code = encoded(F.Word);
    const uint64_t PC = 0x10000 + I * 4;
    ASSERT_EQ(uc_mem_write(Raw, PC, Code.data(), 4), UC_ERR_OK);
    for (unsigned Offset = 0; Offset < 8; ++Offset) {
      SCOPED_TRACE(Offset);
      const uint64_t Base = 0x20000 + (I < 4 ? Offset : 0),
                     Index = I < 4 ? 0 : Offset;
      const uint64_t Initial = 0xfedcba9876543210;
      ASSERT_EQ(uc_mem_write(Raw, 0x20000, Data.Data.data(), 16), UC_ERR_OK);
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_4, &Base), UC_ERR_OK);
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_5, &Index), UC_ERR_OK);
      const uint64_t Low = PairedRegisters ? uint32_t(Initial) : Initial;
      const uint64_t High = Initial >> 32;
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_F2, &Low), UC_ERR_OK);
      if (PairedRegisters)
        ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_F3, &High), UC_ERR_OK);
      const auto Status = uc_emu_start(Raw, PC | micro(), PC + 4, 0, 1);
      const bool Valid = F.IgnoreLowBits || Offset % F.Bytes == 0;
      EXPECT_EQ(Status == UC_ERR_OK, Valid);
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setRegister(mipsreg::A0, Base);
      E.setRegister(mipsreg::A1, Index);
      E.setRegister(mipsreg::w(2), Low);
      if (PairedRegisters)
        E.setRegister(mipsreg::w(3), High);
      MIPSSystemContext C;
      EXPECT_EQ(execute(E, C), Valid);
      uint64_t Expected = 0;
      ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_F2, &Expected), UC_ERR_OK);
      LowOp Read;
      Read.Opcode = NdOp::COPY;
      Read.Output = NdVar::reg(mipsreg::V1, PairedRegisters     ? 4
                                            : Valid && !F.Store ? F.Bytes
                                                                : 8);
      Read.addInput(NdVar::reg(mipsreg::w(2), Read.Output.Size));
      ASSERT_TRUE(E.step(Read));
      EXPECT_EQ(E.getRegister(mipsreg::V1),
                Read.Output.Size == 4 ? uint32_t(Expected) : Expected);
      if (PairedRegisters) {
        ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_F3, &Expected), UC_ERR_OK);
        EXPECT_EQ(E.getRegister(mipsreg::w(3)), uint32_t(Expected));
      }
      uint8_t Memory[16];
      ASSERT_EQ(uc_mem_read(Raw, 0x20000, Memory, 16), UC_ERR_OK);
      for (unsigned J = 0; J < 16; ++J) {
        LowOp ReadByte;
        ReadByte.Opcode = NdOp::LOAD;
        ReadByte.Output = NdVar::tmp(0x9000, 1);
        ReadByte.addInput(NdVar::cst(0x20000 + J, Wide ? 8 : 4));
        ASSERT_TRUE(E.step(ReadByte));
        EXPECT_EQ(E.getRegister(0x9000), Memory[J]);
      }
    }
  }
}

TEST_P(MIPSLegacyFP, IndexedMemoryMatchesOriginalAlignmentAndByteOrder) {
  compareMemory(false);
}

TEST_P(MIPSFR0, MemoryMatchesOriginalRegisterPairsAndFaults) {
  compareMemory(true);
}

TEST_P(MIPSMicroLegacyFP, MemoryMatchesPhysicalOriginalAlignmentAndByteOrder) {
  compareMemory(false);
}

TEST_P(MIPSMicroFR0, MemoryMatchesPhysicalOriginalRegisterPairsAndFaults) {
  compareMemory(true);
}

void MIPSLegacyFP::compareArithmetic(bool PairedRegisters) {
  struct Row {
    const char *Name;
    uint32_t Word;
    unsigned Bytes;
  };
  constexpr Row Rows[] = {
#define NEVERD_TEST_LEGACY_FP(Name, Word, Bytes) {Name, Word, Bytes},
#include "MIPSLegacyFPFixtures.def"
  };
  constexpr uint64_t Samples[] = {0,
                                  0x8000000080000000,
                                  0x3ff800003fc00000,
                                  0xbff00000bf800000,
                                  0x7ff000007f800000,
                                  0xfff00000ff800000,
                                  0x7ff400007fa00000,
                                  0x7ff800007fc00000,
                                  1,
                                  0x7fefffff7f7fffff,
                                  UINT64_MAX,
                                  0x123456789abcdef0};
  // The generic MIPS64r2 FPU implements PS; 74Kf does not. These arithmetic
  // words have identical semantics on MIPS32r2 with PS and MIPS64r2. The
  // surrounding LowIR still uses the selected image's GPR width and FR mode.
  auto FPU = emulation::createScalarFPExecutor(GetParam(), 2);
  ASSERT_TRUE(bool(FPU)) << llvm::toString(FPU.takeError());
  uc_engine *Raw = nullptr;
  const bool Little = archLittleEndian(GetParam());
  const bool Wide = !micro() || isMIPS64(GetParam());
  ASSERT_EQ(uc_open(UC_ARCH_MIPS,
                    uc_mode((Wide ? UC_MODE_MIPS64 : UC_MODE_MIPS32) |
                            (Little ? 0 : UC_MODE_BIG_ENDIAN)),
                    &Raw),
            UC_ERR_OK);
  std::unique_ptr<uc_engine, decltype(&uc_close)> CPU(Raw, uc_close);
  ASSERT_EQ(uc_ctl_set_cpu_model(
                Raw, micro() ? (Wide ? int(UC_CPU_MIPS64_MICROMIPS64R2_GENERIC)
                                     : int(UC_CPU_MIPS32_MICROMIPS32R2_GENERIC))
                             : int(UC_CPU_MIPS64_MIPS64R2_GENERIC)),
            UC_ERR_OK);
  uint64_t Status = 0;
  ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_CP0_STATUS, &Status), UC_ERR_OK);
  Status =
      (Status & ~0x04000000ull) | (PairedRegisters ? 0x20000000 : 0x24000000);
  ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_CP0_STATUS, &Status), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x10000, 4096, UC_PROT_ALL), UC_ERR_OK);
  unsigned Index = 0;
  unsigned QualifiedForms = 0;
  for (const auto &R : Rows) {
    // IV has the scalar arithmetic in this independent fixture, but no PS.
    const bool PairedForm =
        (R.Word >> 26) == 19 ? (R.Word & 7) == 6 : ((R.Word >> 21) & 31) == 22;
    if (Image.MIPS->HistoricalISA == 4 && PairedForm)
      continue;
    const unsigned Op = R.Word >> 26, Fmt = R.Word >> 21 & 31, Fn = R.Word & 63;
    if (PairedRegisters &&
        ((Op == 17 && (Fmt == 22 || Fmt == 21 || (Fn >= 8 && Fn <= 11) ||
                       Fn == 37 || Fn == 38)) ||
         (Op == 19 && (Fn & 7) == 6)))
      continue;
    ++QualifiedForms;
    for (unsigned CC = 0; CC < (R.Bytes ? 1u : 8u);
         CC += ((R.Word >> 21 & 31) == 22 ? 2 : 1)) {
      SCOPED_TRACE(R.Name);
      SCOPED_TRACE(CC);
      const uint32_t Word = R.Word | (CC << 8);
      ASSERT_NO_FATAL_FAILURE(lift(Word));
      EXPECT_STREQ(Unit.Head.Raw->mnemonic, R.Name);
      const auto Bytes = encoded(Word);
      const uint64_t PC = 0x10000 + 4 * Index++;
      ASSERT_EQ(uc_mem_write(Raw, PC, Bytes.data(), 4), UC_ERR_OK);
      for (unsigned Sample = 0; Sample < std::size(Samples); ++Sample)
        for (unsigned Mode = 0; Mode < 16; ++Mode) {
          SCOPED_TRACE(Sample);
          SCOPED_TRACE(Mode);
          uint32_t CSR = (Mode & 3) | (Mode & 4 ? 0x1000000 : 0) |
                         (Mode & 8 ? 0xf80 : 0) | (Sample & 1 ? 0xfe800000 : 0);
          const uint64_t Inputs[] = {
              0xdeadbeef01234567, Samples[Sample],
              Samples[(Sample + 3) % std::size(Samples)]};
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          MIPSSystemContext C;
          C.FCSR = CSR;
          C.ScalarFPU = FPU->get();
          ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_FCSR, &CSR), UC_ERR_OK);
          if (PairedRegisters)
            for (unsigned I = 0; I < 32; ++I) {
              const uint64_t Poison = 0x9e3779b9u * (I + 1);
              E.setRegister(mipsreg::w(I), uint32_t(Poison));
              ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_F0 + I, &Poison),
                        UC_ERR_OK);
            }
          for (unsigned I = 0; I < 3; ++I) {
            const uint64_t Low =
                PairedRegisters ? uint32_t(Inputs[I]) : Inputs[I];
            E.setRegister(mipsreg::w(2 + 2 * I), Low);
            ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_F2 + 2 * I, &Low),
                      UC_ERR_OK);
            if (PairedRegisters) {
              const uint64_t High = Inputs[I] >> 32;
              E.setRegister(mipsreg::w(3 + 2 * I), High);
              ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_F3 + 2 * I, &High),
                        UC_ERR_OK);
            }
          }
          const auto Result = uc_emu_start(Raw, PC | micro(), PC + 4, 0, 1);
          ASSERT_TRUE(Result == UC_ERR_OK || Result == UC_ERR_EXCEPTION);
          uint64_t Expected = 0;
          ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_F2, &Expected), UC_ERR_OK);
          ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_FCSR, &CSR), UC_ERR_OK);
          EXPECT_EQ(execute(E, C), Result == UC_ERR_OK);
          EXPECT_EQ(C.FloatingPointException, Result == UC_ERR_EXCEPTION);
          EXPECT_EQ(C.FCSR, CSR);
          if (PairedRegisters) {
            for (unsigned I = 0; I < 32; ++I) {
              ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_F0 + I, &Expected),
                        UC_ERR_OK);
              EXPECT_EQ(E.getRegister(mipsreg::w(I)), uint32_t(Expected)) << I;
            }
          } else if (Result == UC_ERR_EXCEPTION || R.Bytes == 8 || !R.Bytes)
            EXPECT_EQ(E.getRegister(mipsreg::w(2)), Expected);
          else {
            LowOp Read;
            Read.Opcode = NdOp::COPY;
            Read.Output = NdVar::reg(mipsreg::V0, 4);
            Read.addInput(NdVar::reg(mipsreg::w(2), 4));
            ASSERT_TRUE(E.step(Read));
            EXPECT_EQ(E.getRegister(mipsreg::V0), uint32_t(Expected));
          }
        }
    }
  }
  if (Image.MIPS->HistoricalISA)
    EXPECT_EQ(QualifiedForms, Image.MIPS->HistoricalISA == 4 ? 84u : 109u);
}

TEST_P(MIPSLegacyFP, ArithmeticMatchesOriginalBitsFlagsAndExceptions) {
  compareArithmetic(false);
}

TEST_P(MIPSHistoricalFP, ArithmeticMatchesOriginalBitsFlagsAndExceptions) {
  for (unsigned Level : {4u, 5u}) {
    Image.MIPS->HistoricalISA = Level;
    Image.MIPS->ISARevision = 0;
    ASSERT_TRUE(Decode.init(Image));
    compareArithmetic(false);
  }
}

TEST_P(MIPSFR0, ArithmeticMatchesOriginalRegisterPairsFlagsAndExceptions) {
  compareArithmetic(true);
}

TEST_P(MIPSMicroLegacyFP,
       ArithmeticMatchesPhysicalOriginalBitsFlagsAndExceptions) {
  compareArithmetic(false);
}

TEST_P(MIPSMicroFR0, ArithmeticMatchesPhysicalOriginalPairsFlagsAndExceptions) {
  compareArithmetic(true);
}

TEST_P(MIPSFR0, ForgedLongAndPairedArithmeticCannotExecute) {
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  auto FPU = emulation::createScalarFPExecutor(GetParam(), 2);
  ASSERT_TRUE(bool(FPU));
  for (auto Id : {Intrinsic::MIPSFPADD_PS, Intrinsic::MIPSFPCVT_D_L,
                  Intrinsic::MIPSFPROUND_L_D}) {
    LowOp Op;
    Op.Opcode = NdOp::INTRINSIC;
    Op.Output = NdVar::reg(mipsreg::V0, 8);
    Op.addInput(NdVar::cst(unsigned(Id), 4));
    const auto &F = mips::scalarFPOperation(Id);
    for (unsigned I = 0; I < F.Arity; ++I)
      Op.addInput(NdVar::cst(0, F.InputBytes));
    MIPSSystemContext C;
    C.ScalarFPU = FPU->get();
    C.FCSR = 0;
    EXPECT_FALSE(E.stepMIPSSystem(Op, C));
    EXPECT_EQ(C.FCSR, 0u);
    EXPECT_FALSE(C.FloatingPointException);
  }
}

TEST_P(MIPSLegacyFP, PairedSingleMovesRetainBitsAndSelectLanesIndependently) {
  auto FPU = emulation::createScalarFPExecutor(GetParam(), 2);
  ASSERT_TRUE(bool(FPU));
  const uint64_t A = 0x7fc1234580000000, B = 0xffa5432101234567;
  struct Form {
    uint32_t Word;
    uint64_t Result;
    unsigned Bytes;
  };
  const Form Forms[] = {
      {0x46c620ac, (A << 32) | uint32_t(B), 8},                // pll.ps
      {0x46c620ad, (A << 32) | (B >> 32), 8},                  // plu.ps
      {0x46c620ae, (A & 0xffffffff00000000) | uint32_t(B), 8}, // pul.ps
      {0x46c620af, (A & 0xffffffff00000000) | (B >> 32), 8},   // puu.ps
      {0x460620a6, (A << 32) | uint32_t(B), 8},                // cvt.ps.s
      {0x46c020a0, A >> 32, 4},                                // cvt.s.pu
      {0x46c020a8, uint32_t(A), 4},                            // cvt.s.pl
      {0x46c02086, A, 8},                                      // mov.ps
  };
  for (const auto &F : Forms)
    for (unsigned Destination : {2u, 4u, 6u}) {
      SCOPED_TRACE(F.Word);
      SCOPED_TRACE(Destination);
      ASSERT_NO_FATAL_FAILURE(lift((F.Word & ~0x7c0u) | Destination << 6));
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setRegister(mipsreg::w(4), A);
      E.setRegister(mipsreg::w(6), B);
      MIPSSystemContext C;
      C.FCSR = 0xfe80307c;
      ASSERT_TRUE(execute(E, C));
      EXPECT_EQ(C.FCSR, 0xfe80307cu);
      LowOp Read;
      Read.Opcode = NdOp::COPY;
      Read.Output = NdVar::reg(mipsreg::V0, F.Bytes);
      Read.addInput(NdVar::reg(mipsreg::w(Destination), F.Bytes));
      ASSERT_TRUE(E.step(Read));
      EXPECT_EQ(E.getRegister(mipsreg::V0), F.Result);
    }
  for (unsigned CC = 0; CC < 8; CC += 2)
    for (unsigned State = 0; State < 4; ++State)
      for (unsigned True = 0; True < 2; ++True) {
        ASSERT_NO_FATAL_FAILURE(
            lift(0x46c02091 | (CC * 4 + True) << 16)); // movf/t.ps
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(mipsreg::w(4), A);
        E.setRegister(mipsreg::w(2), B);
        MIPSSystemContext C;
        C.ScalarFPU = FPU->get();
        uint32_t CSR = 0x1004;
        for (unsigned I = 0; I < 2; ++I)
          if (State & (1u << I))
            CSR |= 1u << (CC + I ? CC + I + 24 : 23);
        C.FCSR = CSR;
        ASSERT_TRUE(execute(E, C));
        uint64_t Expected = 0;
        for (unsigned I = 0; I < 2; ++I) {
          const auto V = bool(State & (1u << I)) == bool(True) ? A : B;
          Expected |= uint64_t(uint32_t(V >> (32 * I))) << (32 * I);
        }
        EXPECT_EQ(E.getRegister(mipsreg::w(2)), Expected);
        EXPECT_EQ(C.FCSR, CSR);
      }
  for (unsigned Offset = 0; Offset < 16; ++Offset) {
    ASSERT_NO_FATAL_FAILURE(lift(0x4c86209e)); // alnv.ps f2,f4,f6,a0
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setRegister(mipsreg::A0, Offset);
    E.setRegister(mipsreg::w(4), A);
    E.setRegister(mipsreg::w(6), B);
    MIPSSystemContext C;
    ASSERT_TRUE(execute(E, C));
    if (Offset & 3)
      EXPECT_FALSE(E.getRegister(mipsreg::w(2)));
    else {
      const uint64_t Merged = archLittleEndian(GetParam())
                                  ? (B << 32) | (A >> 32)
                                  : (A << 32) | (B >> 32);
      EXPECT_EQ(E.getRegister(mipsreg::w(2)), Offset & 4 ? Merged : A);
    }
  }
}

TEST_P(MIPSLegacyFP, LegacyNaNAndTrapCannotUseRelease6Executor) {
  auto Old = emulation::createScalarFPExecutor(GetParam(), 2);
  auto R6 = emulation::createScalarFPExecutor(GetParam(), 6);
  ASSERT_TRUE(bool(Old));
  ASSERT_TRUE(bool(R6));
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  MIPSSystemContext C;
  C.FCSR = 0;
  C.ScalarFPU = R6->get();
  E.setRegister(mipsreg::w(4), 0x7fc00000); // Legacy signaling NaN.
  E.setRegister(mipsreg::w(2), 0x1234);
  lift(0x46002085); // abs.s f2,f4
  EXPECT_FALSE(execute(E, C));
  EXPECT_EQ(C.FCSR, 0u);
  EXPECT_EQ(E.getRegister(mipsreg::w(2)), 0x1234u);
  C.ScalarFPU = Old->get();
  ASSERT_TRUE(execute(E, C));
  EXPECT_EQ(*C.FCSR & 0x10040u, 0x10040u); // Invalid Cause and sticky flag.
  C.FCSR = 0x800;                          // Enable Invalid.
  E.setRegister(mipsreg::w(2), 0x1234);
  EXPECT_FALSE(execute(E, C));
  EXPECT_TRUE(C.FloatingPointException);
  EXPECT_EQ(E.getRegister(mipsreg::w(2)), 0x1234u);
  EXPECT_EQ(*C.FCSR & 0x10000u, 0x10000u);
  C.FloatingPointException = false;
  lift(0x4442f800); // cfc1 v0,FCSR observes the committed fault state.
  ASSERT_TRUE(execute(E, C));
  EXPECT_EQ(uint32_t(*E.getRegister(mipsreg::V0)), *C.FCSR);
}
#endif
#ifdef NEVERD_MIPS_UNICORN_REFERENCE
INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPSHistoricalFP,
                         ::testing::Values(Arch::MIPS64, Arch::MIPS64EL));
#endif
INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPSFR0,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));
INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPSMicroFR0,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));
INSTANTIATE_TEST_SUITE_P(EndianAndWidth, MIPSMicroLegacyFP,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));
INSTANTIATE_TEST_SUITE_P(EndianAndWidth, MIPSLegacyFP,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));
} // namespace
} // namespace neverd
