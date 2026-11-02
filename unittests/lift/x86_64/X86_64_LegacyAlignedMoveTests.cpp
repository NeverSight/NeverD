//===- X86_64_LegacyAlignedMoveTests.cpp - Aligned vector moves -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent state and fault-order checks for legacy aligned SIMD moves.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

using namespace neverd;

namespace {

constexpr uint64_t MemoryBase = 0x8000;
constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::DF,
                                           x86reg::OF};

struct MoveEncoding {
  uint8_t Prefix;
  uint8_t LoadOpcode;
  uint8_t StoreOpcode;
};

constexpr MoveEncoding AlignedMoves[] = {
    {0x66, 0x6f, 0x7f}, {0, 0x28, 0x29}, {0x66, 0x28, 0x29}};
constexpr MoveEncoding UnalignedMoves[] = {
    {0xf3, 0x6f, 0x7f}, {0, 0x10, 0x11}, {0x66, 0x10, 0x11}};

std::vector<uint8_t> encoding(const MoveEncoding &Move, bool Store,
                              std::vector<uint8_t> Tail = {0x08},
                              uint8_t Segment = 0, bool AddressOverride = false,
                              uint8_t Rex = 0) {
  std::vector<uint8_t> Bytes;
  if (Segment)
    Bytes.push_back(Segment);
  if (AddressOverride)
    Bytes.push_back(0x67);
  if (Move.Prefix)
    Bytes.push_back(Move.Prefix);
  if (Rex)
    Bytes.push_back(Rex);
  Bytes.insert(Bytes.end(), {0x0f, Store ? Move.StoreOpcode : Move.LoadOpcode});
  Bytes.insert(Bytes.end(), Tail.begin(), Tail.end());
  return Bytes;
}

std::vector<LowOp> lift(Arch Target, const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Target));
  Dec.setStrict(true);
  DecodedInsn Insn{};
  EXPECT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
            int(Bytes.size()));
  std::vector<LowOp> Ops;
  if (!Insn.Raw)
    return Ops;
  LowInstructionUndefinedEffects Effects;
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  EXPECT_NO_THROW(Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved));
  const auto &X = Insn.Raw->detail->x86;
  bool Segmented = false;
  for (unsigned I = 0; I != X.op_count; ++I)
    Segmented |= X.operands[I].type == X86_OP_MEM &&
                 X.operands[I].mem.segment != X86_REG_INVALID;
  const bool AuditedSegment =
      Target == Arch::X64 &&
      (Insn.Raw->id == X86_INS_MOVAPS || Insn.Raw->id == X86_INS_MOVAPD ||
       Insn.Raw->id == X86_INS_MOVDQA);
  if (!Segmented || AuditedSegment) {
    EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
    EXPECT_TRUE(Effects.Effects.empty());
    EXPECT_EQ(Effects.OpCount, Ops.size());
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
    std::vector<LowOp> Plain;
    EXPECT_NO_THROW(Dec.liftToLow(Insn, Plain));
    EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
  } else {
    EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
  }
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
  return Ops;
}

size_t alignmentIndex(const std::vector<LowOp> &Ops) {
  for (size_t I = 0; I != Ops.size(); ++I)
    if (Ops[I].Opcode == NdOp::INTRINSIC && Ops[I].NumInputs &&
        Ops[I].Inputs[0].isConst() &&
        Ops[I].Inputs[0].Offset == uint64_t(Intrinsic::RequireAligned))
      return I;
  return Ops.size();
}

size_t memoryIndex(const std::vector<LowOp> &Ops, bool Store) {
  for (size_t I = 0; I != Ops.size(); ++I)
    if (Ops[I].Opcode == (Store ? NdOp::STORE : NdOp::LOAD))
      return I;
  return Ops.size();
}

std::vector<uint8_t> pattern(size_t Size, unsigned Seed) {
  std::vector<uint8_t> Bytes(Size);
  for (size_t I = 0; I != Size; ++I)
    Bytes[I] = uint8_t(Seed * 53 + I * 29 + 7);
  return Bytes;
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
  Memory.Data = pattern(Size, 91);
  Image.Segments.push_back(std::move(Memory));
  return Image;
}

struct StateOracle {
  std::array<uint64_t, 16> GPRs;
  std::array<std::vector<uint8_t>, x86reg::VectorRegCount> Vectors;

  explicit StateOracle(NdOpEmulator &Emulator) {
    for (unsigned I = 0; I != GPRs.size(); ++I)
      setGPR(Emulator, I, UINT64_C(0xacf8123400000021) + I * 0x40);
    for (unsigned I = 0; I != Vectors.size(); ++I) {
      Vectors[I] = pattern(64, I + 1);
      Emulator.setRegisterBytes(x86reg::vectorReg(I), Vectors[I]);
    }
    for (unsigned I = 0; I != Flags.size(); ++I)
      Emulator.setRegister(Flags[I], I % 2);
    Emulator.setMXCSR(0x1f80);
  }

  void setGPR(NdOpEmulator &Emulator, unsigned Index, uint64_t Value) {
    GPRs[Index] = Value;
    Emulator.setRegister(Index * 8, Value);
  }

  void expect(const NdOpEmulator &Emulator) const {
    for (unsigned I = 0; I != GPRs.size(); ++I)
      EXPECT_EQ(Emulator.getRegister(I * 8), std::optional<uint64_t>(GPRs[I]))
          << "GPR " << I;
    for (unsigned I = 0; I != Vectors.size(); ++I)
      EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(I)),
                std::optional<std::vector<uint8_t>>(Vectors[I]))
          << "complete vector " << I;
    for (unsigned I = 0; I != Flags.size(); ++I)
      EXPECT_EQ(Emulator.getRegister(Flags[I]), std::optional<uint64_t>(I % 2));
    EXPECT_EQ(Emulator.getMXCSR(), 0x1f80U);
  }
};

void expectMemory(NdOpEmulator &Emulator, uint64_t Address,
                  const std::vector<uint8_t> &Expected) {
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::tmp(0x71000000, Expected.size());
  Load.addInput(NdVar::cst(Address, 8));
  ASSERT_TRUE(Emulator.step(Load));
  EXPECT_EQ(Emulator.getRegisterBytes(Load.Output.Offset),
            std::optional<std::vector<uint8_t>>(Expected));
}

void checkAccessShape(const std::vector<LowOp> &Ops, bool Store,
                      NdMemoryAddressSpace Space) {
  const size_t Guard = alignmentIndex(Ops);
  const size_t Access = memoryIndex(Ops, Store);
  ASSERT_LT(Guard, Access);
  ASSERT_LT(Access, Ops.size());
  ASSERT_EQ(Ops[Guard].NumInputs, 3U);
  EXPECT_EQ(Ops[Guard].Output.Size, 0U);
  EXPECT_EQ(Ops[Guard].Inputs[1], Ops[Access].Inputs[0]);
  EXPECT_EQ(Ops[Guard].Inputs[1].Size, 8U);
  EXPECT_EQ(Ops[Guard].Inputs[2], NdVar::scalar(16, 8));
  EXPECT_EQ(Ops[Guard].MemoryAddressSpace, Space);
  EXPECT_EQ(Ops[Access].MemoryAddressSpace, Space);
}

void applyMoveOracle(StateOracle &State, std::vector<uint8_t> &Memory,
                     size_t Offset, bool Store, unsigned Vector = 1) {
  if (Store)
    std::copy_n(State.Vectors[Vector].begin(), 16, Memory.begin() + Offset);
  else
    std::copy_n(Memory.begin() + Offset, 16, State.Vectors[Vector].begin());
}

TEST(X86LegacyAlignedMove, AlignmentFaultPrecedesAnyArchitecturalChange) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &Move : AlignedMoves)
      for (bool Store : {false, true}) {
        const auto Ops = lift(Target, encoding(Move, Store));
        checkAccessShape(Ops, Store, NdMemoryAddressSpace::Default);
        ASSERT_FALSE(Ops.empty());
        for (unsigned Misalignment = 0; Misalignment != 16; ++Misalignment) {
          SCOPED_TRACE(::testing::Message()
                       << int(Move.Prefix) << ' ' << int(Move.LoadOpcode)
                       << " store " << Store << " offset " << Misalignment);
          BinaryImage Image = image(Target);
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          StateOracle Expected(Emulator);
          Expected.setGPR(Emulator, 0, MemoryBase + 16 + Misalignment);
          std::vector<uint8_t> Memory = Image.Segments[0].Data;
          if (!Misalignment)
            applyMoveOracle(Expected, Memory, 16, Store);
          EXPECT_EQ(Emulator.run(Ops),
                    Misalignment ? alignmentIndex(Ops) : Ops.size());
          Expected.expect(Emulator);
          expectMemory(Emulator, MemoryBase, Memory);
        }
      }
}

TEST(X86LegacyAlignedMove, GuardPrecedesUnavailableMemoryAndFaultsDoNotCommit) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &Move : AlignedMoves)
      for (bool Store : {false, true}) {
        const auto Ops = lift(Target, encoding(Move, Store));
        ASSERT_LT(alignmentIndex(Ops), memoryIndex(Ops, Store));
        for (bool Aligned : {false, true}) {
          // Eight bytes are readable/writable. A full aligned access must
          // refuse at LOAD/STORE without committing any available prefix.
          BinaryImage Image = image(Target, MemoryBase, 8);
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          StateOracle Expected(Emulator);
          Expected.setGPR(Emulator, 0,
                          Aligned ? MemoryBase : MemoryBase + 0x101);
          EXPECT_EQ(Emulator.run(Ops),
                    Aligned ? memoryIndex(Ops, Store) : alignmentIndex(Ops));
          Expected.expect(Emulator);
          expectMemory(Emulator, MemoryBase, Image.Segments[0].Data);
        }
        if (Store) {
          BinaryImage Image =
              image(Target, MemoryBase, 64, SegmentFlags::Readable);
          NdOpEmulator Emulator(Image);
          Emulator.setStrictMode(true);
          StateOracle Expected(Emulator);
          Expected.setGPR(Emulator, 0, MemoryBase + 16);
          EXPECT_EQ(Emulator.run(Ops), memoryIndex(Ops, true));
          Expected.expect(Emulator);
          expectMemory(Emulator, MemoryBase, Image.Segments[0].Data);
        }
      }
}

TEST(X86LegacyAlignedMove, RegisterCopiesPreserveHighVectorBitsAndOtherState) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &Move : AlignedMoves)
      for (bool Reverse : {false, true})
        for (bool Alias : {false, true})
          for (bool Extended : {false, true}) {
            if (Extended && Target != Arch::X64)
              continue;
            const unsigned Reg = Extended ? 9 : 1;
            const unsigned RM = Alias ? Reg : Reg + 1;
            const auto Ops = lift(
                Target, encoding(Move, Reverse,
                                 {uint8_t(0xc0 | ((Reg & 7) << 3) | (RM & 7))},
                                 0, false, Extended ? 0x45 : 0));
            EXPECT_EQ(alignmentIndex(Ops), Ops.size());
            ASSERT_FALSE(Ops.empty());
            BinaryImage Image = image(Target);
            NdOpEmulator Emulator(Image);
            Emulator.setStrictMode(true);
            StateOracle Expected(Emulator);
            const unsigned Destination = Reverse ? RM : Reg;
            const unsigned Source = Reverse ? Reg : RM;
            const auto Value = Expected.Vectors[Source];
            std::copy_n(Value.begin(), 16,
                        Expected.Vectors[Destination].begin());
            EXPECT_EQ(Emulator.run(Ops), Ops.size());
            Expected.expect(Emulator);
            expectMemory(Emulator, MemoryBase, Image.Segments[0].Data);
          }
}

TEST(X86LegacyAlignedMove, AddressWidthWrapsBeforeAlignmentAndAccess) {
  for (const auto &Move : AlignedMoves)
    for (bool Store : {false, true})
      for (unsigned Mode = 0; Mode != 3; ++Mode) {
        const Arch Target = Mode == 0 ? Arch::X64 : Arch::X86;
        const bool AddressOverride = Mode != 1;
        const auto Ops = lift(
            Target, encoding(Move, Store, {0x48, 0x40}, 0, AddressOverride));
        checkAccessShape(Ops, Store, NdMemoryAddressSpace::Default);
        BinaryImage Image = image(Target, 0x20);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        StateOracle Expected(Emulator);
        if (Mode == 2) {
          // [bx+si+0x40]: high register bits must not enter 16-bit arithmetic.
          Expected.setGPR(Emulator, x86reg::RBX / 8,
                          UINT64_C(0xcafe5678fffffff0));
          Expected.setGPR(Emulator, x86reg::RSI / 8,
                          UINT64_C(0xfeed123400000000));
        } else {
          // [eax+0x40]: 0xfffffff0 + 0x40 wraps to 0x30.
          Expected.setGPR(Emulator, 0, UINT64_C(0xcafe5678fffffff0));
        }
        std::vector<uint8_t> Memory = Image.Segments[0].Data;
        applyMoveOracle(Expected, Memory, 16, Store);
        EXPECT_EQ(Emulator.run(Ops), Ops.size());
        Expected.expect(Emulator);
        expectMemory(Emulator, 0x20, Memory);
      }
}

TEST(X86LegacyAlignedMove, SegmentedAlignmentUsesTheResolvedLinearAddress) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &Move : AlignedMoves)
      for (bool Store : {false, true})
        for (uint8_t Segment : {uint8_t(0x64), uint8_t(0x65)})
          for (bool AddressOverride : {false, true}) {
            const auto Ops = lift(Target, encoding(Move, Store, {0x08}, Segment,
                                                   AddressOverride));
            const auto Space = Segment == 0x64 ? NdMemoryAddressSpace::X86FS
                                               : NdMemoryAddressSpace::X86GS;
            checkAccessShape(Ops, Store, Space);
            for (unsigned Scenario = 0; Scenario != 3; ++Scenario) {
              BinaryImage Image = image(Target);
              NdOpEmulator Emulator(Image);
              Emulator.setStrictMode(true);
              StateOracle Expected(Emulator);
              const uint64_t Offset = Scenario == 0 ? 8 : 0;
              if (Target == Arch::X86 && AddressOverride) {
                Expected.setGPR(Emulator, x86reg::RBX / 8, Offset);
                Expected.setGPR(Emulator, x86reg::RSI / 8, 0);
              } else {
                Expected.setGPR(Emulator, 0, Offset);
              }
              if (Scenario != 2)
                ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(
                    Space, Scenario == 0 ? MemoryBase + 8 : MemoryBase + 17));
              std::vector<uint8_t> Memory = Image.Segments[0].Data;
              if (Scenario == 0)
                applyMoveOracle(Expected, Memory, 16, Store);
              EXPECT_EQ(Emulator.run(Ops),
                        Scenario == 0 ? Ops.size() : alignmentIndex(Ops));
              Expected.expect(Emulator);
              expectMemory(Emulator, MemoryBase, Memory);
            }
          }
}

TEST(X86LegacyAlignedMove, UnalignedFamiliesKeepUnalignedMemorySemantics) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &Move : UnalignedMoves)
      for (bool Store : {false, true}) {
        const auto Ops = lift(Target, encoding(Move, Store));
        EXPECT_EQ(alignmentIndex(Ops), Ops.size());
        ASSERT_FALSE(Ops.empty());
        BinaryImage Image = image(Target);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        StateOracle Expected(Emulator);
        Expected.setGPR(Emulator, 0, MemoryBase + 17);
        std::vector<uint8_t> Memory = Image.Segments[0].Data;
        applyMoveOracle(Expected, Memory, 17, Store);
        EXPECT_EQ(Emulator.run(Ops), Ops.size());
        Expected.expect(Emulator);
        expectMemory(Emulator, MemoryBase, Memory);
      }
}

TEST(X86LegacyAlignedMove, MalformedOperandsCannotSilentlyLift) {
  for (const auto &Move : AlignedMoves)
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      const auto Bytes = encoding(Move, false);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Arch::X64));
      Dec.setStrict(true);
      DecodedInsn Insn{};
      ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
                int(Bytes.size()));
      auto &X = Insn.Raw->detail->x86;
      switch (Mutation) {
      case 0:
        X.op_count = 1;
        break;
      case 1:
        X.operands[0].size = 8;
        break;
      case 2:
        X.operands[1].size = 8;
        break;
      case 3:
        X.operands[0].reg = X86_REG_RAX;
        break;
      case 4:
        X.operands[0] = X.operands[1];
        break;
      }
      std::vector<LowOp> Ops;
      EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
}

TEST(X86LegacyAlignedMove, PackedAndChecksAlignmentBeforeReadOrCommit) {
  constexpr MoveEncoding And{0x66, 0xdb, 0xdb};
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Size : {8u, 64u}) {
      const auto Ops = lift(Target, encoding(And, false));
      checkAccessShape(Ops, false, NdMemoryAddressSpace::Default);
      ASSERT_LT(alignmentIndex(Ops), memoryIndex(Ops, false));
      for (unsigned Offset = 0; Offset != 16; ++Offset) {
        BinaryImage Image = image(Target, MemoryBase, Size);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        StateOracle Expected(Emulator);
        Expected.setGPR(Emulator, 0, MemoryBase + Offset);
        const bool Success = !Offset && Size >= 16;
        if (Success)
          for (unsigned I = 0; I != 16; ++I)
            Expected.Vectors[1][I] &= Image.Segments[0].Data[I];
        EXPECT_EQ(Emulator.run(Ops), Success  ? Ops.size()
                                     : Offset ? alignmentIndex(Ops)
                                              : memoryIndex(Ops, false));
        Expected.expect(Emulator);
        expectMemory(Emulator, MemoryBase, Image.Segments[0].Data);
      }
    }
}

TEST(X86LegacyAlignedMove, PackedAndWrapsThenChecksResolvedSegmentAddress) {
  constexpr MoveEncoding And{0x66, 0xdb, 0xdb};
  for (unsigned Mode = 0; Mode != 3; ++Mode)
    for (uint8_t Segment : {uint8_t(0), uint8_t(0x64), uint8_t(0x65)})
      for (unsigned Scenario = 0; Scenario != (Segment ? 3u : 1u); ++Scenario) {
        const Arch Target = Mode == 0 ? Arch::X64 : Arch::X86;
        const auto Ops = lift(
            Target, encoding(And, false, {0x48, 0x40}, Segment, Mode != 1));
        const auto Space = Segment == 0x64   ? NdMemoryAddressSpace::X86FS
                           : Segment == 0x65 ? NdMemoryAddressSpace::X86GS
                                             : NdMemoryAddressSpace::Default;
        checkAccessShape(Ops, false, Space);
        BinaryImage Image = image(Target, 0x20);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        StateOracle Expected(Emulator);
        // The offset wraps at 32 or 16 bits before adding a segment base.
        if (Mode == 2) {
          Expected.setGPR(Emulator, x86reg::RBX / 8,
                          UINT64_C(0xcafe5678fffffff0));
          Expected.setGPR(Emulator, x86reg::RSI / 8, 0);
        } else {
          Expected.setGPR(Emulator, 0, UINT64_C(0xcafe5678fffffff0));
        }
        if (Segment && Scenario != 2)
          ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(Space, Scenario));
        if (!Scenario)
          for (unsigned I = 0; I != 16; ++I)
            Expected.Vectors[1][I] &= Image.Segments[0].Data[16 + I];
        EXPECT_EQ(Emulator.run(Ops),
                  Scenario ? alignmentIndex(Ops) : Ops.size());
        Expected.expect(Emulator);
        expectMemory(Emulator, 0x20, Image.Segments[0].Data);
      }
}

} // namespace
