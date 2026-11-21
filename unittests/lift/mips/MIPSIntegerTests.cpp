//===- MIPSIntegerTests.cpp - MIPS integer semantic regressions ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Execute lifted integer operations and compare them with the pinned Unicorn
/// interpreter, including MIPS64 word-result extension and HI/LO splitting.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/IntegerCallArguments.h"
#include "neverd/ir/SourceParameterPlacement.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

using namespace neverd;

namespace {

std::vector<uint8_t> encode(llvm::ArrayRef<uint32_t> Words, Arch Target) {
  std::vector<uint8_t> Result;
  for (uint32_t Word : Words)
    for (unsigned I = 0; I != 4; ++I)
      Result.push_back(
          uint8_t(Word >> (8 * (archLittleEndian(Target) ? I : 3 - I))));
  return Result;
}

TEST(MIPSPhysicalDecode, SingleInstructionPathsMatchTheStreamingIterator) {
  using Method = int (Decoder::*)(const uint8_t *, size_t, va_t, DecodedInsn &);
  for (Arch A : {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (auto Mode : {InstructionMode::MIPSClassic, InstructionMode::MIPS16,
                      InstructionMode::MicroMIPS}) {
      SCOPED_TRACE(::testing::Message()
                   << unsigned(A) << "/" << unsigned(Mode));
      Decoder D;
      if (Mode == InstructionMode::MIPS16 && isMIPS64(A)) {
        EXPECT_FALSE(D.init(A, Mode));
        continue;
      }
      const bool Little = archLittleEndian(A);
      const auto CsMode = static_cast<cs_mode>(
          (isMIPS64(A) ? CS_MODE_MIPS64R2 : CS_MODE_MIPS32R2) |
          (Little ? CS_MODE_LITTLE_ENDIAN : CS_MODE_BIG_ENDIAN) |
          (Mode == InstructionMode::MIPS16      ? CS_MODE_MIPS16
           : Mode == InstructionMode::MicroMIPS ? CS_MODE_MICRO
                                                : 0));
      struct Reference {
        csh Handle{};
        cs_insn *Insn = nullptr;
        ~Reference() {
          if (Insn)
            cs_free(Insn, 1);
          if (Handle)
            cs_close(&Handle);
        }
      } Ref;
      ASSERT_EQ(cs_open(CS_ARCH_MIPS, CsMode, &Ref.Handle), CS_ERR_OK);
      ASSERT_EQ(cs_option(Ref.Handle, CS_OPT_DETAIL, CS_OPT_ON), CS_ERR_OK);
      Ref.Insn = cs_malloc(Ref.Handle);
      ASSERT_NE(Ref.Insn, nullptr);
      ASSERT_TRUE(D.init(A, Mode));
      const uint16_t Half = Mode == InstructionMode::MIPS16 ? 0x6a81 : 0x0c44;
      auto Bytes = encode({0x8c820004u}, A); // LW v0,4(a0).
      if (Mode != InstructionMode::MIPSClassic)
        Bytes = {uint8_t(Half >> (Little ? 0 : 8)),
                 uint8_t(Half >> (Little ? 8 : 0))};
      for (bool Text : {false, true}) {
        D.setText(Text);
        ASSERT_EQ(
            cs_option(Ref.Handle, CS_OPT_TEXT, Text ? CS_OPT_ON : CS_OPT_OFF),
            CS_ERR_OK);
        const uint8_t *Next = Bytes.data();
        size_t Remaining = Bytes.size();
        uint64_t PC = 0x2040;
        ASSERT_TRUE(
            cs_disasm_iter(Ref.Handle, &Next, &Remaining, &PC, Ref.Insn));
        ASSERT_EQ(Remaining, 0u);
        for (Method Decode : {&Decoder::decodeOne, &Decoder::decodeOneForLift,
                              &Decoder::decodeOneLight}) {
          DecodedInsn Out{};
          Out.MIPSForbiddenFollower = true;
          Out.AArch64Prefix = AArch64PrefixWords{};
          EXPECT_EQ((D.*Decode)(Bytes.data(), Bytes.size(), 0x2041, Out), 0);
          EXPECT_FALSE(Out.MIPSForbiddenFollower);
          EXPECT_FALSE(Out.AArch64Prefix);
          EXPECT_EQ((D.*Decode)(Bytes.data(), 1, 0x2040, Out), 0);
          ASSERT_EQ((D.*Decode)(Bytes.data(), Bytes.size(), 0x2040, Out),
                    Ref.Insn->size);
          EXPECT_EQ(Out.Addr, Ref.Insn->address);
          EXPECT_EQ(Out.Id, Ref.Insn->id);
          EXPECT_EQ(Out.Size, Ref.Insn->size);
          ASSERT_NE(Out.Raw, nullptr);
          ASSERT_NE(Out.Raw->detail, nullptr);
          EXPECT_EQ(Out.Raw->is_alias, Ref.Insn->is_alias);
          EXPECT_STREQ(Out.Raw->mnemonic, Ref.Insn->mnemonic);
          EXPECT_STREQ(Out.Raw->op_str, Ref.Insn->op_str);
          const auto &Actual = Out.Raw->detail->mips;
          const auto &Expected = Ref.Insn->detail->mips;
          ASSERT_EQ(Actual.op_count, Expected.op_count);
          if (Text)
            ASSERT_GT(Expected.op_count, 0);
          for (unsigned I = 0; I != Expected.op_count; ++I) {
            const auto &Got = Actual.operands[I], &Want = Expected.operands[I];
            ASSERT_EQ(Got.type, Want.type);
            EXPECT_EQ(Got.access, Want.access);
            EXPECT_EQ(Got.is_reglist, Want.is_reglist);
            EXPECT_EQ(Got.is_unsigned, Want.is_unsigned);
            if (Want.type == MIPS_OP_REG)
              EXPECT_EQ(Got.reg, Want.reg);
            else if (Want.type == MIPS_OP_IMM)
              EXPECT_EQ(Got.imm, Want.imm);
            else if (Want.type == MIPS_OP_MEM) {
              EXPECT_EQ(Got.mem.base, Want.mem.base);
              EXPECT_EQ(Got.mem.disp, Want.mem.disp);
            }
          }
        }
      }
    }
}

TEST(MIPSPhysicalDecode, WordArithmeticReadsRegistersInSourceOrder) {
  for (Arch Target : {Arch::MIPS64, Arch::MIPS64EL}) {
    for (auto Mode :
         {InstructionMode::MIPSClassic, InstructionMode::MicroMIPS}) {
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Target, Mode));
      for (bool Subtract : {false, true}) {
        SCOPED_TRACE(::testing::Message() << getArchName(Target) << "/"
                                          << unsigned(Mode) << "/" << Subtract);
        // ADDU/SUBU v0,a0,a1 and their compact ADDU16/SUBU16 encodings.
        auto Bytes = encode({Subtract ? 0x00851023u : 0x00851021u}, Target);
        if (Mode == InstructionMode::MicroMIPS) {
          const uint8_t Low = Subtract ? 0x59 : 0x58;
          Bytes = archLittleEndian(Target) ? std::vector<uint8_t>{Low, 0x05}
                                           : std::vector<uint8_t>{0x05, Low};
        }
        DecodedInsn Instruction;
        ASSERT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x10000,
                                          Instruction),
                  Bytes.size());
        std::vector<LowOp> Ops;
        Decode.liftToLow(Instruction, Ops);
        std::vector<const LowOp *> Reads;
        const LowOp *Arithmetic = nullptr;
        for (const auto &Op : Ops) {
          if (Op.Opcode == NdOp::SUBBYTES)
            Reads.push_back(&Op);
          if (Op.Opcode == (Subtract ? NdOp::INT_SUB : NdOp::INT_ADD))
            Arithmetic = &Op;
        }
        ASSERT_EQ(Reads.size(), 2u);
        ASSERT_NE(Arithmetic, nullptr);
        for (unsigned I = 0; I != 2; ++I) {
          EXPECT_EQ(Reads[I]->Inputs[0], NdVar::reg(mipsreg::r(4 + I), 8));
          EXPECT_EQ(Reads[I]->Output.Size, 4u);
          EXPECT_EQ(Arithmetic->Inputs[I], Reads[I]->Output);
        }
      }
    }
  }
}

class MIPSInteger : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPSInteger, RegisterAndStackContractsAreWidthSpecific) {
  const auto &Info = getTargetRegInfo(GetParam());
  const bool Wide = isMIPS64(GetParam());
  EXPECT_EQ(Info.TheArch, GetParam());
  EXPECT_EQ(Info.FullRegWidth, Wide ? 8 : 4);
  EXPECT_EQ(Info.IntParamRegs.size(), Wide ? 8u : 4u);
  EXPECT_EQ(Info.StackPointer, mipsreg::SP);
  EXPECT_EQ(Info.LinkRegister, mipsreg::RA);
  const auto Layout = Info.integerArgumentLayout(BinaryFormat::ELF);
  EXPECT_EQ(Layout.CallStackBase, Wide ? 0 : 16);
  EXPECT_EQ(Layout.EntryStackBase, Wide ? 0 : 16);
}

TEST_P(MIPSInteger, IntegerSourceParametersRespectPairsAndStackSlots) {
  const auto &Info = getTargetRegInfo(GetParam());
  const bool Wide = isMIPS64(GetParam());
  const bool Little = archLittleEndian(GetParam());
  const std::vector<TypeRef> Types = {NdType::makeInt(4), NdType::makeInt(8),
                                      NdType::makeInt(4)};
  const auto Placement = placeSourceParameters(GetParam(), BinaryFormat::ELF,
                                               NdType::makeInt(8), Types);
  ASSERT_TRUE(Placement);
  ASSERT_EQ(Placement->Parameters.size(), 3u);
  const auto &Pair = Placement->Parameters[1];
  ASSERT_EQ(Pair.size(), Wide ? 1u : 2u);
  EXPECT_EQ(Pair[0].Location.RegisterOffset, mipsreg::r(Wide ? 5 : 6));
  EXPECT_EQ(Pair[0].Offset, Wide || Little ? 0 : 4);
  if (!Wide) {
    EXPECT_EQ(Pair[1].Location.RegisterOffset, mipsreg::r(7));
    EXPECT_EQ(Pair[1].Offset, Little ? 4 : 0);
    EXPECT_EQ(Placement->Parameters[2][0].Location.EntryStackOffset, 16);
    EXPECT_EQ(Info.integerReturnWordOffset(0), Little ? 0u : 4u);
    EXPECT_EQ(Info.integerReturnWordOffset(1), Little ? 4u : 0u);
    const auto Arguments = integerCallArguments(GetParam(), BinaryFormat::ELF,
                                                NdType::makeInt(8), Types);
    // The general source-call adapter requires a dense carrier prefix; a1
    // is padding here and must never become an invented register read.
    EXPECT_FALSE(Arguments);
    const std::vector<TypeRef> Dense = {NdType::makeInt(8), NdType::makeInt(4)};
    const auto DenseArguments = integerCallArguments(
        GetParam(), BinaryFormat::ELF, NdType::makeInt(8), Dense);
    ASSERT_TRUE(DenseArguments);
    EXPECT_EQ(DenseArguments->Positions, 3u);
    EXPECT_EQ(DenseArguments->Parameters[0].Pieces[0].Argument,
              Little ? 0u : 1u);
  }
  std::vector<TypeRef> StackTypes(Info.IntParamRegs.size(),
                                  NdType::makeInt(Info.PointerSize));
  StackTypes.push_back(NdType::makeInt(2));
  const auto Stack = placeSourceParameters(GetParam(), BinaryFormat::ELF,
                                           NdType::makeVoid(), StackTypes);
  ASSERT_TRUE(Stack);
  ASSERT_EQ(Stack->Parameters.size(), StackTypes.size());
  const auto &Last = Stack->Parameters.back()[0].Location;
  EXPECT_EQ(Last.Kind, SourceABICarrierKind::Stack);
  EXPECT_EQ(Last.EntryStackOffset,
            Info.CallStackArgumentBase + (Little ? 0 : Info.PointerSize - 2));
}

TEST_P(MIPSInteger, DelayedTransferCannotBeLiftedWithoutItsSlot) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  const auto Bytes = encode({0x03e00008}, GetParam()); // jr ra
  DecodedInsn Instruction;
  ASSERT_EQ(
      Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x10000, Instruction),
      4);
  std::vector<LowOp> Ops;
  EXPECT_THROW(Decode.liftToLow(Instruction, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST_P(MIPSInteger, ZeroAndSignExtensionHaveIndependentExpectedValues) {
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = isMIPS64(GetParam()) ? Bitness::Bits64 : Bitness::Bits32;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(mipsreg::r(4), 0x7fffffff);
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  // addiu v0,a0,1; lui v1,0x8000; sltiu a1,zero,-1; addu zero,v0,v1
  const auto Bytes =
      encode({0x24820001, 0x3c038000, 0x2c05ffff, 0x00430021}, GetParam());
  for (size_t Offset = 0; Offset < Bytes.size(); Offset += 4) {
    DecodedInsn Instruction;
    ASSERT_EQ(Decode.decodeOneForLift(Bytes.data() + Offset,
                                      Bytes.size() - Offset, 0x10000 + Offset,
                                      Instruction),
              4);
    std::vector<LowOp> Ops;
    Decode.liftToLow(Instruction, Ops);
    for (const auto &Op : Ops) {
      EXPECT_FALSE(Op.Output.isReg() && Op.Output.Offset == mipsreg::Zero);
      ASSERT_TRUE(Emulator.step(Op));
    }
  }
  const uint64_t SignBit =
      isMIPS64(GetParam()) ? 0xffffffff80000000ull : 0x80000000ull;
  EXPECT_EQ(Emulator.getRegister(mipsreg::V0), SignBit);
  EXPECT_EQ(Emulator.getRegister(mipsreg::V1), SignBit);
  EXPECT_EQ(Emulator.getRegister(mipsreg::r(5)), 1u);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST_P(MIPSInteger, LoadsAndStoresUseTargetOrderAndAlignment) {
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = isMIPS64(GetParam()) ? Bitness::Bits64 : Bitness::Bits32;
  Segment Data;
  Data.VA = 0x2000;
  Data.Data = {0x80, 0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde};
  Data.Data.resize(32);
  Data.Size = Data.FileSz = Data.Data.size();
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Data));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(mipsreg::r(4), 0x2000);
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam()));
  auto Execute = [&](uint32_t Word) {
    const auto Bytes = encode({Word}, GetParam());
    DecodedInsn Instruction;
    if (Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000,
                                Instruction) != 4)
      return false;
    std::vector<LowOp> Ops;
    Decode.liftToLow(Instruction, Ops);
    for (const auto &Op : Ops)
      if (!Emulator.step(Op))
        return false;
    return true;
  };
  ASSERT_TRUE(Execute(0x80820000)); // lb v0,0(a0)
  EXPECT_EQ(Emulator.getRegister(mipsreg::V0),
            isMIPS64(GetParam()) ? 0xffffffffffffff80ull : 0xffffff80ull);
  ASSERT_TRUE(Execute(0x90830000)); // lbu v1,0(a0)
  EXPECT_EQ(Emulator.getRegister(mipsreg::V1), 0x80u);
  ASSERT_TRUE(Execute(0x8c820000)); // lw v0,0(a0)
  const uint32_t ExpectedWord =
      archLittleEndian(GetParam()) ? 0x56341280u : 0x80123456u;
  EXPECT_EQ(Emulator.getRegister(mipsreg::V0),
            isMIPS64(GetParam()) ? uint64_t(int64_t(int32_t(ExpectedWord)))
                                 : uint64_t(ExpectedWord));
  ASSERT_TRUE(Execute(0xac820008)); // sw v0,8(a0)
  ASSERT_TRUE(Execute(0x90830008)); // lbu v1,8(a0)
  EXPECT_EQ(Emulator.getRegister(mipsreg::V1), 0x80u);
  ASSERT_TRUE(Execute(0x9083000b)); // lbu v1,11(a0)
  EXPECT_EQ(Emulator.getRegister(mipsreg::V1), 0x56u);
  // A misaligned load must fault before overwriting its destination.
  Emulator.setRegister(mipsreg::V0, 99);
  EXPECT_FALSE(Execute(0x8c820001)); // lw v0,1(a0)
  EXPECT_EQ(Emulator.getRegister(mipsreg::V0), 99u);
  if (isMIPS64(GetParam())) {
    ASSERT_TRUE(Execute(0xdc820000)); // ld v0,0(a0)
    EXPECT_EQ(Emulator.getRegister(mipsreg::V0), archLittleEndian(GetParam())
                                                     ? 0xdebc9a7856341280ull
                                                     : 0x80123456789abcdeull);
    ASSERT_TRUE(Execute(0xfc820010)); // sd v0,16(a0)
    ASSERT_TRUE(Execute(0x90830017)); // lbu v1,23(a0)
    EXPECT_EQ(Emulator.getRegister(mipsreg::V1), 0xdeu);
  }
}

TEST_P(MIPSInteger, DefinedIntegerEffectsMatchUnicorn) {
#ifndef NEVERD_MIPS_UNICORN_REFERENCE
  GTEST_SKIP() << "The pinned Unicorn reference was not built";
#else
  const Arch Target = GetParam();
  const bool Wide = isMIPS64(Target);
  std::vector<uint32_t> Words = {
      0x2482ffff, // addiu v0,a0,-1
      0x00451821, // addu v1,v0,a1
      0x0085302a, // slt a2,a0,a1
      0x0085382b, // sltu a3,a0,a1
      0x34488000, // ori r8,v0,0x8000
      0x3c098000, // lui r9,0x8000
      0x00850018, // mult a0,a1
      0x00005010, // mfhi r10
      0x00005812, // mflo r11
      0x00850019, // multu a0,a1
      0x00006010, // mfhi r12
      0x00006812, // mflo r13
      0x00247042, // rotr r14,a0,1
      0x00a47806, // srlv r15,a0,a1
      0x70821020, // clz v0,a0
      0x70a31821, // clo v1,a1
  };
  if (Wide) {
    Words.push_back(0x648affff); // daddiu r10,a0,-1
    Words.push_back(0x0085582d); // daddu r11,a0,a1
    Words.push_back(0x0004603c); // dsll32 r12,a0,0
    Words.push_back(0x0024683e); // drotr32 r13,a0,0
    Words.push_back(0x0085001c); // dmult a0,a1
    Words.push_back(0x00007010); // mfhi r14
    Words.push_back(0x00007812); // mflo r15
  }
  const auto Bytes = encode(Words, Target);
  constexpr uint64_t Cases[] = {
      0,         1, 0x7fffffff, 0xffffffff80000000ull, 0xffffffffffffffffull,
      0x12345678};
  for (unsigned Case = 0; Case < std::size(Cases); ++Case) {
    SCOPED_TRACE(Case);
    uc_engine *Raw = nullptr;
    const auto Mode =
        static_cast<uc_mode>((Wide ? UC_MODE_MIPS64 : UC_MODE_MIPS32) |
                             (archLittleEndian(Target) ? UC_MODE_LITTLE_ENDIAN
                                                       : UC_MODE_BIG_ENDIAN));
    ASSERT_EQ(uc_open(UC_ARCH_MIPS, Mode, &Raw), UC_ERR_OK);
    std::unique_ptr<uc_engine, decltype(&uc_close)> Reference(Raw, uc_close);
    ASSERT_EQ(
        uc_ctl_set_cpu_model(Raw, Wide ? int(UC_CPU_MIPS64_MIPS64R2_GENERIC)
                                       : int(UC_CPU_MIPS32_24KC)),
        UC_ERR_OK);
    ASSERT_EQ(uc_mem_map(Raw, 0x10000, 0x1000, UC_PROT_ALL), UC_ERR_OK);
    ASSERT_EQ(uc_mem_write(Raw, 0x10000, Bytes.data(), Bytes.size()),
              UC_ERR_OK);
    BinaryImage Image;
    Image.Arch = Target;
    Image.Bits = Wide ? Bitness::Bits64 : Bitness::Bits32;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    for (unsigned I = 1; I < 32; ++I) {
      uint64_t Value = Cases[(Case + I) % std::size(Cases)];
      if (!Wide)
        Value = uint32_t(Value);
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_0 + I, &Value), UC_ERR_OK);
      Emulator.setRegister(mipsreg::r(I), Value);
    }
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Target));
    for (size_t Offset = 0; Offset < Bytes.size(); Offset += 4) {
      DecodedInsn Instruction;
      ASSERT_EQ(Decode.decodeOneForLift(Bytes.data() + Offset,
                                        Bytes.size() - Offset, 0x10000 + Offset,
                                        Instruction),
                4);
      std::vector<LowOp> Ops;
      Decode.liftToLow(Instruction, Ops);
      for (const auto &Op : Ops)
        ASSERT_TRUE(Emulator.step(Op)) << ndOpName(Op.Opcode);
    }
    const uc_err Executed =
        uc_emu_start(Raw, 0x10000, 0x10000 + Bytes.size(), 0, Words.size());
    ASSERT_EQ(Executed, UC_ERR_OK) << uc_strerror(Executed);
    for (unsigned I = 1; I < 32; ++I) {
      uint64_t Expected = 0;
      ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_0 + I, &Expected), UC_ERR_OK);
      EXPECT_EQ(Emulator.getRegister(mipsreg::r(I)), Expected) << "r" << I;
    }
    EXPECT_FALSE(Emulator.skips().any());
  }
#endif
}

INSTANTIATE_TEST_SUITE_P(Profiles, MIPSInteger,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL,
                                           Arch::MIPS64, Arch::MIPS64EL));

} // namespace
