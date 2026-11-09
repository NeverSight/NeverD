//===- MIPS16MemoryTests.cpp - MIPS16 memory semantics -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent literal encodings, expected values and original-target runs
/// exercise MIPS16 halfword order, memory faults and scalar LowIR semantics.

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"

#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif

#include <array>
#include <memory>
#include <vector>

using namespace neverd;

namespace {

std::vector<uint8_t> bytes(uint16_t First, uint16_t Second, unsigned Size,
                           Arch Target) {
  std::vector<uint8_t> Result;
  for (uint16_t Half : {First, Second}) {
    Result.push_back(Half >> (archLittleEndian(Target) ? 0 : 8));
    Result.push_back(Half >> (archLittleEndian(Target) ? 8 : 0));
  }
  Result.resize(Size);
  return Result;
}

struct Memory {
  const char *Name;
  uint16_t First, Second;
  unsigned Size, Base, Register;
  int Offset;
  unsigned Width;
  bool Store, Signed;
};
constexpr Memory Memories[] = {
#define MIPS16_MEMORY(Name, First, Second, Size, Base, Register, Offset,       \
                      Width, Store, Signed)                                    \
  {#Name, First, Second, Size, Base, Register, Offset, Width, Store, Signed},
#include "MIPS16MemoryFixtures.def"
};

std::array<uint8_t, 32> memoryBytes() {
  std::array<uint8_t, 32> Result;
  for (unsigned I = 0; I != Result.size(); ++I)
    Result[I] = 0x80 + I * 3;
  return Result;
}

uint32_t loadedValue(const Memory &Case, Arch Target) {
  const auto Data = memoryBytes();
  uint32_t Result = 0;
  for (unsigned I = 0; I != Case.Width; ++I)
    Result |= uint32_t(Data[8 + Case.Offset + I])
              << (8 * (archLittleEndian(Target) ? I : Case.Width - 1 - I));
  if (Case.Signed && Case.Width < 4 &&
      (Result & (uint32_t{1} << (8 * Case.Width - 1))))
    Result |= ~uint32_t{0} << (8 * Case.Width);
  return Case.Register ? Result : 0;
}

std::array<uint8_t, 32> storedBytes(const Memory &Case, Arch Target) {
  auto Data = memoryBytes();
  if (Case.Store)
    for (unsigned I = 0; I != Case.Width; ++I)
      Data[8 + Case.Offset + I] =
          (Case.Register ? 0x89abcdefu : 0u) >>
          (8 * (archLittleEndian(Target) ? I : Case.Width - 1 - I));
  return Data;
}

class MIPS16Memory : public ::testing::TestWithParam<Arch> {};

TEST_P(MIPS16Memory, MemoryUsesTargetOrderExactWidthAndCapturedBase) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  for (const auto &Case : Memories) {
    SCOPED_TRACE(Case.Name);
    BinaryImage Image;
    Image.Arch = GetParam();
    Image.Bits = Bitness::Bits32;
    Segment Data;
    const auto Initial = memoryBytes();
    Data.VA = 0x2000;
    Data.Data.assign(Initial.begin(), Initial.end());
    Data.Size = Data.FileSz = Data.Data.size();
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Segments.push_back(std::move(Data));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    if (Case.Register)
      Emulator.setRegister(mipsreg::r(Case.Register), 0x89abcdef);
    Emulator.setRegister(mipsreg::r(Case.Base), 0x2008);
    const auto Code = bytes(Case.First, Case.Second, Case.Size, GetParam());
    DecodedInsn Insn;
    ASSERT_EQ(Decode.decodeOneForLift(Code.data(), Code.size(), 0x10002, Insn),
              int(Case.Size));
    std::vector<LowOp> Ops;
    Decode.liftToLow(Insn, Ops);
    for (const auto &Op : Ops)
      ASSERT_TRUE(Emulator.step(Op));
    if (!Case.Store && Case.Register)
      EXPECT_EQ(Emulator.getRegister(mipsreg::r(Case.Register)),
                loadedValue(Case, GetParam()));
    const auto Expected = storedBytes(Case, GetParam());
    for (unsigned I = 0; I != Expected.size(); ++I) {
      LowOp Read;
      Read.Opcode = NdOp::LOAD;
      Read.Output = NdVar::reg(mipsreg::r(3), 1);
      Read.addInput(NdVar::cst(0x2000 + I, 4));
      ASSERT_TRUE(Emulator.step(Read));
      const auto Value = Emulator.getRegister(mipsreg::r(3));
      ASSERT_TRUE(Value);
      EXPECT_EQ(*Value & 255, Expected[I]) << I;
    }
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST_P(MIPS16Memory, MemoryMatchesPinnedOriginalTargetExecution) {
#ifndef NEVERD_MIPS_UNICORN_REFERENCE
  GTEST_SKIP() << "The pinned Unicorn reference was not built";
#else
  uc_engine *Raw = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_MIPS,
                    static_cast<uc_mode>(UC_MODE_MIPS32 |
                                         (archLittleEndian(GetParam())
                                              ? UC_MODE_LITTLE_ENDIAN
                                              : UC_MODE_BIG_ENDIAN)),
                    &Raw),
            UC_ERR_OK);
  std::unique_ptr<uc_engine, decltype(&uc_close)> Reference(Raw, uc_close);
  ASSERT_EQ(uc_ctl_set_cpu_model(Raw, UC_CPU_MIPS32_24KF), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x10000, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x2000, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  unsigned Index = 0;
  for (const auto &Case : Memories) {
    SCOPED_TRACE(Case.Name);
    const auto Code = bytes(Case.First, Case.Second, Case.Size, GetParam());
    const uint64_t Address = 0x10002 + Index++ * 8;
    auto Data = memoryBytes();
    ASSERT_EQ(uc_mem_write(Raw, Address, Code.data(), Code.size()), UC_ERR_OK);
    ASSERT_EQ(uc_mem_write(Raw, 0x2000, Data.data(), Data.size()), UC_ERR_OK);
    uint32_t Base = 0x2008, Value = 0x89abcdef;
    // The reference API can overwrite the backing slot even for $zero;
    // seed only architecturally writable registers.
    if (Case.Register)
      ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_0 + Case.Register, &Value),
                UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_0 + Case.Base, &Base), UC_ERR_OK);
    ASSERT_EQ(uc_emu_start(Raw, Address | 1, Address + Case.Size, 0, 1),
              UC_ERR_OK);
    if (!Case.Store) {
      ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_0 + Case.Register, &Value),
                UC_ERR_OK);
      EXPECT_EQ(Value, loadedValue(Case, GetParam()));
    }
    ASSERT_EQ(uc_mem_read(Raw, 0x2000, Data.data(), Data.size()), UC_ERR_OK);
    EXPECT_EQ(Data, storedBytes(Case, GetParam()));
  }
#endif
}

TEST_P(MIPS16Memory, FaultingLoadsDoNotPublishDestinations) {
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = Bitness::Bits32;
  Segment Data;
  Data.VA = 0x2000;
  Data.Data.resize(32, 0xa5);
  Data.Size = Data.FileSz = Data.Data.size();
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Data));
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  for (const auto &Case : Memories) {
    if (Case.Store)
      continue;
    SCOPED_TRACE(Case.Name);
    const auto Code = bytes(Case.First, Case.Second, Case.Size, GetParam());
    DecodedInsn Insn;
    ASSERT_EQ(Decode.decodeOneForLift(Code.data(), Code.size(), 0x10002, Insn),
              int(Case.Size));
    std::vector<LowOp> Ops;
    Decode.liftToLow(Insn, Ops);
    for (uint32_t Base : {0x4008u, 0x2009u}) {
      if (Base == 0x2009 && Case.Width == 1)
        continue; // Byte loads have no alignment requirement.
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      if (Case.Register)
        Emulator.setRegister(mipsreg::r(Case.Register), 99);
      Emulator.setRegister(mipsreg::r(Case.Base), Base);
      bool Faulted = false;
      for (const auto &Op : Ops)
        if (!Emulator.step(Op)) {
          Faulted = true;
          break;
        }
      EXPECT_TRUE(Faulted);
      if (Case.Register)
        EXPECT_EQ(Emulator.getRegister(mipsreg::r(Case.Register)),
                  Case.Register == Case.Base ? Base : 99u);
    }
  }
}

TEST_P(MIPS16Memory, DeadLoadFaultPrecedesTheFollowingWriteOrCall) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = Bitness::Bits32;
  for (bool Slot : {false, true}) {
    const auto Code =
        bytes(Slot ? 0xed40 : 0xa440, Slot ? 0xa440 : 0x6a01, 4, GetParam());
    std::vector<LowOp> Ops;
    for (unsigned Offset = 0; Offset < Code.size();) {
      DecodedExecutionUnit Unit;
      const int Size = Decode.decodeUnitForLift(
          Code.data() + Offset, Code.size() - Offset, 0x1000 + Offset, Unit);
      ASSERT_GT(Size, 0);
      Decode.liftUnitToLow(Unit, Ops);
      Offset += Size;
    }
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::A0, 0x4000);
    Emulator.setRegister(mipsreg::A1, 0x20001);
    Emulator.setRegister(mipsreg::V0, 99);
    bool Fault = false, Called = false;
    for (const auto &Op : Ops) {
      if (Op.Opcode == NdOp::INDIR_CALL)
        Called = true;
      if (!Emulator.step(Op)) {
        Fault = true;
        break;
      }
    }
    EXPECT_TRUE(Fault);
    EXPECT_FALSE(Called);
    EXPECT_EQ(Emulator.getRegister(mipsreg::V0), 99u);
  }
}

TEST_P(MIPS16Memory, PCRelativeBaseAlignsBeforeTheSignedOffset) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = Bitness::Bits32;
  Segment Data;
  Data.VA = 0x10000;
  Data.Data.resize(16, 0xa5);
  Data.Size = Data.FileSz = Data.Data.size();
  Data.Flags = SegmentFlags::Readable;
  Image.Segments.push_back(std::move(Data));
  for (bool Load : {false, true})
    for (bool Extended : {false, true})
      for (va_t PC : {va_t(0x10002), va_t(0x10006), va_t(0xfffffffe)})
        for (int Offset : {-32768, -4, 0, 2, 4, 32767}) {
          if (!Extended && (Offset < 0 || Offset % 4))
            continue;
          const uint16_t Half = (Load ? 0xb200 : 0x0a00) |
                                (Extended ? uint16_t(Offset) & 31 : Offset / 4);
          const uint16_t Prefix =
              0xf000 | (uint16_t(Offset) & 0x7e0) | (uint16_t(Offset) >> 11);
          const auto Code = Extended ? bytes(Prefix, Half, 4, GetParam())
                                     : bytes(Half, 0, 2, GetParam());
          DecodedInsn Insn;
          ASSERT_EQ(Decode.decodeOneForLift(Code.data(), Code.size(), PC, Insn),
                    int(Code.size()));
          std::vector<LowOp> Ops;
          Decode.liftToLow(Insn, Ops);
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          Emulator.setRegister(mipsreg::V0, 99);
          const uint32_t Expected = uint32_t((PC & ~va_t(3)) + Offset);
          bool Success = true;
          for (const auto &Op : Ops) {
            EXPECT_EQ(Op.Addr, PC);
            if (!Emulator.step(Op)) {
              Success = false;
              break;
            }
          }
          if (Load) {
            const bool Mapped = Expected >= 0x10000 && Expected <= 0x1000c;
            EXPECT_EQ(Success, Mapped && !(Expected & 3));
            EXPECT_EQ(Emulator.getRegister(mipsreg::V0),
                      Success ? 0xa5a5a5a5u : 99u);
          } else {
            ASSERT_TRUE(Success);
            EXPECT_EQ(Emulator.getRegister(mipsreg::V0), Expected);
          }
        }
}

TEST_P(MIPS16Memory, PCRelativeSlotUsesTransferHeadButKeepsSlotProvenance) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = Bitness::Bits32;
  Segment Data;
  Data.VA = 0x10008;
  Data.Data.assign(4, 0x5a);
  Data.Size = Data.FileSz = Data.Data.size();
  Data.Flags = SegmentFlags::Readable;
  Image.Segments.push_back(std::move(Data));
  for (bool Load : {false, true}) {
    // JR ra at 2 mod 4; the following halfword starts at 0 mod 4.
    const auto Code = bytes(0xe820, Load ? 0xb202 : 0x0a02, 4, GetParam());
    DecodedExecutionUnit Unit;
    ASSERT_EQ(Decode.decodeUnitForLift(Code.data(), Code.size(), 0x10002, Unit),
              4);
    std::vector<LowOp> Ops;
    const auto Member = Decode.liftUnitToLow(Unit, Ops);
    ASSERT_TRUE(Member);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::RA, 0x700001);
    for (const auto &Op : Ops)
      if (Op.Opcode != NdOp::RETURN)
        ASSERT_TRUE(Emulator.step(Op));
    for (unsigned I = Member->FirstOp; I < Member->FirstOp + Member->OpCount;
         ++I)
      EXPECT_EQ(Ops[I].Addr, 0x10004u);
    EXPECT_EQ(Emulator.getRegister(mipsreg::V0), Load ? 0x5a5a5a5au : 0x10008u);
  }
}

TEST_P(MIPS16Memory, EveryOrdinaryRegisterFieldKeepsTheCapturedBase) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = Bitness::Bits32;
  Segment Data;
  Data.VA = 0x2000;
  Data.Data.assign(4, 0xff);
  Data.Size = Data.FileSz = Data.Data.size();
  Data.Flags = SegmentFlags::Readable;
  Image.Segments.push_back(std::move(Data));
  for (unsigned Base = 0; Base != 8; ++Base)
    for (unsigned Destination = 0; Destination != 8; ++Destination) {
      const auto Code =
          bytes(0x9800 | (Base << 8) | (Destination << 5), 0, 2, GetParam());
      DecodedInsn Insn;
      ASSERT_EQ(
          Decode.decodeOneForLift(Code.data(), Code.size(), 0x10000, Insn), 2);
      std::vector<LowOp> Ops;
      Decode.liftToLow(Insn, Ops);
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setRegister(mipsreg::r(Base < 2 ? Base + 16 : Base), 0x2000);
      for (const auto &Op : Ops)
        ASSERT_TRUE(Emulator.step(Op));
      EXPECT_EQ(Emulator.getRegister(mipsreg::r(
                    Destination < 2 ? Destination + 16 : Destination)),
                0xffffffffu);
    }
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPS16Memory,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace
