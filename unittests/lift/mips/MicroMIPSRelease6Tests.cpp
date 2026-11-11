//===- MicroMIPSRelease6Tests.cpp - Compact R6 semantics -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#include "neverd/support/BinaryEncoding.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif

namespace neverd {
namespace {

class MicroMIPSRelease6 : public ::testing::TestWithParam<Arch> {
protected:
  BinaryImage Image;
  Decoder Decode;
  DecodedExecutionUnit Unit;
  static constexpr va_t PC = 0x10000;
  void SetUp() override {
    Image.Arch = GetParam();
    Image.Mode = InstructionMode::MicroMIPS;
    Image.Bits = Bitness::Bits32;
    Image.MIPS.emplace();
    Image.MIPS->ABI = MIPSABI::O32;
    Image.MIPS->ISARevision = 6;
    Image.MIPS->HasMicroMIPS = true;
    ASSERT_TRUE(Decode.init(Image));
  }
  unsigned encode(uint8_t *Bytes, uint32_t Word) {
    const auto Order = archLittleEndian(GetParam()) ? llvm::endianness::little
                                                    : llvm::endianness::big;
    if (Word > 0xffff) {
      llvm::support::endian::write16(Bytes, Word >> 16, Order);
      llvm::support::endian::write16(Bytes + 2, uint16_t(Word), Order);
      return 4;
    }
    llvm::support::endian::write16(Bytes, uint16_t(Word), Order);
    return 2;
  }
};

TEST_P(MicroMIPSRelease6, PublicTargetsPhysicalSpansAndLinkRoles) {
  struct Row {
    uint32_t Word;
    const char *Name;
    va_t Destination;
    NdOp Transfer;
  };
  // microMIPS32 Volume II-B, R6.05: displacement units are halfwords.
  const Row Cases[] = {{0x74a40004, "beqc", PC + 12, NdOp::COND_BR},
                       {0x94000008, "bc", PC + 20, NdOp::BRANCH},
                       {0xb7ffffff, "balc", PC + 2, NdOp::CALL},
                       {0xcc03, "bc16", PC + 8, NdOp::BRANCH},
                       {0xad7d, "bnezc16", PC - 4, NdOp::COND_BR},
                       {0x80040004, "jic", 0, NdOp::INDIR_BR},
                       {0xa0040004, "jialc", 0, NdOp::INDIR_CALL},
                       {0x47e3, "jrc16", 0, NdOp::RETURN}};
  for (auto [Word, Name, Target, Transfer] : Cases) {
    SCOPED_TRACE(Name);
    uint8_t Bytes[8]{};
    const auto Size = encode(Bytes, Word);
    encode(Bytes + Size, 0x47e3);
    DecodedInsn Physical;
    ASSERT_EQ(Decode.decodeOne(Bytes, Size + 2, PC, Physical), Size);
    EXPECT_STREQ(Physical.Raw->mnemonic, Name);
    EXPECT_TRUE(std::equal(Bytes, Bytes + Size, Physical.Raw->bytes));
    if (Target) {
      auto &Detail = Physical.Raw->detail->mips;
      ASSERT_NE(Detail.op_count, 0);
      const auto &Dest = Detail.operands[Detail.op_count - 1];
      EXPECT_EQ(Dest.type, MIPS_OP_IMM);
      EXPECT_EQ(Dest.imm, Target);
    }
    ASSERT_EQ(Decode.decodeUnitForLift(Bytes, Size + 2, PC, Unit), Size);
    EXPECT_FALSE(Unit.HasDelaySlot);
    EXPECT_FALSE(Unit.Head.MIPSForbiddenFollower.has_value());
    std::vector<LowOp> Ops;
    Decode.liftUnitToLow(Unit, Ops);
    ASSERT_FALSE(Ops.empty());
    EXPECT_EQ(Ops.back().Opcode, Transfer);
    if (Target)
      EXPECT_EQ(Ops.back().Inputs[0].Offset, Target);
    if (Transfer == NdOp::CALL)
      EXPECT_EQ(Decode.directCallTarget(Unit.Head), Target);
  }
}

TEST_P(MicroMIPSRelease6,
       SystemSelectorsAndEncodingAliasesMatchClassicEffects) {
  struct Row {
    uint32_t Micro;
    uint32_t Classic;
    const char *Name;
  };
  const Row Pairs[] = {
      {0x00002800, 0x140, "pause"},      {0x00001800, 0xc0, "ehb"},
      {0x0002477c, 0x41626000, "di"},    {0x0002577c, 0x41626020, "ei"},
      {0x004c00fc, 0x40026000, "mfc0"},  {0x004c02fc, 0x40826000, "mtc0"},
      {0x004c08f4, 0x40426001, "mfhc0"}, {0x004c0af4, 0x40c26001, "mthc0"},
      {0x004419c0, 0x7c0220fb, "rdhwr"}, {0x206461f8, 0x7c83fc25, "cache"},
      {0x606421f8, 0x7c83fc35, "pref"},  {0x00a4120f, 0x00851045, "lsa"},
      {0x00a4161f, 0x7c8512e0, "align"}};
  const auto Order = archLittleEndian(GetParam()) ? llvm::endianness::little
                                                  : llvm::endianness::big;
  auto Basic = Image;
  Basic.Mode = InstructionMode::Default;
  Basic.MIPS->HasMicroMIPS = false;
  Decoder Classic;
  ASSERT_TRUE(Classic.init(Basic));
  for (const auto &[Micro, Native, Name] : Pairs) {
    SCOPED_TRACE(Name);
    uint8_t Bytes[4]; // These POOL32 words may have an all-zero first halfword.
    llvm::support::endian::write16(Bytes, Micro >> 16, Order);
    llvm::support::endian::write16(Bytes + 2, uint16_t(Micro), Order);
    DecodedInsn Physical;
    ASSERT_EQ(Decode.decodeOne(Bytes, 4, PC, Physical), 4);
    EXPECT_STREQ(Physical.Raw->mnemonic, Name);
    if (Micro == 0x206461f8 || Micro == 0x606421f8) {
      auto &Mem = Physical.Raw->detail->mips.operands[1];
      ASSERT_EQ(Mem.type, MIPS_OP_MEM);
      EXPECT_EQ(Mem.mem.disp, -8);
    }
    EXPECT_TRUE(std::equal(Bytes, Bytes + 4, Physical.Raw->bytes));
    ASSERT_EQ(Decode.decodeUnitForLift(Bytes, 4, PC, Unit), 4);
    std::vector<LowOp> CompactOps, ClassicOps;
    Decode.liftUnitToLow(Unit, CompactOps);
    llvm::support::endian::write32(Bytes, Native, Order);
    ASSERT_EQ(Classic.decodeUnitForLift(Bytes, 4, PC, Unit), 4);
    Classic.liftUnitToLow(Unit, ClassicOps);
    EXPECT_EQ(CompactOps, ClassicOps);
  }
}

TEST_P(MicroMIPSRelease6,
       PairedReservationsRetainRegisterOrderAndFeatureChecks) {
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 8;
  Data.Data = {0x80, 1, 2, 3, 4, 5, 6, 7};
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Data));
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  ASSERT_TRUE(E.setMIPSReservationModel(16, true));
  E.setRegister(mipsreg::A0, 0x2000);
  for (uint32_t Word : {0x60441030u, 0x60449030u}) {
    uint8_t Bytes[4];
    encode(Bytes, Word);
    DecodedInsn Physical;
    ASSERT_EQ(Decode.decodeOne(Bytes, 4, PC, Physical), 4);
    EXPECT_STREQ(Physical.Raw->mnemonic, (Word & 0x8000) ? "scwp" : "llwp");
    ASSERT_EQ(Decode.decodeUnitForLift(Bytes, 4, PC, Unit), 4);
    std::vector<LowOp> Ops;
    Decode.liftUnitToLow(Unit, Ops);
    for (const auto &Op : Ops)
      ASSERT_TRUE(E.step(Op));
    if (!(Word & 0x8000)) {
      EXPECT_EQ(E.getRegister(mipsreg::V0),
                archLittleEndian(GetParam()) ? 0x03020180u : 0x04050607u);
      EXPECT_EQ(E.getRegister(mipsreg::V1),
                archLittleEndian(GetParam()) ? 0x07060504u : 0x80010203u);
    } else
      EXPECT_EQ(E.getRegister(mipsreg::V0), 1u);
  }
  for (auto Bad : {0x60441031u, 0x60441230u, 0x60445030u}) {
    uint8_t Bytes[4];
    encode(Bytes, Bad);
    if (Decode.decodeUnitForLift(Bytes, 4, PC, Unit)) {
      std::vector<LowOp> Ops;
      EXPECT_THROW(Decode.liftUnitToLow(Unit, Ops), UnliftedInstruction);
    }
  }
}

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
TEST_P(MicroMIPSRelease6, NativeHalfwordsMatchIntegerStateAndNextPC) {
  const auto Mode = static_cast<uc_mode>(
      UC_MODE_MIPS32 | (archLittleEndian(GetParam()) ? UC_MODE_LITTLE_ENDIAN
                                                     : UC_MODE_BIG_ENDIAN));
  uc_engine *Raw = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_MIPS, Mode, &Raw), UC_ERR_OK);
  std::unique_ptr<uc_engine, decltype(&uc_close)> Reference(Raw, uc_close);
  ASSERT_EQ(uc_ctl_set_cpu_model(Raw, UC_CPU_MIPS32_MIPS32R6_GENERIC),
            UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, PC, 0x2000, UC_PROT_ALL), UC_ERR_OK);
  std::vector<uint32_t> Words = {
      0x05c4,     0x00445b3c, 0x00444b3c, 0x00820b3c, 0x00a41018, 0x00a41058,
      0x00a41098, 0x00a410d8, 0x00a41118, 0x00a41158, 0x00a41198, 0x00a411d8,
      0x00a41140, 0x00a41180, 0x74a40004, 0x7ca40004, 0x74850008, 0x7c850008,
      0x80800004, 0xa0800004, 0x94000008, 0xb4000008, 0xd4a40004, 0xf4a40004,
      0xe0a40004, 0xc0a40004, 0x80040004, 0xa0040004, 0xcc03,     0x00a4120f,
      0x00a4161f, 0x4540,     0x4541,     0x4548,     0x4549,     0x4c50,
      0x4c4e,     0x6d7f};
  for (unsigned I = 0; I < 16; ++I) // Every ANDI16 mask.
    Words.push_back(0x2d40 | I);
  for (unsigned I = 0; I < 8; ++I) // Every ADDIUR2 immediate.
    Words.push_back(0x6d40 | (I << 1));
  for (unsigned I : {0u, 1u, 2u, 255u, 256u, 509u, 510u, 511u})
    Words.push_back(0x4c01 | (I << 1));
  // A taken compact branch can translate its successor before the next case.
  // Install the complete code image before running the first instruction.
  for (unsigned I = 0; I < std::size(Words); ++I) {
    uint8_t Bytes[4];
    const auto Size = encode(Bytes, Words[I]);
    ASSERT_EQ(uc_mem_write(Raw, PC + 8 * I, Bytes, Size), UC_ERR_OK);
  }
  for (unsigned I = 0; I < std::size(Words); ++I) {
    SCOPED_TRACE(::testing::Message() << std::hex << Words[I]);
    uint8_t Bytes[4];
    const auto Size = encode(Bytes, Words[I]);
    const uint32_t Address = PC + 8 * I;
    ASSERT_EQ(Decode.decodeUnitForLift(Bytes, Size, Address, Unit), Size);
    std::vector<LowOp> Ops;
    Decode.liftUnitToLow(Unit, Ops);
    for (uint32_t X : {0u, 3u, 0x80000001u, 0x7fffffffu}) {
      uint32_t A = (Words[I] == 0x80040004 || Words[I] == 0xa0040004)
                       ? uint32_t(PC + 0x301)
                       : X;
      uint32_t B = 3, RA = 0x10055, V0 = 0x5555, V1 = 0x16;
      uint32_t SP = 0x22221000;
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_SP, &SP), UC_ERR_OK);
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_A0, &A), UC_ERR_OK);
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_A1, &B), UC_ERR_OK);
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_RA, &RA), UC_ERR_OK);
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_V0, &V0), UC_ERR_OK);
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_V1, &V1), UC_ERR_OK);
      ASSERT_EQ(uc_emu_start(Raw, Address | 1, PC + 0x1000, 0, 1), UC_ERR_OK);
      uint32_t Next = Address + Size;
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setRegister(mipsreg::SP, SP);
      E.setRegister(mipsreg::A0, A);
      E.setRegister(mipsreg::A1, B);
      E.setRegister(mipsreg::V0, V0);
      E.setRegister(mipsreg::V1, V1);
      E.setRegister(mipsreg::RA, RA);
      NdOpEmulatorShadow Shadow(E);
      for (const auto &Op : Ops) {
        if (Op.Opcode == NdOp::COND_BR) {
          const auto Taken = Shadow.value(Op.Inputs[1]);
          ASSERT_TRUE(Taken.has_value());
          if (*Taken) {
            Next = Op.Inputs[0].Offset;
            break;
          }
        } else if (Op.Opcode == NdOp::BRANCH || Op.Opcode == NdOp::CALL ||
                   Op.Opcode == NdOp::INDIR_BR ||
                   Op.Opcode == NdOp::INDIR_CALL) {
          const auto Dest = Shadow.value(Op.Inputs[0]);
          ASSERT_TRUE(Dest.has_value());
          Next = *Dest & ~uint32_t(1);
          break;
        } else
          ASSERT_TRUE(E.step(Op));
      }
      uint32_t Actual = 0;
      ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_PC, &Actual), UC_ERR_OK);
      EXPECT_EQ(Next, Actual);
      ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_RA, &Actual), UC_ERR_OK);
      EXPECT_EQ(E.getRegister(mipsreg::RA), Actual);
      ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_V0, &Actual), UC_ERR_OK);
      EXPECT_EQ(E.getRegister(mipsreg::V0), Actual);
      ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_SP, &Actual), UC_ERR_OK);
      EXPECT_EQ(E.getRegister(mipsreg::SP), Actual);
    }
  }
}
#endif

TEST(MicroMIPS64Release6, PhysicalDoublewordFieldsMatchClassicLowIR) {
  // microMIPS64 Volume II-B R6.05, paired with GNU as mips64r6 encodings.
  const struct {
    uint32_t Micro, Classic;
  } Cases[] = {
      {0x58a41018, 0x0085109c}, // dmul $2,$4,$5
      {0x58a41058, 0x008510dc}, // dmuh $2,$4,$5
      {0x58a41098, 0x0085109d}, // dmulu $2,$4,$5
      {0x58a410d8, 0x008510dd}, // dmuhu $2,$4,$5
      {0x58a41118, 0x0085109e}, // ddiv $2,$4,$5
      {0x58a41158, 0x008510de}, // dmod $2,$4,$5
      {0x58a41198, 0x0085109f}, // ddivu $2,$4,$5
      {0x58a411d8, 0x008510df}, // dmodu $2,$4,$5
      {0x58445b3c, 0x00801052}, // dclz $2,$4
      {0x58444b3c, 0x00801053}, // dclo $2,$4
      {0x58820b3c, 0x7c041024}, // dbitswap $2,$4
      {0x58a4161c, 0x7c8513a4}, // dalign $2,$4,$5,6
      {0x4224fffc, 0x0486fffc}, // dahi $4,$4,0xfffc
      {0x4204fffc, 0x049efffc}, // dati $4,$4,0xfffc
      {0xf0440005, 0x74820005}, // daui $2,$4,5
      {0x584c08fc, 0x40226001}, // dmfc0 $2,$12,1
      {0x584c0afc, 0x40a26001}, // dmtc0 $2,$12,1
      {0x604471f8, 0x7c82fc37}, // lld $2,-8($4)
      {0x6044f1f8, 0x7c82fc27}, // scd $2,-8($4)
      {0x60445030, 0x7c821877}, // lldp $2,$3,($4)
      {0x6044d030, 0x7c821867}, // scdp $2,$3,($4)
      {0x58442808, 0x0004117c}, // dsll32 $2,$4,5
      {0x58447b3c, 0x7c0410a4}, // dsbh $2,$4
      {0x5844fb3c, 0x7c041164}, // dshd $2,$4
      {0x5c44fff8, 0x6482fff8}, // daddiu $2,$4,-8
      {0x58a41150, 0x0085102d}, // daddu $2,$4,$5
      {0x58a411d0, 0x0085102f}, // dsubu $2,$4,$5
      {0xdc440010, 0xdc820010}, // ld $2,16($4)
      {0xd8440010, 0xfc820010}, // sd $2,16($4)
      {0x6044eff8, 0x9c82fff8}, // lwu $2,-8($4)
      {0x78580002, 0xec580002}, // ldpc $2,.+16
  };
  const va_t Address = 0x100001000;
  for (const auto Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    const auto Order = archLittleEndian(Target) ? llvm::endianness::little
                                                : llvm::endianness::big;
    BinaryImage Image;
    Image.Arch = Target;
    Image.Bits = Bitness::Bits64;
    Image.Mode = InstructionMode::MicroMIPS;
    Image.MIPS.emplace();
    Image.MIPS->ABI = MIPSABI::N64;
    Image.MIPS->ISARevision = 6;
    Image.MIPS->HasMicroMIPS = true;
    Decoder Compact, Classic;
    ASSERT_TRUE(Compact.init(Image));
    auto Standard = Image;
    Standard.Mode = InstructionMode::Default;
    Standard.MIPS->HasMicroMIPS = false;
    ASSERT_TRUE(Classic.init(Standard));
    for (const auto &[Micro, Native] : Cases) {
      SCOPED_TRACE(::testing::Message() << std::hex << Micro);
      uint8_t Bytes[4], Reference[4];
      llvm::support::endian::write16(Bytes, Micro >> 16, Order);
      llvm::support::endian::write16(Bytes + 2, uint16_t(Micro), Order);
      llvm::support::endian::write32(Reference, Native, Order);
      DecodedInsn Physical, Expected;
      ASSERT_EQ(Compact.decodeOne(Bytes, 4, Address, Physical), 4);
      ASSERT_EQ(Classic.decodeOne(Reference, 4, Address, Expected), 4);
      EXPECT_STREQ(Physical.Raw->mnemonic, Expected.Raw->mnemonic);
      EXPECT_STREQ(Physical.Raw->op_str, Expected.Raw->op_str);
      EXPECT_TRUE(std::equal(Bytes, Bytes + 4, Physical.Raw->bytes));
      DecodedExecutionUnit Unit;
      std::vector<LowOp> Ops, ReferenceOps;
      ASSERT_EQ(Compact.decodeUnitForLift(Bytes, 4, Address, Unit), 4);
      ASSERT_NO_THROW(Compact.liftUnitToLow(Unit, Ops));
      ASSERT_EQ(Classic.decodeUnitForLift(Reference, 4, Address, Unit), 4);
      ASSERT_NO_THROW(Classic.liftUnitToLow(Unit, ReferenceOps));
      ASSERT_FALSE(Ops.empty());
      EXPECT_EQ(Ops, ReferenceOps);
    }
    // The DLSA and DSRA32 format diagrams conflict with the opcode map.
    for (uint32_t Word : {0x58a41104u, 0x58a41100u, 0x58442884u, 0x58442888u}) {
      uint8_t Bytes[4];
      llvm::support::endian::write16(Bytes, Word >> 16, Order);
      llvm::support::endian::write16(Bytes + 2, uint16_t(Word), Order);
      DecodedExecutionUnit Unit;
      if (Compact.decodeUnitForLift(Bytes, 4, Address, Unit)) {
        std::vector<LowOp> Ops;
        EXPECT_THROW(Compact.liftUnitToLow(Unit, Ops), UnliftedInstruction);
      }
    }
  }
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MicroMIPSRelease6,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace
} // namespace neverd
