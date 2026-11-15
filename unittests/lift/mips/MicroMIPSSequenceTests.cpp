//===- MicroMIPSSequenceTests.cpp - Compact multi-effect instructions -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise the two physical halfword layouts, fault order and tagged returns.
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

BinaryImage sequenceImage(Arch Target, unsigned Revision, unsigned DataBytes) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Mode = InstructionMode::MicroMIPS;
  Image.Bits = isMIPS64(Target) ? Bitness::Bits64 : Bitness::Bits32;
  Image.MIPS.emplace();
  Image.MIPS->ABI = isMIPS64(Target) ? MIPSABI::N64 : MIPSABI::O32;
  Image.MIPS->HasMicroMIPS = true;
  Image.MIPS->ISARevision = Revision;
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = DataBytes;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  for (unsigned I = 0; I < DataBytes; ++I)
    Data.Data.push_back(uint8_t(0x85 + 3 * I));
  Image.Segments.push_back(std::move(Data));
  return Image;
}

std::vector<uint8_t> halfwords(Arch Target, llvm::ArrayRef<uint16_t> Words) {
  std::vector<uint8_t> Bytes(Words.size() * 2);
  const auto Order = archLittleEndian(Target) ? llvm::endianness::little
                                              : llvm::endianness::big;
  for (size_t I = 0; I < Words.size(); ++I)
    llvm::support::endian::write16(Bytes.data() + 2 * I, Words[I], Order);
  return Bytes;
}

TEST(MicroMIPSSequence,
     PartialLoadKeepsEarlierRegistersAndFaultsOnMisalignment) {
  for (auto Target :
       {Arch::MIPS32, Arch::MIPS32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (unsigned Revision : {2, 6}) {
      auto Image = sequenceImage(Target, Revision, 4);
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Image));
      const auto Bytes =
          halfwords(Target, {uint16_t(Revision == 6 ? 0x4402 : 0x4500)});
      DecodedExecutionUnit Unit;
      ASSERT_EQ(Decode.decodeUnitForLift(Bytes.data(), 2, 0x10000, Unit), 2);
      std::vector<LowOp> Ops;
      Decode.liftUnitToLow(Unit, Ops);
      const uint32_t First = readTargetEndian<uint32_t>(
          Image.Segments[0].Data.data(), archLittleEndian(Target));
      for (uint32_t SP : {0x2000u, 0x2001u}) {
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(mipsreg::SP, SP);
        E.setRegister(mipsreg::r(16), 1);
        E.setRegister(mipsreg::RA, 3);
        EXPECT_FALSE(std::all_of(Ops.begin(), Ops.end(),
                                 [&](const LowOp &Op) { return E.step(Op); }));
        const uint64_t Loaded = isMIPS64(Target)
                                    ? uint64_t(int64_t(int32_t(First)))
                                    : uint64_t(First);
        EXPECT_EQ(E.getRegister(mipsreg::r(16)), SP == 0x2000 ? Loaded : 1u);
        EXPECT_EQ(E.getRegister(mipsreg::RA), 3u);
        EXPECT_EQ(E.getRegister(mipsreg::SP), SP);
      }
      if (Revision == 2) {
        for (uint16_t Slot : {0x4500, 0x4540, 0x8400, 0x4700}) {
          const auto Code = halfwords(Target, {0x459f, Slot}); // JR16 ra
          ASSERT_EQ(Decode.decodeUnitForLift(Code.data(), 4, 0x10000, Unit), 4);
          Ops.clear();
          EXPECT_THROW(Decode.liftUnitToLow(Unit, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
        }
      }
    }
}

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
// Every register selector, every list length/offset and every stack increment.
std::vector<uint16_t> sequenceWords(bool R6) {
  std::vector<uint16_t> Words;
  for (unsigned D = 0; D < 8; ++D)
    for (unsigned S = 0; S < 8; ++S)
      for (unsigned T = 0; T < 8; ++T)
        Words.push_back((R6 ? 0x4404 : 0x8400) | (D << 7) | (T << 4) |
                        (R6 ? ((S & 4) << 1) | (S & 3) : (S << 1)));
  for (unsigned Count = 0; Count < 4; ++Count)
    for (unsigned Off = 0; Off < 16; ++Off)
      for (unsigned Store : {0u, 1u})
        Words.push_back(R6 ? 0x4402 | (Count << 8) | (Off << 4) | (Store << 3)
                           : 0x4500 | (Count << 4) | Off | (Store << 6));
  for (unsigned I = 0; I < 32; ++I) {
    Words.push_back(R6 ? 0x4413 | (I << 5) : 0x4700 | I);
    if (!R6) {
      Words.push_back(0x4600 | I); // MFHI16
      Words.push_back(0x4640 | I); // MFLO16
    }
  }
  return Words;
}

TEST(MicroMIPSSequence, OriginalHalfwordsMatchAllGPRsPCAndStackBytes) {
  constexpr va_t PC = 0x10000;
  for (auto Target : {Arch::MIPS32, Arch::MIPS32EL})
    for (unsigned Revision : {2, 6}) {
      auto Image = sequenceImage(Target, Revision, 96);
      const auto &Data = Image.Segments.front().Data;
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Image));
      uc_engine *Raw = nullptr;
      auto Mode = static_cast<uc_mode>(
          UC_MODE_MIPS32 | (archLittleEndian(Target) ? UC_MODE_LITTLE_ENDIAN
                                                     : UC_MODE_BIG_ENDIAN));
      ASSERT_EQ(uc_open(UC_ARCH_MIPS, Mode, &Raw), UC_ERR_OK);
      std::unique_ptr<uc_engine, decltype(&uc_close)> Core(Raw, uc_close);
      ASSERT_EQ(uc_ctl_set_cpu_model(Raw, Revision == 6
                                              ? UC_CPU_MIPS32_MIPS32R6_GENERIC
                                              : UC_CPU_MIPS32_M14KC),
                UC_ERR_OK);
      ASSERT_EQ(uc_mem_map(Raw, PC, 0x4000, UC_PROT_ALL), UC_ERR_OK);
      ASSERT_EQ(uc_mem_map(Raw, 0x2000, 0x1000, UC_PROT_ALL), UC_ERR_OK);
      const auto Words = sequenceWords(Revision == 6);
      const auto Bytes = halfwords(Target, Words);
      // Separate test sites so a translated block cannot end in the next
      // case's compact return when Unicorn single-steps the current site.
      for (size_t I = 0; I < Words.size(); ++I)
        ASSERT_EQ(uc_mem_write(Raw, PC + 8 * I, Bytes.data() + 2 * I, 2),
                  UC_ERR_OK);
      for (size_t I = 0; I < Words.size(); ++I) {
        SCOPED_TRACE(::testing::Message()
                     << "r" << Revision << " " << std::hex << Words[I]);
        const va_t Address = PC + 8 * I;
        DecodedExecutionUnit Unit;
        ASSERT_EQ(
            Decode.decodeUnitForLift(Bytes.data() + 2 * I, 2, Address, Unit),
            2);
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(Decode.liftUnitToLow(Unit, Ops));
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        for (unsigned Reg = 1; Reg < 32; ++Reg) {
          uint32_t V = Reg == 29   ? 0x2000u
                       : Reg == 31 ? 0x13001u
                                   : 0x879a0102u + 0x32101u * Reg;
          E.setRegister(mipsreg::r(Reg), V);
          ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_0 + Reg, &V), UC_ERR_OK);
        }
        uint32_t HI = 0x89abcdef, LO = 0x76543210;
        E.setRegister(mipsreg::HI, HI);
        E.setRegister(mipsreg::LO, LO);
        ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_HI, &HI), UC_ERR_OK);
        ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_LO, &LO), UC_ERR_OK);
        ASSERT_EQ(uc_mem_write(Raw, 0x2000, Data.data(), Data.size()),
                  UC_ERR_OK);
        ASSERT_EQ(uc_emu_start(Raw, Address | 1, PC + 0x3000, 0, 1), UC_ERR_OK);
        uint32_t Next = Address + 2;
        for (const auto &Op : Ops) {
          if (Op.Opcode == NdOp::RETURN) {
            const auto V = NdOpEmulatorShadow(E).value(Op.Inputs[0]);
            ASSERT_TRUE(V);
            Next = *V & ~1u;
          } else
            ASSERT_TRUE(E.step(Op));
        }
        uint32_t Actual = 0;
        ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_PC, &Actual), UC_ERR_OK);
        EXPECT_EQ(Next, Actual);
        for (unsigned Reg = 1; Reg < 32; ++Reg) {
          ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_0 + Reg, &Actual), UC_ERR_OK);
          ASSERT_EQ(E.getRegister(mipsreg::r(Reg)), Actual) << Reg;
        }
        const auto W = Words[I];
        const bool Store =
            Revision == 6 ? (W & 0xfc0f) == 0x440a : (W & 0xffc0) == 0x4540;
        if (Store) {
          // The pinned Unicorn helper_swm stores halfwords. Compare the full
          // architectural words here; micro-swm.S independently runs SWM16
          // with the same selectors under Linux QEMU.
          auto Memory = Data;
          const unsigned Count =
              1 + (Revision == 6 ? (W >> 8) & 3 : (W >> 4) & 3);
          const unsigned Off = Revision == 6 ? (W >> 4) & 15 : W & 15;
          for (unsigned J = 0; J <= Count; ++J) {
            const uint32_t V =
                J == Count ? 0x13001u : 0x879a0102u + 0x32101u * (16 + J);
            llvm::support::endian::write32(Memory.data() + 4 * (Off + J), V,
                                           archLittleEndian(Target)
                                               ? llvm::endianness::little
                                               : llvm::endianness::big);
          }
          for (unsigned N = 0; N < Memory.size(); ++N) {
            LowOp Read;
            Read.Opcode = NdOp::LOAD;
            Read.Output = NdVar::reg(mipsreg::V0, 1);
            Read.addInput(NdVar::cst(0x2000 + N, 4));
            ASSERT_TRUE(E.step(Read));
            ASSERT_EQ(*E.getRegister(mipsreg::V0) & 255, Memory[N]) << N;
          }
        }
      }
    }
}
#endif

} // namespace
} // namespace neverd
