//===- ScalarMoveAuditTests.cpp - Legacy scalar move admission ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Native encoding checks and independent complete-state scalar move oracles.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <vector>

using namespace neverd;

namespace {

constexpr uint64_t CodeAddress = 0x1000, MemoryBase = 0x8000;
constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::DF,
                                           x86reg::OF};

struct ScalarMove {
  uint8_t Prefix;
  unsigned Bytes;
};

constexpr ScalarMove Moves[] = {{0xf3, 4}, {0xf2, 8}};

std::vector<uint8_t> encoding(ScalarMove Move, bool Reverse,
                              std::vector<uint8_t> Tail = {0x08},
                              uint8_t Rex = 0, bool AddressOverride = false) {
  std::vector<uint8_t> Bytes;
  if (AddressOverride)
    Bytes.push_back(0x67);
  Bytes.push_back(Move.Prefix);
  if (Rex)
    Bytes.push_back(Rex);
  Bytes.insert(Bytes.end(), {0x0f, uint8_t(Reverse ? 0x11 : 0x10)});
  Bytes.insert(Bytes.end(), Tail.begin(), Tail.end());
  return Bytes;
}

std::vector<LowOp> lift(Arch Target, llvm::ArrayRef<uint8_t> Bytes) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Target));
  Dec.setStrict(true);
  DecodedInsn Insn{};
  EXPECT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
            int(Bytes.size()));
  if (!Insn.Raw)
    return {};
  std::vector<LowOp> Ops, Plain;
  LowInstructionUndefinedEffects Effects;
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  Dec.liftToLow(Insn, Plain);
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic;
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
  return Ops;
}

BinaryImage image(Arch Target, uint64_t Base = MemoryBase, unsigned Size = 64,
                  SegmentFlags Permission = SegmentFlags::Readable |
                                            SegmentFlags::Writable) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Bits = Target == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Segment Memory;
  Memory.VA = Base;
  Memory.Size = Size;
  Memory.Flags = Permission;
  for (unsigned I = 0; I != Size; ++I)
    Memory.Data.push_back(uint8_t(19 + I * 29 + I / 7));
  Image.Segments.push_back(std::move(Memory));
  return Image;
}

struct StateOracle {
  std::array<uint64_t, 16> Registers;
  std::array<std::vector<uint8_t>, x86reg::VectorRegCount> Vectors;
  std::array<uint8_t, Flags.size()> FlagValues;

  explicit StateOracle(NdOpEmulator &Emulator, unsigned Seed = 0) {
    for (unsigned I = 0; I != Registers.size(); ++I)
      setRegister(Emulator, I * 8, UINT64_C(0xabcdee0012345000) + I * 61);
    for (unsigned I = 0; I != Vectors.size(); ++I) {
      for (unsigned B = 0; B != 64; ++B)
        Vectors[I].push_back(uint8_t(13 + Seed * 71 + I * 53 + B * 29));
      Emulator.setRegisterBytes(x86reg::vectorReg(I), Vectors[I]);
    }
    for (unsigned I = 0; I != Flags.size(); ++I) {
      FlagValues[I] = (Seed + I) % 2;
      Emulator.setRegister(Flags[I], FlagValues[I]);
    }
    Emulator.setMXCSR(0x5f80);
  }

  void setRegister(NdOpEmulator &Emulator, unsigned Offset, uint64_t Value) {
    Registers[Offset / 8] = Value;
    Emulator.setRegister(Offset, Value);
  }

  void expect(const NdOpEmulator &Emulator) const {
    for (unsigned I = 0; I != Registers.size(); ++I)
      EXPECT_EQ(Emulator.getRegister(I * 8),
                std::optional<uint64_t>(Registers[I]))
          << "GPR " << I;
    for (unsigned I = 0; I != Vectors.size(); ++I)
      EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(I)),
                std::optional<std::vector<uint8_t>>(Vectors[I]))
          << "complete vector " << I;
    for (unsigned I = 0; I != Flags.size(); ++I)
      EXPECT_EQ(Emulator.getRegister(Flags[I]),
                std::optional<uint64_t>(FlagValues[I]));
    EXPECT_EQ(Emulator.getMXCSR(), 0x5f80U);
  }
};

void expectMemory(NdOpEmulator &Emulator, uint64_t Base,
                  const std::vector<uint8_t> &Expected) {
  for (size_t I = 0; I != Expected.size(); ++I) {
    LowOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = NdVar::tmp(0x71000000, 1);
    Load.addInput(NdVar::cst(Base + I, 8));
    ASSERT_TRUE(Emulator.step(Load));
    EXPECT_EQ(Emulator.getRegister(Load.Output.Offset),
              std::optional<uint64_t>(Expected[I]))
        << "byte " << I;
  }
}

size_t memoryIndex(llvm::ArrayRef<LowOp> Ops, bool Store) {
  for (size_t I = 0; I != Ops.size(); ++I)
    if (Ops[I].Opcode == (Store ? NdOp::STORE : NdOp::LOAD))
      return I;
  return Ops.size();
}

void applyMemoryOracle(ScalarMove Move, StateOracle &State,
                       std::vector<uint8_t> &Memory, size_t Offset, bool Store,
                       unsigned Vector = 1) {
  if (Store) {
    std::copy_n(State.Vectors[Vector].begin(), Move.Bytes,
                Memory.begin() + Offset);
  } else {
    std::copy_n(Memory.begin() + Offset, Move.Bytes,
                State.Vectors[Vector].begin());
    std::fill(State.Vectors[Vector].begin() + Move.Bytes,
              State.Vectors[Vector].begin() + 16, 0);
  }
}

TEST(X86ScalarMoveAudit, RegisterFormsMergeOnlyScalarAndPreserveCompleteState) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto Move : Moves)
      for (bool Reverse : {false, true})
        for (bool Alias : {false, true})
          for (bool Extended : {false, true}) {
            if (Extended && Target != Arch::X64)
              continue;
            SCOPED_TRACE(::testing::Message()
                         << Move.Bytes << ' ' << Reverse << ' ' << Alias << ' '
                         << Extended);
            const unsigned Reg = Extended ? 9 : 1;
            const unsigned RM = Alias ? Reg : Reg + 1;
            const uint8_t ModRM = uint8_t(0xc0 | ((Reg & 7) << 3) | (RM & 7));
            const auto Ops = lift(
                Target, encoding(Move, Reverse, {ModRM}, Extended ? 0x45 : 0));
            ASSERT_FALSE(Ops.empty());
            BinaryImage Image = image(Target);
            NdOpEmulator Emulator(Image);
            Emulator.setStrictMode(true);
            StateOracle Expected(Emulator, Alias);
            const unsigned Destination = Reverse ? RM : Reg;
            const unsigned Source = Reverse ? Reg : RM;
            const auto SourceBytes = Expected.Vectors[Source];
            std::copy_n(SourceBytes.begin(), Move.Bytes,
                        Expected.Vectors[Destination].begin());
            EXPECT_EQ(Emulator.run(Ops), Ops.size());
            Expected.expect(Emulator);
            expectMemory(Emulator, MemoryBase, Image.Segments[0].Data);
          }
}

TEST(X86ScalarMoveAudit, MemoryFormsUseExactWidthAtEveryAlignment) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto Move : Moves)
      for (bool Store : {false, true}) {
        const auto Ops = lift(Target, encoding(Move, Store));
        const size_t Access = memoryIndex(Ops, Store);
        ASSERT_LT(Access, Ops.size());
        const auto Memory = lowMemoryOperands(Ops[Access]);
        ASSERT_TRUE(Memory.Complete);
        EXPECT_EQ(Memory.AccessSize, Move.Bytes);
        EXPECT_TRUE(std::none_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
          return Op.Opcode == NdOp::INTRINSIC;
        }));
        for (unsigned Alignment = 0; Alignment != 16; ++Alignment) {
          BinaryImage Image = image(Target);
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          StateOracle Expected(Emulator, Alignment);
          Expected.setRegister(Emulator, x86reg::RAX,
                               MemoryBase + 16 + Alignment);
          auto Bytes = Image.Segments[0].Data;
          applyMemoryOracle(Move, Expected, Bytes, 16 + Alignment, Store);
          EXPECT_EQ(Emulator.run(Ops), Ops.size());
          Expected.expect(Emulator);
          expectMemory(Emulator, MemoryBase, Bytes);
        }
        // Exactly the scalar extent is available. A 16-byte read would fail.
        BinaryImage Image = image(Target, MemoryBase, Move.Bytes);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        StateOracle Expected(Emulator);
        Expected.setRegister(Emulator, x86reg::RAX, MemoryBase);
        auto Bytes = Image.Segments[0].Data;
        applyMemoryOracle(Move, Expected, Bytes, 0, Store);
        EXPECT_EQ(Emulator.run(Ops), Ops.size());
        Expected.expect(Emulator);
        expectMemory(Emulator, MemoryBase, Bytes);
      }
}

TEST(X86ScalarMoveAudit, AddressFormsRetainRegisterWidthIndexAndDisplacement) {
  struct AddressCase {
    Arch Target;
    std::vector<uint8_t> Tail;
    uint8_t Rex;
    bool Override;
    uint64_t Base, Address;
    unsigned Vector;
    std::map<unsigned, uint64_t> Registers;
  };
  const AddressCase Cases[] = {{Arch::X64,
                                {0x4c, 0x8b, 0xfd},
                                0,
                                false,
                                MemoryBase,
                                MemoryBase + 13,
                                1,
                                {{x86reg::RBX, MemoryBase}, {x86reg::RCX, 4}}},
                               {Arch::X64,
                                {0x4c, 0xa3, 0x05},
                                0x47,
                                false,
                                MemoryBase,
                                MemoryBase + 17,
                                9,
                                {{x86reg::R11, MemoryBase}, {x86reg::R12, 3}}},
                               {Arch::X64,
                                {0x0c, 0x25, 0x11, 0x80, 0, 0},
                                0,
                                false,
                                MemoryBase,
                                MemoryBase + 17,
                                1,
                                {}},
                               {Arch::X86,
                                {0x0d, 0x11, 0x80, 0, 0},
                                0,
                                false,
                                MemoryBase,
                                MemoryBase + 17,
                                1,
                                {}},
                               {Arch::X64,
                                {0x48, 0x40},
                                0,
                                true,
                                0x20,
                                0x30,
                                1,
                                {{x86reg::RAX, UINT64_C(0xcafe5678fffffff0)}}},
                               {Arch::X86,
                                {0x48, 0x40},
                                0,
                                false,
                                0x20,
                                0x30,
                                1,
                                {{x86reg::RAX, UINT64_C(0xcafe5678fffffff0)}}},
                               {Arch::X86,
                                {0x48, 0x40},
                                0,
                                true,
                                0x20,
                                0x30,
                                1,
                                {{x86reg::RBX, UINT64_C(0xcafe5678fffffff0)},
                                 {x86reg::RSI, UINT64_C(0xfeed123400000000)}}},
                               {Arch::X64,
                                {0x0d, 0x09, 0x70, 0, 0},
                                0,
                                false,
                                MemoryBase,
                                MemoryBase + 17,
                                1,
                                {}}};
  for (const auto Move : Moves)
    for (bool Store : {false, true})
      for (const auto &C : Cases) {
        const auto Ops =
            lift(C.Target, encoding(Move, Store, C.Tail, C.Rex, C.Override));
        ASSERT_FALSE(Ops.empty());
        BinaryImage Image = image(C.Target, C.Base);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        StateOracle Expected(Emulator);
        for (const auto &[Offset, Value] : C.Registers)
          Expected.setRegister(Emulator, Offset, Value);
        auto Bytes = Image.Segments[0].Data;
        applyMemoryOracle(Move, Expected, Bytes, C.Address - C.Base, Store,
                          C.Vector);
        EXPECT_EQ(Emulator.run(Ops), Ops.size());
        Expected.expect(Emulator);
        expectMemory(Emulator, C.Base, Bytes);
      }
}

TEST(X86ScalarMoveAudit, IncompleteMemoryAndReadOnlyStoresDoNotCommit) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto Move : Moves)
      for (bool Store : {false, true}) {
        const auto Ops = lift(Target, encoding(Move, Store));
        const auto Access = memoryIndex(Ops, Store);
        ASSERT_LT(Access, Ops.size());
        for (unsigned Available = 0; Available != Move.Bytes; ++Available) {
          BinaryImage Image = image(Target, MemoryBase, Available);
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          StateOracle Expected(Emulator);
          Expected.setRegister(Emulator, x86reg::RAX, MemoryBase);
          EXPECT_EQ(Emulator.run(Ops), Access);
          Expected.expect(Emulator);
          expectMemory(Emulator, MemoryBase, Image.Segments[0].Data);
        }
        if (Store) {
          BinaryImage Image =
              image(Target, MemoryBase, 64, SegmentFlags::Readable);
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          StateOracle Expected(Emulator);
          Expected.setRegister(Emulator, x86reg::RAX, MemoryBase + 1);
          EXPECT_EQ(Emulator.run(Ops), Access);
          Expected.expect(Emulator);
          expectMemory(Emulator, MemoryBase, Image.Segments[0].Data);
        }
      }
}

void expectUnaudited(Decoder &Dec, DecodedInsn &Insn) {
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  Effects.Coverage = LowUndefinedCoverage::Complete;
  Effects.OperationDigest = "stale";
  Effects.Effects.push_back({1, NdVar::reg(x86reg::AF, 1), 0, 1, {}});
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  try {
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  } catch (const UnliftedInstruction &) {
  }
  EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_NE(Effects.OperationDigest, "stale");
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
}

TEST(X86ScalarMoveAudit, StringModernAndExtraPrefixFormsStayUnaudited) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  const std::vector<std::vector<uint8_t>> Cases = {
      {0xa5},
      {0xf3, 0xa5},
      {0xf2, 0xa5},
      {0xc5, 0xfa, 0x10, 0xc1},
      {0xc5, 0xfb, 0x10, 0xc1},
      {0x62, 0xf1, 0x7e, 0x08, 0x10, 0xc1},
      {0x62, 0xf1, 0xff, 0x08, 0x10, 0xc1},
      {0x64, 0xf3, 0x0f, 0x10, 0x08},
      {0x65, 0xf2, 0x0f, 0x11, 0x08},
      {0xf3, 0xf3, 0x0f, 0x10, 0x08},
      {0xf2, 0xf2, 0x0f, 0x10, 0x08},
      {0x66, 0xf3, 0x0f, 0x10, 0x08},
      {0xf3, 0x66, 0x0f, 0x10, 0x08},
      {0x67, 0xf2, 0x0f, 0x10, 0xc1},
      {0xf3, 0x42, 0x0f, 0x10, 0xc1},
      {0xf2, 0x40, 0x40, 0x0f, 0x10, 0xc1}};
  for (const auto &Bytes : Cases) {
    DecodedInsn Insn{};
    const int Size =
        Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn);
    if (Size <= 0)
      continue;
    ASSERT_EQ(Size, int(Bytes.size()));
    SCOPED_TRACE(Insn.Raw->mnemonic);
    expectUnaudited(Dec, Insn);
  }
}

TEST(X86ScalarMoveAudit, RawAndDetailDisagreementCannotAuthorizeEffects) {
  using Mutation = void (*)(cs_insn &);
  const Mutation Common[] = {
      [](cs_insn &I) { I.bytes[I.size++] = 0x90; },
      [](cs_insn &I) { I.bytes[0] ^= 1; },
      [](cs_insn &I) { I.detail->x86.op_count = 1; },
      [](cs_insn &I) { I.detail->x86.addr_size = 4; },
      [](cs_insn &I) { I.detail->x86.rex ^= 1; },
      [](cs_insn &I) { I.detail->x86.opcode[1] ^= 1; },
      [](cs_insn &I) { I.detail->x86.modrm ^= 8; },
      [](cs_insn &I) { I.detail->x86.prefix[0] = 0xf3; },
      [](cs_insn &I) { I.detail->x86.prefix[2] = 0x66; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset += 1; },
      [](cs_insn &I) { I.detail->x86.encoding.imm_size = 1; },
      [](cs_insn &I) { I.detail->x86.avx_sae = true; },
      [](cs_insn &I) { I.detail->x86.operands[0].size = 8; },
      [](cs_insn &I) { I.detail->x86.operands[0].reg = X86_REG_XMM2; },
      [](cs_insn &I) { I.detail->x86.operands[0].avx_zero_opmask = true; }};
  const Mutation Memory[] = {
      [](cs_insn &I) { I.detail->x86.operands[1].size = 16; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.base = X86_REG_RDX; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.index = X86_REG_R9; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.scale = 1; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.disp += 1; },
      [](cs_insn &I) { I.detail->x86.operands[1].mem.segment = X86_REG_FS; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_size = 4; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_offset -= 1; },
      [](cs_insn &I) { I.bytes[4] ^= 8; },
      [](cs_insn &I) { I.bytes[5] ^= 1; }};
  const Mutation Register[] = {
      [](cs_insn &I) { I.detail->x86.operands[1].size = 4; },
      [](cs_insn &I) { I.detail->x86.operands[1].reg = X86_REG_YMM2; },
      [](cs_insn &I) { I.detail->x86.disp = 1; },
      [](cs_insn &I) { I.detail->x86.sib = 1; }};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  for (const auto Move : Moves)
    for (bool IsMemory : {false, true}) {
      const auto Bytes =
          encoding(Move, false,
                   IsMemory ? std::vector<uint8_t>{0x4c, 0x8b, 0xfd}
                            : std::vector<uint8_t>{0xca});
      const auto Specific = IsMemory ? llvm::ArrayRef<Mutation>(Memory)
                                     : llvm::ArrayRef<Mutation>(Register);
      std::vector<Mutation> Mutations(std::begin(Common), std::end(Common));
      Mutations.insert(Mutations.end(), Specific.begin(), Specific.end());
      for (unsigned Index = 0; Index != Mutations.size(); ++Index) {
        SCOPED_TRACE(::testing::Message()
                     << Move.Bytes << '/' << IsMemory << '/' << Index);
        DecodedInsn Insn{};
        ASSERT_EQ(
            Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
            int(Bytes.size()));
        Mutations[Index](*Insn.Raw);
        expectUnaudited(Dec, Insn);
      }
    }
}

TEST(X86ScalarMoveAudit, CompleteAuditRequiresStrictLiftingAndFullSpanDigest) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (const auto Move : Moves)
    for (bool Store : {false, true}) {
      const auto Bytes = encoding(Move, Store);
      DecodedInsn Insn{};
      ASSERT_EQ(
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), CodeAddress, Insn),
          int(Bytes.size()));
      Dec.setStrict(false);
      expectUnaudited(Dec, Insn);
      Dec.setStrict(true);
      std::vector<LowOp> Ops(1);
      Ops.front().Opcode = NdOp::COPY;
      Ops.front().Output = NdVar::reg(x86reg::RDX, 8);
      Ops.front().addInput(NdVar::cst(0x55, 8));
      LowInstructionUndefinedEffects Effects;
      Dec.liftToLow(Insn, Ops, {}, {}, &Effects);
      ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
      ASSERT_GT(Ops.size(), 1U);
      EXPECT_EQ(Effects.OpCount, Ops.size() - 1);
      const auto Span = llvm::ArrayRef<LowOp>(Ops).drop_front();
      EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Span));
      EXPECT_NE(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
      auto Changed = std::vector<LowOp>(Span.begin(), Span.end());
      Changed.pop_back();
      EXPECT_NE(Effects.OperationDigest, lowUndefinedOperationDigest(Changed));
    }
}

} // namespace
