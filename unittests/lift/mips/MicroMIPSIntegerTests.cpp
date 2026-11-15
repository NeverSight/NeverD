//===- MicroMIPSIntegerTests.cpp - Compressed scalar semantics -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent literal encodings, expected values and original-target runs
/// exercise microMIPS halfword order and reuse of scalar LowIR semantics.

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

struct Scalar {
  const char *Name;
  uint16_t First, Second;
  unsigned Size;
  uint32_t Result;
};
constexpr Scalar Scalars[] = {
#define MICRO_SCALAR(Name, First, Second, Size, Result)                        \
  {#Name, First, Second, Size, Result},
#include "MicroMIPSScalarFixtures.def"
};

struct Memory {
  const char *Name;
  uint16_t First, Second;
  unsigned Size, Base, Register;
  int Offset;
  unsigned Width;
  bool Store, Signed;
};
constexpr Memory Memories[] = {
#define MICRO_MEMORY(Name, First, Second, Size, Base, Register, Offset, Width, \
                     Store, Signed)                                            \
  {#Name, First, Second, Size, Base, Register, Offset, Width, Store, Signed},
#include "MicroMIPSMemoryFixtures.def"
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

class MicroMIPSInteger : public ::testing::TestWithParam<Arch> {};

TEST_P(MicroMIPSInteger, MSAPhysicalIdentityAndHalfwordBranchTarget) {
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = Bitness::Bits32;
  Image.Mode = InstructionMode::MicroMIPS;
  Image.MIPS.emplace();
  Image.MIPS->HasMSA = true;
  Image.MIPS->HasMicroMIPS = true;
  Image.MIPS->ISARevision = 5;
  Decoder D;
  ASSERT_TRUE(D.init(Image));
  DecodedInsn Insn{};
  const auto Add = bytes(0x5801, 0x00aa, 4, GetParam()); // ADDV.B w2,w0,w1
  ASSERT_EQ(D.decodeOne(Add.data(), Add.size(), 0x10002, Insn), 4);
  EXPECT_EQ(Insn.Id, unsigned(MIPS_INS_ADDV_B));
  EXPECT_STREQ(Insn.Raw->mnemonic, "addv.b");
  EXPECT_EQ(Insn.Raw->address, 0x10002u);
  EXPECT_TRUE(std::equal(Add.begin(), Add.end(), Insn.Raw->bytes));
  const auto Branch = bytes(0x8301, 0xfffd, 4, GetParam()); // BZ.B w1, PC+4-6
  ASSERT_EQ(D.decodeOneLight(Branch.data(), Branch.size(), 0x10002, Insn), 4);
  EXPECT_EQ(Insn.Id, unsigned(MIPS_INS_BZ_B));
  ASSERT_NE(Insn.Raw->detail, nullptr);
  const auto &Detail = Insn.Raw->detail->mips;
  ASSERT_EQ(Detail.op_count, 2u);
  EXPECT_EQ(Detail.operands[1].imm, 0x10000);
  EXPECT_TRUE(std::equal(Branch.begin(), Branch.end(), Insn.Raw->bytes));
  // Feature changes on a reused handle cannot grant MSA semantics to the next
  // image.
  Image.MIPS->HasMSA = false;
  D.configureFor(Image);
  if (D.decodeOneForLift(Add.data(), Add.size(), 0x10002, Insn)) {
    std::vector<LowOp> Ops;
    EXPECT_THROW(D.liftToLow(Insn, Ops), UnliftedInstruction);
  }
}

TEST_P(MicroMIPSInteger,
       LiteralEncodingsHaveIndependentArithmeticExpectations) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MicroMIPS));
  BinaryImage Image;
  Image.Arch = GetParam();
  Image.Bits = Bitness::Bits32;
  for (const auto &Case : Scalars) {
    SCOPED_TRACE(Case.Name);
    const auto Code = bytes(Case.First, Case.Second, Case.Size, GetParam());
    DecodedInsn Insn;
    // A 32-bit micro instruction may start at 2 mod 4.
    ASSERT_EQ(Decode.decodeOneForLift(Code.data(), Code.size(), 0x10002, Insn),
              int(Case.Size));
    std::vector<LowOp> Ops;
    Decode.liftToLow(Insn, Ops);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::r(4), 0x89abcdef);
    Emulator.setRegister(mipsreg::r(5), 0x12345678);
    for (const auto &Op : Ops) {
      ASSERT_TRUE(Emulator.step(Op));
      EXPECT_EQ(Op.Addr, 0x10002u);
    }
    EXPECT_EQ(Emulator.getRegister(mipsreg::V0), Case.Result);
    EXPECT_FALSE(Emulator.skips().any());
    EXPECT_EQ(
        Decode.decodeOneForLift(Code.data(), Code.size() - 1, 0x10002, Insn),
        0);
    EXPECT_EQ(Decode.decodeOneLight(Code.data(), Code.size(), 0x10003, Insn),
              0);
  }
}

TEST_P(MicroMIPSInteger, ScalarResultsMatchPinnedOriginalTargetExecution) {
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
  ASSERT_EQ(uc_ctl_set_cpu_model(Raw, UC_CPU_MIPS32_M14KC), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x10000, 0x1000, UC_PROT_ALL), UC_ERR_OK);
  unsigned Index = 0;
  for (const auto &Case : Scalars) {
    SCOPED_TRACE(Case.Name);
    const auto Code = bytes(Case.First, Case.Second, Case.Size, GetParam());
    const uint64_t Address = 0x10002 + Index++ * 8;
    ASSERT_EQ(uc_mem_write(Raw, Address, Code.data(), Code.size()), UC_ERR_OK);
    uint32_t A = 0x89abcdef, B = 0x12345678, Result = 0;
    ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_4, &A), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_5, &B), UC_ERR_OK);
    ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_2, &Result), UC_ERR_OK);
    ASSERT_EQ(uc_emu_start(Raw, Address | 1, Address + Case.Size, 0, 1),
              UC_ERR_OK);
    ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_2, &Result), UC_ERR_OK);
    EXPECT_EQ(Result, Case.Result);
  }
#endif
}

TEST_P(MicroMIPSInteger, MemoryUsesTargetOrderExactWidthAndCapturedBase) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MicroMIPS));
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

TEST_P(MicroMIPSInteger, MemoryMatchesPinnedOriginalTargetExecution) {
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
  ASSERT_EQ(uc_ctl_set_cpu_model(Raw, UC_CPU_MIPS32_M14KC), UC_ERR_OK);
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

TEST_P(MicroMIPSInteger, FaultingLoadsDoNotPublishEvenWhenDiscarded) {
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
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MicroMIPS));
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

TEST_P(MicroMIPSInteger, RequiredSlotWidthAndCapturedTargetArePreserved) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MicroMIPS));
  BinaryImage Image;
  Image.Arch = GetParam();
  for (const auto &[Head, SlotSize] :
       {std::pair{uint16_t(0x45e4), 2u}, std::pair{uint16_t(0x45c4), 4u}}) {
    // jalrs16/jalr16 a0; move a0,ra. The slot changes the target register.
    auto Code = bytes(Head, 0, 2, GetParam());
    const auto Slot = SlotSize == 2 ? bytes(0x0c9f, 0, 2, GetParam())
                                    : bytes(0x001f, 0x2150, 4, GetParam());
    Code.insert(Code.end(), Slot.begin(), Slot.end());
    DecodedExecutionUnit Unit;
    ASSERT_EQ(Decode.decodeUnitForLift(Code.data(), Code.size(), 0x10002, Unit),
              int(Code.size()));
    std::vector<LowOp> Ops;
    const auto Member = Decode.liftUnitToLow(Unit, Ops);
    ASSERT_TRUE(Member);
    EXPECT_EQ(Member->Offset, 2);
    EXPECT_EQ(Member->Size, SlotSize);
    EXPECT_EQ(Decode.controlTargetMode(Unit.Head, InstructionMode::MicroMIPS),
              LowInstructionTargetMode::FromTargetBit0);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(mipsreg::r(4), 0x20001);
    for (const auto &Op : Ops) {
      if (Op.Opcode == NdOp::INDIR_CALL)
        EXPECT_EQ(Emulator.getRegister(Op.Inputs[0].Offset), 0x20001u);
      else
        ASSERT_TRUE(Emulator.step(Op));
    }
    EXPECT_EQ(Emulator.getRegister(mipsreg::r(4)), (0x10004u + SlotSize) | 1u);
    EXPECT_EQ(
        Decode.decodeUnitForLift(Code.data(), Code.size() - 1, 0x10002, Unit),
        0);
    auto Wrong = bytes(Head, 0, 2, GetParam());
    const auto WrongSlot = SlotSize == 2 ? bytes(0, 0, 4, GetParam())
                                         : bytes(0x0c00, 0, 2, GetParam());
    Wrong.insert(Wrong.end(), WrongSlot.begin(), WrongSlot.end());
    EXPECT_EQ(
        Decode.decodeUnitForLift(Wrong.data(), Wrong.size(), 0x10002, Unit), 0);
  }
}

TEST_P(MicroMIPSInteger, CompactReturnNeedsNoSlotAndSlotCannotTransfer) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MicroMIPS));
  const auto Code = bytes(0x45bf, 0, 2, GetParam()); // jrc ra
  DecodedExecutionUnit Unit;
  ASSERT_EQ(Decode.decodeUnitForLift(Code.data(), Code.size(), 0x10002, Unit),
            2);
  EXPECT_FALSE(Unit.HasDelaySlot);
  EXPECT_EQ(Decode.returnsToCaller(Unit.Head), true);
  std::vector<LowOp> Ops;
  EXPECT_FALSE(Decode.liftUnitToLow(Unit, Ops));
  ASSERT_FALSE(Ops.empty());
  EXPECT_EQ(Ops.back().Opcode, NdOp::RETURN);
  auto Bad = bytes(0x459f, 0x45bf, 4, GetParam()); // jr16 ra; jrc ra
  ASSERT_EQ(Decode.decodeUnitForLift(Bad.data(), Bad.size(), 0x10002, Unit), 4);
  Ops.clear();
  EXPECT_THROW(Decode.liftUnitToLow(Unit, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

INSTANTIATE_TEST_SUITE_P(ByteOrders, MicroMIPSInteger,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));

} // namespace
