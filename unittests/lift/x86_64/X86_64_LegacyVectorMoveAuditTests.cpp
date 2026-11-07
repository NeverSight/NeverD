//===- X86_64_LegacyVectorMoveAuditTests.cpp - Vector move audit ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Complete vector values and independent admission boundaries for legacy
/// moves.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include <array>
#include <cstdint>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::DF,
                                           x86reg::OF};

struct MoveEncoding {
  unsigned Id;
  uint8_t Prefix;
  uint8_t LoadOpcode;
  uint8_t StoreOpcode;
  bool Aligned;
};

constexpr MoveEncoding Moves[] = {{X86_INS_MOVDQU, 0xf3, 0x6f, 0x7f, false},
                                  {X86_INS_MOVDQA, 0x66, 0x6f, 0x7f, true},
                                  {X86_INS_MOVUPS, 0, 0x10, 0x11, false},
                                  {X86_INS_MOVUPD, 0x66, 0x10, 0x11, false},
                                  {X86_INS_MOVAPS, 0, 0x28, 0x29, true},
                                  {X86_INS_MOVAPD, 0x66, 0x28, 0x29, true}};

std::vector<uint8_t> encoding(const MoveEncoding &Move, bool Store,
                              llvm::ArrayRef<uint8_t> Tail,
                              bool AddressPrefix = false, uint8_t Rex = 0) {
  std::vector<uint8_t> Bytes;
  if (Move.Prefix)
    Bytes.push_back(Move.Prefix);
  if (AddressPrefix)
    Bytes.push_back(0x67);
  if (Rex)
    Bytes.push_back(0x40 | Rex);
  Bytes.insert(Bytes.end(), {0x0f, Store ? Move.StoreOpcode : Move.LoadOpcode});
  Bytes.insert(Bytes.end(), Tail.begin(), Tail.end());
  return Bytes;
}

struct MemoryCase {
  Arch Target;
  std::vector<uint8_t> ModRM;
  int Base, Index;
  unsigned Scale;
  int64_t Displacement;
  bool AddressPrefix = false;
  uint8_t Rex = 0;
};

void checkMove(Arch Target, const std::vector<uint8_t> &Bytes,
               unsigned Destination, unsigned Source, const MemoryCase *Memory,
               bool Store) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Target));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
            int(Bytes.size()));
  SCOPED_TRACE(::testing::Message()
               << Insn.Raw->mnemonic << ' ' << Insn.Raw->op_str);
  std::vector<LowOp> Ops, Plain;
  LowInstructionUndefinedEffects Effects;
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  Dec.liftToLow(Insn, Plain);
  ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic;
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_EQ(Effects.OpCount, Ops.size());
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Plain));
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});

  for (unsigned Seed : {0u, 1u, 2u, 3u}) {
    SymContext Ctx;
    SymState State(Ctx);
    SymExec Exec(Ctx, State);
    std::array<uint64_t, 16> Registers;
    for (unsigned I = 0; I != Registers.size(); ++I) {
      Registers[I] = UINT64_C(0x100004003) + I * 0x40;
      State.write(SymSpace::Register, I * 8, Ctx.mkConst(64, Registers[I]));
    }
    std::vector<llvm::APInt> Vectors;
    for (unsigned I = 0; I != x86reg::VectorRegCount; ++I) {
      std::array<uint64_t, 8> Words;
      for (unsigned J = 0; J != Words.size(); ++J) {
        const uint64_t Pattern =
            UINT64_C(0xd7a319c05b8e42f1) * (1 + I + J * 37);
        Words[J] = Seed == 2   ? 0
                   : Seed == 3 ? UINT64_MAX
                   : Seed == 1 ? ~Pattern
                               : Pattern;
      }
      Vectors.emplace_back(512, llvm::ArrayRef<uint64_t>(Words));
      State.write(SymSpace::Register, x86reg::vectorReg(I),
                  Ctx.mkConst(Vectors.back()));
    }
    for (unsigned I = 0; I != Flags.size(); ++I)
      State.write(SymSpace::Register, Flags[I], Ctx.mkConst(8, (Seed + I) % 2));

    uint64_t Address = 0;
    std::array<uint8_t, 18> MemoryBytes;
    for (unsigned I = 0; I != MemoryBytes.size(); ++I)
      MemoryBytes[I] = uint8_t(0x87 + I * 29 + Seed * 71);
    if (Memory) {
      Address = Memory->Displacement;
      if (Memory->Base == -2)
        Address += 0x1000 + Bytes.size();
      else if (Memory->Base >= 0)
        Address += Registers[Memory->Base];
      if (Memory->Index >= 0)
        Address += Registers[Memory->Index] * Memory->Scale;
      const unsigned AddressSize = Target == Arch::X64
                                       ? (Memory->AddressPrefix ? 4 : 8)
                                       : (Memory->AddressPrefix ? 2 : 4);
      if (AddressSize < 8)
        Address &= (UINT64_C(1) << (AddressSize * 8)) - 1;
      ASSERT_GT(Address, 0U);
      ASSERT_LT(Address, UINT64_MAX - MemoryBytes.size());
      for (unsigned I = 0; I != MemoryBytes.size(); ++I)
        State.store(Ctx.mkConst(64, Address - 1 + I),
                    Ctx.mkConst(8, MemoryBytes[I]));
    }
    if (Memory && Store) {
      for (unsigned I = 0; I != 16; ++I)
        MemoryBytes[I + 1] = Vectors[Source].extractBitsAsZExtValue(8, I * 8);
    } else {
      llvm::APInt Value(128, 0);
      if (Memory) {
        for (unsigned I = 0; I != 16; ++I)
          Value.insertBits(llvm::APInt(8, MemoryBytes[I + 1]), I * 8);
      } else if (Insn.Raw->id == X86_INS_MOVLHPS ||
                 Insn.Raw->id == X86_INS_MOVHLPS) {
        const bool HighDestination = Insn.Raw->id == X86_INS_MOVLHPS;
        Value = Vectors[Destination].trunc(128);
        Value.insertBits(
            Vectors[Source].extractBits(64, HighDestination ? 0 : 64),
            HighDestination ? 64 : 0);
      } else {
        Value = Vectors[Source].trunc(128);
      }
      Vectors[Destination].insertBits(Value, 0);
    }
    ASSERT_EQ(Exec.run(Ops), Ops.size());
    ASSERT_EQ(Exec.unmodelledCount(), 0U);
    for (unsigned I = 0; I != Vectors.size(); ++I) {
      auto Actual =
          Ctx.asConst(State.read(SymSpace::Register, x86reg::vectorReg(I), 64));
      ASSERT_TRUE(Actual);
      EXPECT_EQ(*Actual, Vectors[I]) << "complete vector " << I;
    }
    for (unsigned I = 0; I != Registers.size(); ++I) {
      auto Actual = Ctx.asConst(State.read(SymSpace::Register, I * 8, 8));
      ASSERT_TRUE(Actual);
      EXPECT_EQ(Actual->getZExtValue(), Registers[I]);
    }
    for (unsigned I = 0; I != Flags.size(); ++I) {
      auto Actual = Ctx.asConst(State.read(SymSpace::Register, Flags[I], 1));
      ASSERT_TRUE(Actual);
      EXPECT_EQ(Actual->getZExtValue(), (Seed + I) % 2);
    }
    if (Memory)
      for (unsigned I = 0; I != MemoryBytes.size(); ++I) {
        auto Actual =
            Ctx.asConst(State.load(Ctx.mkConst(64, Address - 1 + I), 1));
        ASSERT_TRUE(Actual);
        EXPECT_EQ(Actual->getZExtValue(), MemoryBytes[I])
            << "memory byte " << I;
      }
  }
}

TEST(X86LegacyVectorMoveAudit, FullValuesAliasesFlagsAndHighVectorBits) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &Move : Moves)
      for (bool Store : {false, true})
        for (unsigned Destination : {0u, 7u, 8u, 15u})
          for (unsigned Source : {0u, 7u, 8u, 15u}) {
            if (Target == Arch::X86 && (Destination >= 8 || Source >= 8))
              continue;
            const unsigned Reg = Store ? Source : Destination;
            const unsigned RM = Store ? Destination : Source;
            const uint8_t Rex = (Reg >= 8 ? 4 : 0) | (RM >= 8 ? 1 : 0);
            const auto Bytes = encoding(
                Move, Store, {uint8_t(0xc0 | ((Reg & 7) << 3) | (RM & 7))},
                false, Rex);
            checkMove(Target, Bytes, Destination, Source, nullptr, false);
            if (testing::Test::HasFatalFailure())
              return;
          }
}

TEST(X86LegacyVectorMoveAudit, HalfMovesPreserveOtherLaneAndHighVectorBits) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (uint8_t Opcode : {0x12, 0x16})
      for (unsigned Destination : {0u, 7u, 8u, 15u})
        for (unsigned Source : {0u, 7u, 8u, 15u}) {
          if (Target == Arch::X86 && (Destination >= 8 || Source >= 8))
            continue;
          const uint8_t Rex =
              (Destination >= 8 ? 4 : 0) | (Source >= 8 ? 1 : 0);
          const MoveEncoding Move{0, 0, Opcode, Opcode, false};
          const auto Bytes = encoding(
              Move, false,
              {uint8_t(0xc0 | ((Destination & 7) << 3) | (Source & 7))}, false,
              Rex);
          checkMove(Target, Bytes, Destination, Source, nullptr, false);
          if (testing::Test::HasFatalFailure())
            return;
        }
}

TEST(X86LegacyVectorMoveAudit,
     UnalignedMemoryAndAddressFormsPreserveOtherState) {
  const MemoryCase Cases[] = {
      {Arch::X64, {0x00}, 0, -1, 1, 0},
      {Arch::X64, {0x46, 0xfd}, 6, -1, 1, -3},
      {Arch::X64, {0x84, 0x94, 0x99, 0xba, 0xff, 0xff}, 4, 2, 4, -17767},
      {Arch::X64, {0x04, 0xe4}, 4, -1, 1, 0},
      {Arch::X64, {0x04, 0x4d, 0, 0x20, 0, 0}, -1, 1, 2, 0x2000},
      {Arch::X64, {0x05, 0xe1, 0xff, 0xff, 0xff}, -2, -1, 1, -31},
      {Arch::X64, {0x05, 0xe1, 0xff, 0xff, 0xff}, -2, -1, 1, -31, true},
      {Arch::X64, {0x00}, 8, -1, 1, 0, false, 1},
      {Arch::X64, {0x04, 0x00}, 0, 8, 1, 0, false, 2},
      {Arch::X64, {0x04, 0x4b}, 3, 1, 2, 0, true},
      {Arch::X86, {0x04, 0x4b}, 3, 1, 2, 0},
      {Arch::X86, {0x00}, 3, 6, 1, 0, true},
      {Arch::X86, {0x47, 0xfd}, 3, -1, 1, -3, true},
      {Arch::X86, {0x06, 0x01, 0x90}, -1, -1, 1, int16_t(0x9001), true},
  };
  for (const auto &Move : Moves) {
    if (Move.Aligned)
      continue; // Aligned memory uses the strict emulator fault oracle.
    for (const auto &C : Cases)
      for (bool Store : {false, true})
        for (unsigned Vector : {0u, 7u, 8u, 15u}) {
          if (C.Target == Arch::X86 && Vector >= 8)
            continue;
          const uint8_t Rex = C.Rex | (Vector >= 8 ? 4 : 0);
          auto ModRM = C.ModRM;
          ModRM[0] |= (Vector & 7) << 3;
          const auto Bytes = encoding(Move, Store, ModRM, C.AddressPrefix, Rex);
          checkMove(C.Target, Bytes, Vector, Vector, &C, Store);
          if (testing::Test::HasFatalFailure())
            return;
        }
  }
}

void expectUnaudited(Decoder &Dec, DecodedInsn &Insn) {
  LowInstructionUndefinedEffects Effects;
  Effects.Coverage = LowUndefinedCoverage::Complete;
  Effects.OperationDigest = "stale";
  Effects.Effects.push_back({1, NdVar::reg(x86reg::AF, 1), 0, 1, {}});
  LowInstructionPreservedState Preserved;
  Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
  std::vector<LowOp> Ops;
  try {
    Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
  } catch (const UnliftedInstruction &) {
    // Rejection by the semantic lifter is also fail-closed admission.
  }
  EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_TRUE(Effects.Effects.empty());
  EXPECT_NE(Effects.OperationDigest, "stale");
  EXPECT_EQ(Preserved, LowInstructionPreservedState{});
}

TEST(X86LegacyVectorMoveAudit, HalfMovesBindRegisterEncodingAndDetail) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  using Mutation = void (*)(cs_insn &);
  const Mutation Mutations[] = {
      [](cs_insn &I) { I.bytes[I.detail->x86.encoding.modrm_offset] ^= 8; },
      [](cs_insn &I) { I.bytes[I.detail->x86.encoding.modrm_offset - 1] ^= 4; },
      [](cs_insn &I) { I.bytes[I.size++] = 0x90; },
      [](cs_insn &I) { I.detail->x86.op_count = 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].size = 8; },
      [](cs_insn &I) { I.detail->x86.operands[1].reg = X86_REG_XMM2; },
      [](cs_insn &I) { I.detail->x86.rex ^= 4; },
      [](cs_insn &I) { I.detail->x86.prefix[0] = 0xf3; },
      [](cs_insn &I) { I.detail->x86.prefix[2] = 0x66; },
      [](cs_insn &I) { I.detail->x86.eflags = X86_EFLAGS_MODIFY_ZF; },
      [](cs_insn &I) { I.detail->x86.addr_size = 4; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset += 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].avx_zero_opmask = true; },
  };
  for (uint8_t Opcode : {0x12, 0x16})
    for (uint8_t Rex : {0, 5}) {
      const MoveEncoding Move{0, 0, Opcode, Opcode, false};
      const auto Bytes = encoding(Move, false, {0xc1}, false, Rex);
      for (Mutation Change : Mutations) {
        DecodedInsn Insn{};
        ASSERT_EQ(
            Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
            int(Bytes.size()));
        Change(*Insn.Raw);
        expectUnaudited(Dec, Insn);
      }
      for (uint8_t Prefix : {0x66, 0x67, 0xf2, 0xf3}) {
        auto Prefixed = Bytes;
        Prefixed.insert(Prefixed.begin(), Prefix);
        DecodedInsn Insn{};
        if (Dec.decodeOneForLift(Prefixed.data(), Prefixed.size(), 0x1000,
                                 Insn) <= 0)
          continue;
        // Another valid instruction may share this opcode, but cannot claim
        // the unprefixed half-move contract.
        Insn.Raw->id = Opcode == 0x12 ? X86_INS_MOVHLPS : X86_INS_MOVLHPS;
        expectUnaudited(Dec, Insn);
      }
    }
}

TEST(X86LegacyVectorMoveAudit,
     OtherEncodingsAndUnalignedSegmentedMemoryRemainUnaudited) {
  const std::vector<std::vector<uint8_t>> Cases = {
      {0x64, 0x65, 0x0f, 0x28, 0x00},       // conflicting segments
      {0x64, 0x64, 0x0f, 0x28, 0x00},       // repeated segment
      {0x64, 0x0f, 0x28, 0xc1},             // segment on register form
      {0x40, 0x64, 0x0f, 0x28, 0x00},       // legacy prefix after REX
      {0xc5, 0xf9, 0x6f, 0x00},             // VEX aligned load
      {0xc5, 0xfa, 0x6f, 0xc1},             // VEX.128
      {0xc5, 0xfe, 0x6f, 0xc1},             // VEX.256
      {0x62, 0xf1, 0x7e, 0x48, 0x6f, 0xc1}, // EVEX.512
      {0xf3, 0xf3, 0x0f, 0x6f, 0xc1},
      {0x66, 0x66, 0x0f, 0x6f, 0xc1},
      {0x66, 0xf3, 0x0f, 0x6f, 0xc1},
      {0xf3, 0x40, 0x40, 0x0f, 0x6f, 0xc1},
      {0xf3, 0x67, 0x67, 0x0f, 0x6f, 0x00},
      {0xf3, 0x64, 0x0f, 0x6f, 0x00},
      {0x0f, 0x6f, 0xc1},                   // MMX
      {0xc5, 0xf8, 0x10, 0xc1},             // VEX floating register move
      {0xc5, 0xf9, 0x11, 0x00},             // VEX floating memory store
      {0xc5, 0xf8, 0x28, 0x00},             // VEX floating aligned load
      {0x62, 0xf1, 0x7c, 0x48, 0x10, 0xc1}, // EVEX floating move
      {0x66, 0x66, 0x0f, 0x28, 0xc1},
      {0x67, 0x67, 0x0f, 0x11, 0x00},
      {0x40, 0x40, 0x0f, 0x29, 0xc1},
  };
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (const auto &Bytes : Cases) {
    DecodedInsn Insn{};
    const int Size =
        Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn);
    if (Size <= 0)
      continue;
    ASSERT_EQ(Size, int(Bytes.size()));
    SCOPED_TRACE(::testing::Message()
                 << Insn.Raw->mnemonic << ' ' << Insn.Raw->op_str);
    expectUnaudited(Dec, Insn);
  }
}

cs_x86_op &memoryOperand(cs_insn &Insn) {
  auto &X = Insn.detail->x86;
  return X.operands[X.operands[0].type == X86_OP_MEM ? 0 : 1];
}

TEST(X86LegacyVectorMoveAudit, MalformedRawAndDetailCannotAuthorizeCoverage) {
  using Mutation = void (*)(cs_insn &);
  const Mutation RegisterMutations[] = {
      [](cs_insn &I) { I.bytes[I.detail->x86.encoding.modrm_offset] ^= 8; },
      [](cs_insn &I) { I.bytes[I.detail->x86.encoding.modrm_offset - 1] ^= 1; },
      [](cs_insn &I) { I.bytes[I.size++] = 0x90; },
      [](cs_insn &I) { I.detail->x86.op_count = 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].size = 8; },
      [](cs_insn &I) { I.detail->x86.operands[1].reg = X86_REG_XMM2; },
      [](cs_insn &I) { I.detail->x86.rex = 0x44; },
      [](cs_insn &I) { I.detail->x86.prefix[0] = 0xf3; },
      [](cs_insn &I) {
        I.detail->x86.prefix[2] = I.detail->x86.prefix[2] ? 0 : 0x66;
      },
      [](cs_insn &I) { I.detail->x86.addr_size = 4; },
      [](cs_insn &I) { I.detail->x86.encoding.modrm_offset += 1; },
      [](cs_insn &I) { I.detail->x86.operands[0].avx_zero_opmask = true; },
      [](cs_insn &I) { I.detail->x86.operands[1].avx_bcast = X86_AVX_BCAST_4; },
  };
  const Mutation MemoryMutations[] = {
      [](cs_insn &I) { memoryOperand(I).mem.base = X86_REG_RDX; },
      [](cs_insn &I) { memoryOperand(I).mem.index = X86_REG_RSI; },
      [](cs_insn &I) { memoryOperand(I).mem.scale = 3; },
      [](cs_insn &I) { memoryOperand(I).mem.segment = X86_REG_FS; },
      [](cs_insn &I) { memoryOperand(I).mem.disp += 1; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_offset -= 1; },
      [](cs_insn &I) { I.detail->x86.encoding.disp_size = 4; },
      [](cs_insn &I) { memoryOperand(I).size = 8; },
      [](cs_insn &I) { I.bytes[I.detail->x86.encoding.modrm_offset + 1] ^= 8; },
  };
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (const auto &Move : Moves)
    for (bool Store : {false, true})
      for (bool Memory : {false, true}) {
        const auto Bytes =
            encoding(Move, Store,
                     Memory ? llvm::ArrayRef<uint8_t>({0x44, 0x4b, 0xfd})
                            : llvm::ArrayRef<uint8_t>({0xc1}));
        const auto Mutations =
            Memory ? llvm::ArrayRef<Mutation>(MemoryMutations)
                   : llvm::ArrayRef<Mutation>(RegisterMutations);
        for (unsigned Index = 0; Index != Mutations.size(); ++Index) {
          SCOPED_TRACE(::testing::Message() << Move.Id << '/' << Store << '/'
                                            << Memory << '/' << Index);
          DecodedInsn Insn{};
          ASSERT_EQ(
              Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
              int(Bytes.size()));
          Mutations[Index](*Insn.Raw);
          expectUnaudited(Dec, Insn);
        }
      }
}

TEST(X86LegacyVectorMoveAudit, InstructionIdentityBindsPrefixAndOpcode) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (const auto &Move : Moves)
    for (bool Store : {false, true})
      for (bool Memory : {false, true}) {
        const auto Bytes =
            encoding(Move, Store, {uint8_t(Memory ? 0x08 : 0xc1)});
        for (const auto &Other : Moves) {
          if (Move.Id == Other.Id)
            continue;
          SCOPED_TRACE(::testing::Message() << Move.Id << '/' << Other.Id << '/'
                                            << Store << '/' << Memory);
          DecodedInsn Insn{};
          ASSERT_EQ(
              Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
              int(Bytes.size()));
          Insn.Raw->id = Other.Id;
          expectUnaudited(Dec, Insn);
        }
      }
}

TEST(X86LegacyVectorMoveAudit, ZeroByteCannotMasqueradeAsAnAbsentPrefix) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (const auto &Move : Moves) {
    if (Move.Prefix)
      continue;
    for (bool Store : {false, true})
      for (bool Memory : {false, true}) {
        const auto Bytes =
            encoding(Move, Store, {uint8_t(Memory ? 0x08 : 0xc1)});
        DecodedInsn Insn{};
        ASSERT_EQ(
            Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
            int(Bytes.size()));
        // Keep the opcode and detail offsets consistent after prepending 00.
        // A zero mandatory-prefix sentinel must never consume a raw byte.
        for (unsigned I = Insn.Raw->size; I != 0; --I)
          Insn.Raw->bytes[I] = Insn.Raw->bytes[I - 1];
        Insn.Raw->bytes[0] = 0;
        ++Insn.Raw->size;
        ++Insn.Raw->detail->x86.encoding.modrm_offset;
        expectUnaudited(Dec, Insn);
      }
  }
}

TEST(X86LegacyVectorMoveAudit, SegmentAndPrefixBoundariesRemainUnaudited) {
  for (Arch Target : {Arch::X86, Arch::X64}) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Target));
    for (const auto &Move : Moves)
      for (bool Store : {false, true})
        for (uint8_t Prefix : {uint8_t(0x64), uint8_t(0x65), uint8_t(0x67)}) {
          // Only x64 aligned moves bind the explicit FS/GS guard and access.
          // Repeated 67 remains noncanonical even when normalized away.
          if (Target == Arch::X64 && Move.Aligned && Prefix != 0x67)
            continue;
          auto Bytes = encoding(Move, Store, {0x08}, Prefix == 0x67);
          Bytes.insert(Bytes.begin(), Prefix);
          DecodedInsn Insn{};
          const int Size =
              Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn);
          if (Size <= 0)
            continue;
          ASSERT_EQ(Size, int(Bytes.size()));
          SCOPED_TRACE(::testing::Message()
                       << Move.Id << '/' << Store << '/' << unsigned(Prefix));
          expectUnaudited(Dec, Insn);
        }
  }
}

TEST(X86LegacyVectorMoveAudit,
     AlignedSegmentsBindGuardAndMemoryToOriginalBytes) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  for (const auto &Move : Moves) {
    if (!Move.Aligned)
      continue;
    for (bool Store : {false, true})
      for (uint8_t Segment : {uint8_t(0x64), uint8_t(0x65)})
        for (bool AddressPrefix : {false, true}) {
          auto Bytes =
              encoding(Move, Store, {0x44, 0x4b, 0xfd}, AddressPrefix, 4);
          Bytes.insert(Bytes.begin(), Segment);
          for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
            SCOPED_TRACE(::testing::Message()
                         << Move.Id << '/' << Store << '/' << unsigned(Segment)
                         << '/' << AddressPrefix << '/' << Mutation);
            DecodedInsn Insn{};
            ASSERT_EQ(
                Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
                int(Bytes.size()));
            if (Mutation == 1)
              Insn.Raw->bytes[0] ^= 1;
            else if (Mutation == 2)
              Insn.Raw->detail->x86.prefix[1] ^= 1;
            else if (Mutation == 3)
              memoryOperand(*Insn.Raw).mem.segment = X86_REG_INVALID;
            else if (Mutation == 4)
              Insn.Raw->detail->x86.addr_size = AddressPrefix ? 8 : 4;
            if (Mutation) {
              expectUnaudited(Dec, Insn);
              continue;
            }
            std::vector<LowOp> Ops;
            LowInstructionUndefinedEffects Effects;
            LowInstructionPreservedState Preserved;
            Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
            ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
                << Effects.Diagnostic;
            EXPECT_TRUE(Effects.Effects.empty());
            EXPECT_EQ(Effects.OperationDigest,
                      lowUndefinedOperationDigest(Ops));
            EXPECT_EQ(Preserved, LowInstructionPreservedState{});
            unsigned SegmentedOperations = 0;
            for (const auto &Op : Ops)
              if (Op.MemoryAddressSpace != NdMemoryAddressSpace::Default) {
                ++SegmentedOperations;
                EXPECT_EQ(Op.MemoryAddressSpace,
                          Segment == 0x64 ? NdMemoryAddressSpace::X86FS
                                          : NdMemoryAddressSpace::X86GS);
              }
            EXPECT_EQ(SegmentedOperations, 2u);
          }
        }
  }
}

} // namespace
