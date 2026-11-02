//===- X86_64_NoIndexAddressTests.cpp - absent SIB index semantics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace neverd;

namespace {

std::vector<uint8_t> prefixes(Arch Target, unsigned AddressBytes,
                              unsigned ResultBytes, bool ExtendIndex = false) {
  std::vector<uint8_t> Bytes;
  if (Target == Arch::X64 && AddressBytes == 4)
    Bytes.push_back(0x67);
  if (ResultBytes == 2)
    Bytes.push_back(0x66);
  if (Target == Arch::X64 && (ResultBytes == 8 || ExtendIndex))
    Bytes.push_back(0x40 | (ResultBytes == 8 ? 8 : 0) | (ExtendIndex ? 2 : 0));
  return Bytes;
}

void lift(Arch Target, const std::vector<uint8_t> &Bytes,
          std::vector<LowOp> &Ops,
          LowInstructionUndefinedEffects *CapturedEffects = nullptr) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Target));
  Decode.setStrict(true);
  DecodedInsn Instruction{};
  ASSERT_EQ(
      Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Instruction),
      static_cast<int>(Bytes.size()));
  LowInstructionUndefinedEffects Effects;
  ASSERT_NO_THROW(Decode.liftToLow(Instruction, Ops, {}, {}, &Effects));
  ASSERT_FALSE(Ops.empty());
  EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  if (CapturedEffects)
    *CapturedEffects = Effects;
}

BinaryImage image(Arch Target) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Bits = Target == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Segment Memory;
  Memory.VA = 0x4000;
  Memory.Data.resize(0x100);
  const std::vector<uint8_t> InitialMemory = {0xdd, 0xcc, 0xbb, 0xaa,
                                              0x44, 0x33, 0x22, 0x11};
  std::copy(InitialMemory.begin(), InitialMemory.end(),
            Memory.Data.begin() + 0x11);
  Memory.Size = Memory.FileSz = Memory.Data.size();
  Memory.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Memory));
  return Image;
}

TEST(X86NoIndexAddress, LeaPreservesWidthAndIgnoresEveryScaleEncoding) {
  for (Arch Target : {Arch::X86, Arch::X64}) {
    for (unsigned AddressBytes : {4u, 8u}) {
      if (Target == Arch::X86 && AddressBytes == 8)
        continue;
      for (unsigned ResultBytes : {2u, 4u, 8u}) {
        if (Target == Arch::X86 && ResultBytes == 8)
          continue;
        for (unsigned Scale : {0u, 1u, 2u, 3u}) {
          SCOPED_TRACE(::testing::Message()
                       << static_cast<unsigned>(Target) << '/' << AddressBytes
                       << '/' << ResultBytes << '/' << Scale);
          auto Bytes = prefixes(Target, AddressBytes, ResultBytes);
          // LEA into AX/EAX/RAX, base BX/EBX/RBX, absent SIB index, disp8.
          Bytes.insert(
              Bytes.end(),
              {0x8d, 0x44, static_cast<uint8_t>((Scale << 6) | 0x23), 0x21});
          std::vector<LowOp> Ops;
          ASSERT_NO_FATAL_FAILURE(lift(Target, Bytes, Ops));
          NdOpEmulator Emulator(image(Target));
          Emulator.setStrictMode(true);
          const uint64_t Initial = Target == Arch::X64
                                       ? UINT64_C(0xcafe889944332211)
                                       : UINT64_C(0x44332211);
          Emulator.setRegister(x86reg::RAX, Initial);
          Emulator.setRegister(x86reg::RBX, UINT64_C(0x12345678fffffff0));
          Emulator.setRegister(x86reg::CF, 1);
          Emulator.setRegister(x86reg::ZF, 0);
          ASSERT_EQ(Emulator.run(Ops), Ops.size());
          ASSERT_FALSE(Emulator.skips().any());
          const uint64_t Address =
              AddressBytes == 8 ? UINT64_C(0x1234567900000011) : UINT64_C(0x11);
          const uint64_t Expected = ResultBytes == 8 ? Address
                                    : ResultBytes == 4
                                        ? static_cast<uint32_t>(Address)
                                        : (Initial & ~UINT64_C(0xffff)) |
                                              static_cast<uint16_t>(Address);
          EXPECT_EQ(Emulator.getRegister(x86reg::RAX), Expected);
          EXPECT_EQ(Emulator.getRegister(x86reg::CF), 1u);
          EXPECT_EQ(Emulator.getRegister(x86reg::ZF), 0u);
        }
      }
    }
  }
}

TEST(X86NoIndexAddress, LoadsAndStoresUseTheSameAbsentIndex) {
  for (Arch Target : {Arch::X86, Arch::X64}) {
    for (unsigned AddressBytes : {4u, 8u}) {
      if (Target == Arch::X86 && AddressBytes == 8)
        continue;
      for (unsigned Scale : {0u, 1u, 2u, 3u}) {
        for (bool Store : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << static_cast<unsigned>(Target) << '/' << AddressBytes
                       << '/' << Scale << '/' << Store);
          auto Bytes = prefixes(Target, AddressBytes, 4);
          Bytes.insert(Bytes.end(),
                       {static_cast<uint8_t>(Store ? 0x89 : 0x8b), 0x44,
                        static_cast<uint8_t>((Scale << 6) | 0x23), 0x11});
          std::vector<LowOp> Ops;
          ASSERT_NO_FATAL_FAILURE(lift(Target, Bytes, Ops));
          NdOpEmulator Emulator(image(Target));
          Emulator.setStrictMode(true);
          Emulator.setRegister(x86reg::RBX, AddressBytes == 4
                                                ? UINT64_C(0x100004000)
                                                : UINT64_C(0x4000));
          Emulator.setRegister(x86reg::RAX, 0x76543210);
          ASSERT_EQ(Emulator.run(Ops), Ops.size());
          ASSERT_FALSE(Emulator.skips().any());
          if (Store) {
            LowOp ReadBack;
            ReadBack.Opcode = NdOp::LOAD;
            ReadBack.Output = NdVar::reg(x86reg::RDX, 8);
            ReadBack.addInput(NdVar::address(0x4011, 8));
            ASSERT_TRUE(Emulator.step(ReadBack));
            EXPECT_EQ(Emulator.getRegister(x86reg::RDX),
                      UINT64_C(0x1122334476543210));
          } else {
            EXPECT_EQ(Emulator.getRegister(x86reg::RAX), UINT64_C(0xaabbccdd));
          }
        }
      }
    }
  }
}

TEST(X86NoIndexAddress, AbsoluteDisplacementRetainsAddressProvenance) {
  for (Arch Target : {Arch::X86, Arch::X64}) {
    for (unsigned AddressBytes : {4u, 8u}) {
      if (Target == Arch::X86 && AddressBytes == 8)
        continue;
      auto Bytes = prefixes(Target, AddressBytes, Target == Arch::X64 ? 8 : 4);
      // No base, no index, redundant scale 8, negative disp32.
      Bytes.insert(Bytes.end(), {0x8d, 0x04, 0xe5, 0x30, 0x20, 0x10, 0xf0});
      std::vector<LowOp> Ops;
      ASSERT_NO_FATAL_FAILURE(lift(Target, Bytes, Ops));
      NdOpEmulator Emulator(image(Target));
      Emulator.setStrictMode(true);
      Emulator.setRegister(x86reg::RAX, 0);
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      ASSERT_FALSE(Emulator.skips().any());
      const uint64_t Expected = AddressBytes == 8 ? UINT64_C(0xfffffffff0102030)
                                                  : UINT64_C(0xf0102030);
      EXPECT_EQ(Emulator.getRegister(x86reg::RAX), Expected);
      EXPECT_TRUE(std::any_of(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst() &&
              (Op.Inputs[I].Offset &
               (Op.Inputs[I].Size == 8 ? UINT64_MAX : UINT64_C(0xffffffff))) ==
                  Expected &&
              Op.Inputs[I].Provenance == ConstantAddressProvenance::Address)
            return true;
        return false;
      }));
    }
  }
}

TEST(X86NoIndexAddress, LogicalMemoryKeepsItsUndefinedFlagSidecar) {
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  ASSERT_NO_FATAL_FAILURE(
      lift(Arch::X64, {0x21, 0x44, 0xe3, 0x11}, Ops, &Effects));
  ASSERT_EQ(Effects.Effects.size(), 1u);
  EXPECT_EQ(Effects.Effects[0].Output, NdVar::reg(x86reg::AF, 1));
  NdOpEmulator Emulator(image(Arch::X64));
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RBX, 0x4000);
  Emulator.setRegister(x86reg::RAX, 0x0fff000f);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_FALSE(Emulator.skips().any());
  EXPECT_EQ(Emulator.getRegister(x86reg::CF), 0u);
  EXPECT_EQ(Emulator.getRegister(x86reg::OF), 0u);
  EXPECT_EQ(Emulator.getRegister(x86reg::SF), 0u);
  EXPECT_EQ(Emulator.getRegister(x86reg::ZF), 0u);
  EXPECT_EQ(Emulator.getRegister(x86reg::PF), 0u);
  LowOp ReadBack;
  ReadBack.Opcode = NdOp::LOAD;
  ReadBack.Output = NdVar::reg(x86reg::RDX, 8);
  ReadBack.addInput(NdVar::address(0x4011, 8));
  ASSERT_TRUE(Emulator.step(ReadBack));
  EXPECT_EQ(Emulator.getRegister(x86reg::RDX), UINT64_C(0x112233440abb000d));
}

TEST(X86NoIndexAddress, RexXSelectsRealR12Index) {
  for (unsigned AddressBytes : {4u, 8u}) {
    for (unsigned Scale : {0u, 1u, 2u, 3u}) {
      auto Bytes = prefixes(Arch::X64, AddressBytes, 8, true);
      Bytes.insert(
          Bytes.end(),
          {0x8d, 0x44, static_cast<uint8_t>((Scale << 6) | 0x23), 0x11});
      std::vector<LowOp> Ops;
      ASSERT_NO_FATAL_FAILURE(lift(Arch::X64, Bytes, Ops));
      NdOpEmulator Emulator(image(Arch::X64));
      Emulator.setStrictMode(true);
      Emulator.setRegister(x86reg::RBX, UINT64_C(0x100004000));
      Emulator.setRegister(x86reg::R12, 7);
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      ASSERT_FALSE(Emulator.skips().any());
      const uint64_t Sum = UINT64_C(0x100004011) + (7u << Scale);
      EXPECT_EQ(Emulator.getRegister(x86reg::RAX),
                AddressBytes == 8 ? Sum : static_cast<uint32_t>(Sum));
    }
  }
}

TEST(X86NoIndexAddress, PseudoIndicesAreWidthCheckedAndNeverBases) {
  for (bool Address32 : {false, true}) {
    for (bool MutateBase : {false, true}) {
      auto Bytes = prefixes(Arch::X64, Address32 ? 4 : 8, 8);
      Bytes.insert(Bytes.end(), {0x8d, 0x44, 0xe3, 0x21});
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Arch::X64));
      Decode.setStrict(true);
      DecodedInsn Instruction{};
      ASSERT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000,
                                        Instruction),
                static_cast<int>(Bytes.size()));
      auto &Memory = Instruction.Raw->detail->x86.operands[1].mem;
      if (Address32)
        ASSERT_TRUE(Memory.index == X86_REG_INVALID ||
                    Memory.index == X86_REG_EIZ);
      else
        ASSERT_EQ(Memory.index, X86_REG_RIZ);
      if (MutateBase)
        Memory.base = Address32 ? X86_REG_EIZ : X86_REG_RIZ;
      else
        Memory.index = Address32 ? X86_REG_RIZ : X86_REG_EIZ;
      std::vector<LowOp> Ops;
      LowInstructionUndefinedEffects Effects;
      EXPECT_THROW(Decode.liftToLow(Instruction, Ops, {}, {}, &Effects),
                   UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
      EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
    }
  }
}

TEST(X86NoIndexAddress, InvalidAddressWidthCannotAuthenticatePseudoIndex) {
  const std::vector<uint8_t> Bytes = {0x48, 0x8d, 0x44, 0xe3, 0x21};
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X64));
  Decode.setStrict(true);
  DecodedInsn Instruction{};
  ASSERT_EQ(
      Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Instruction),
      static_cast<int>(Bytes.size()));
  ASSERT_EQ(Instruction.Raw->detail->x86.operands[1].mem.index, X86_REG_RIZ);
  Instruction.Raw->detail->x86.addr_size = 3;
  std::vector<LowOp> Ops;
  LowInstructionUndefinedEffects Effects;
  EXPECT_THROW(Decode.liftToLow(Instruction, Ops, {}, {}, &Effects),
               UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
  EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
}

TEST(X86NoIndexAddress, SegmentOffsetsAndLeaRetainDistinctAddressRoles) {
  for (uint8_t Segment : {0x64, 0x65}) {
    for (bool Lea : {false, true}) {
      std::vector<uint8_t> Bytes = {
          Segment, 0x48, static_cast<uint8_t>(Lea ? 0x8d : 0x8b),
          0x04,    0xe5, 0x21,
          0,       0,    0};
      std::vector<LowOp> Ops;
      ASSERT_NO_FATAL_FAILURE(lift(Arch::X64, Bytes, Ops));
      const auto AddressSpace = Segment == 0x64 ? NdMemoryAddressSpace::X86FS
                                                : NdMemoryAddressSpace::X86GS;
      NdOpEmulator Emulator(image(Arch::X64));
      Emulator.setStrictMode(true);
      ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(AddressSpace, 0x3ff0));
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      ASSERT_FALSE(Emulator.skips().any());
      EXPECT_EQ(Emulator.getRegister(x86reg::RAX),
                Lea ? UINT64_C(0x21) : UINT64_C(0x11223344aabbccdd));
      bool SawDisplacement = false;
      for (const auto &Op : Ops) {
        if (Op.Opcode == NdOp::LOAD)
          EXPECT_EQ(Op.MemoryAddressSpace, AddressSpace);
        for (unsigned I = 0; I < Op.NumInputs; ++I)
          if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == 0x21) {
            SawDisplacement = true;
            EXPECT_EQ(Op.Inputs[I].Provenance,
                      Lea ? ConstantAddressProvenance::Address
                          : ConstantAddressProvenance::Scalar);
          }
      }
      EXPECT_TRUE(SawDisplacement);
    }
  }
}

TEST(X86NoIndexAddress, RelocatedAddressOwnershipSurvivesOnlyUnchangedBits) {
  for (unsigned AddressBytes : {4u, 8u}) {
    auto Bytes = prefixes(Arch::X64, AddressBytes, 8);
    Bytes.insert(Bytes.end(), {0x8d, 0x04, 0xe5, 0x30, 0x20, 0x10, 0xf0});
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Arch::X64));
    Decode.setStrict(true);
    DecodedInsn Instruction{};
    ASSERT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000,
                                      Instruction),
              static_cast<int>(Bytes.size()));
    RelocatedAddressOperand Reloc;
    Reloc.FieldVA = 0x1000 + Bytes.size() - 4;
    Reloc.EncodedValue = 0xf0102030;
    Reloc.Width = 4;
    Reloc.TargetVA = UINT64_C(0xfffffffff0102030);
    Reloc.TargetOwnerVA = UINT64_C(0xfffffffff0102000);
    Reloc.Provenance = ConstantAddressProvenance::DataAddress;
    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(Decode.liftToLow(Instruction, Ops, {Reloc}));
    const uint64_t Expected = AddressBytes == 8
                                  ? Reloc.TargetVA
                                  : static_cast<uint32_t>(Reloc.TargetVA);
    bool SawAddress = false;
    for (const auto &Op : Ops)
      for (unsigned I = 0; I < Op.NumInputs; ++I)
        if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == Expected) {
          SawAddress = true;
          EXPECT_EQ(Op.Inputs[I].Provenance,
                    AddressBytes == 8 ? ConstantAddressProvenance::DataAddress
                                      : ConstantAddressProvenance::Address);
          EXPECT_EQ(Op.Inputs[I].AddressOwnerVA,
                    AddressBytes == 8 ? Reloc.TargetOwnerVA : InvalidVA);
        }
    EXPECT_TRUE(SawAddress);
  }
}

} // namespace
