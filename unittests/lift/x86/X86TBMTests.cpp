//===- X86TBMTests.cpp - TBM values, flags and encoding admission --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent bit-position oracles for the nine trailing-bit operations.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <array>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t Entry = 0x1000;
constexpr va_t DataAddress = 0x7000;
struct Encoding {
  unsigned Id;
  uint8_t Opcode;
  uint8_t Group;
};
constexpr Encoding Encodings[] = {
    {X86_INS_BLCFILL, 1, 1}, {X86_INS_BLCI, 2, 6},   {X86_INS_BLCIC, 1, 5},
    {X86_INS_BLCMSK, 2, 1},  {X86_INS_BLCS, 1, 3},   {X86_INS_BLSFILL, 1, 2},
    {X86_INS_BLSIC, 1, 6},   {X86_INS_T1MSKC, 1, 7}, {X86_INS_TZMSK, 1, 4}};
constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::ZF, x86reg::SF,
                                           x86reg::OF, x86reg::DF, x86reg::AF,
                                           x86reg::PF};

uint64_t mask(unsigned Width) { return Width == 8 ? UINT64_MAX : UINT32_MAX; }

// Scan for the first zero/one, without repeating the lifter's +/-1 formula.
uint64_t expected(unsigned Id, uint64_t Source, unsigned Width) {
  const bool Set =
      Id == X86_INS_BLSFILL || Id == X86_INS_BLSIC || Id == X86_INS_TZMSK;
  const unsigned Bits = Width * 8;
  unsigned First = 0;
  while (First != Bits && ((Source >> First) & 1) != Set)
    ++First;
  const uint64_t Below =
      First == Bits ? mask(Width) : (UINT64_C(1) << First) - 1;
  const uint64_t Bit = First == Bits ? 0 : UINT64_C(1) << First;
  switch (Id) {
  case X86_INS_BLCFILL:
    return Source & ~Below;
  case X86_INS_BLCI:
    return mask(Width) ^ Bit;
  case X86_INS_BLCIC:
    return Bit;
  case X86_INS_BLCMSK:
    return Below | Bit;
  case X86_INS_BLCS:
    return Source | Bit;
  case X86_INS_BLSFILL:
    return Source | Below;
  case X86_INS_BLSIC:
    return mask(Width) ^ Bit;
  case X86_INS_T1MSKC:
    return mask(Width) ^ Below;
  case X86_INS_TZMSK:
    return Below;
  default:
    ADD_FAILURE();
    return 0;
  }
}

std::vector<uint8_t> encode(const Encoding &E, bool W, unsigned Destination,
                            unsigned Source, bool Memory = false) {
  return {0x8f, static_cast<uint8_t>(0xe9 ^ (Source >= 8 ? 0x20 : 0)),
          static_cast<uint8_t>((W ? 0x80 : 0) | ((~Destination & 15) << 3)),
          E.Opcode,
          static_cast<uint8_t>((Memory ? 0 : 0xc0) | (E.Group << 3) |
                               (Source & 7))};
}

int decode(Decoder &D, const std::vector<uint8_t> &Bytes, unsigned Route,
           DecodedInsn &Insn) {
  return Route == 0 ? D.decodeOne(Bytes.data(), Bytes.size(), Entry, Insn)
         : Route == 1
             ? D.decodeOneLight(Bytes.data(), Bytes.size(), Entry, Insn)
             : D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, Insn);
}

BinaryImage image(Arch Target, uint64_t Value, unsigned Width) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Bits = Target == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Segment Data;
  Data.VA = DataAddress;
  Data.Size = Width;
  Data.Flags = SegmentFlags::Readable;
  for (unsigned I = 0; I < Width; ++I)
    Data.Data.push_back(Value >> (I * 8));
  Image.Segments.push_back(std::move(Data));
  return Image;
}

TEST(X86TBM, ResultsFlagsAliasesAndMemoryAcrossDecodeRoutes) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool W : {false, true}) {
      const unsigned Width = Target == Arch::X64 && W ? 8 : 4;
      for (const auto &E : Encodings)
        for (auto [Destination, Source] :
             {std::pair{0u, 1u}, {1u, 1u}, {9u, 10u}, {9u, 9u}}) {
          if (Target == Arch::X86 && Source >= 8)
            continue;
          for (bool Memory : {false, true})
            for (unsigned Route : {0u, 1u, 2u}) {
              const auto Bytes = encode(E, W, Destination, Source, Memory);
              SCOPED_TRACE(testing::PrintToString(Bytes));
              SCOPED_TRACE(static_cast<unsigned>(Target));
              SCOPED_TRACE(Route);
              Decoder D;
              ASSERT_TRUE(D.init(Target));
              D.setStrict(true);
              DecodedInsn Insn{};
              ASSERT_EQ(decode(D, Bytes, Route, Insn), Bytes.size());
              ASSERT_EQ(Insn.Id, E.Id);
              std::vector<LowOp> Ops, Plain;
              LowInstructionUndefinedEffects Effects;
              ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
              ASSERT_NO_THROW(D.liftToLow(Insn, Plain));
              ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
                  << Effects.Diagnostic;
              EXPECT_EQ(Effects.OperationDigest,
                        lowUndefinedOperationDigest(Plain));
              ASSERT_EQ(Effects.Effects.size(), 2u);
              EXPECT_EQ(Effects.Effects[0].Output, NdVar::reg(x86reg::AF, 1));
              EXPECT_EQ(Effects.Effects[1].Output, NdVar::reg(x86reg::PF, 1));
              for (const auto &Effect : Effects.Effects) {
                EXPECT_EQ(Effect.BitCount, 1u);
                EXPECT_EQ(Effect.BitOffset, 0u);
                EXPECT_FALSE(Effect.When);
              }
              const uint64_t Sign = UINT64_C(1) << (Width * 8 - 1);
              for (uint64_t Value :
                   {UINT64_C(0), UINT64_C(1), UINT64_C(2), UINT64_C(7),
                    UINT64_C(8), Sign - 1, Sign, mask(Width) - 1, mask(Width),
                    UINT64_C(0xa5195ac3fe817204) & mask(Width)})
                for (unsigned Old : {0u, 0x55u, 0x2au, 0x7fu}) {
                  auto Image = image(Target, Value, Width);
                  NdOpEmulator Emulator(Image);
                  Emulator.setStrictMode(true);
                  for (unsigned R = 0; R != (Target == Arch::X64 ? 16 : 8); ++R)
                    Emulator.setRegister(R * 8,
                                         Target == Arch::X64
                                             ? UINT64_C(0xaabbccddeeff0011)
                                             : UINT64_C(0xeeff0011));
                  Emulator.setRegister(Source * 8,
                                       Memory ? DataAddress : Value);
                  for (unsigned F = 0; F != Flags.size(); ++F)
                    Emulator.setRegister(Flags[F], (Old >> F) & 1);
                  ASSERT_EQ(Emulator.run(Ops), Ops.size());
                  const uint64_t Result = expected(E.Id, Value, Width);
                  EXPECT_EQ(Emulator.getRegister(Destination * 8), Result);
                  const bool Decrement = E.Id == X86_INS_BLSFILL ||
                                         E.Id == X86_INS_BLSIC ||
                                         E.Id == X86_INS_TZMSK;
                  EXPECT_EQ(Emulator.getRegister(x86reg::CF),
                            Decrement ? Value == 0 : Value == mask(Width));
                  EXPECT_EQ(Emulator.getRegister(x86reg::ZF), Result == 0);
                  EXPECT_EQ(Emulator.getRegister(x86reg::SF),
                            (Result & Sign) != 0);
                  EXPECT_EQ(Emulator.getRegister(x86reg::OF), 0u);
                  EXPECT_EQ(Emulator.getRegister(x86reg::DF), (Old >> 4) & 1);
                  if (Source != Destination)
                    EXPECT_EQ(Emulator.getRegister(Source * 8),
                              Memory ? DataAddress : Value);
                  ++Cases;
                }
            }
        }
    }
  RecordProperty("value_and_flag_cases", Cases);
}

TEST(X86TBM, IgnoredXopFieldsSelectTheLowRegisterBankIn32BitMode) {
  unsigned Cases = 0;
  for (const auto &E : Encodings)
    for (bool W : {false, true})
      for (unsigned Destination : {1u, 9u})
        for (unsigned Source : {1u, 9u})
          for (unsigned Route : {0u, 1u, 2u}) {
            Decoder D;
            ASSERT_TRUE(D.init(Arch::X86));
            D.setStrict(true);
            const auto Bytes = encode(E, W, Destination, Source);
            SCOPED_TRACE(testing::PrintToString(Bytes));
            DecodedInsn Insn{};
            ASSERT_EQ(decode(D, Bytes, Route, Insn), Bytes.size());
            ASSERT_EQ(Insn.Raw->detail->x86.operands[0].reg, X86_REG_ECX);
            ASSERT_EQ(Insn.Raw->detail->x86.operands[1].reg, X86_REG_ECX);
            std::vector<LowOp> Ops;
            LowInstructionUndefinedEffects Effects;
            ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
            EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
                << Effects.Diagnostic;
            for (uint64_t Value : {UINT64_C(0), UINT64_C(0xffffffff)}) {
              auto Image = image(Arch::X86, 0, 4);
              NdOpEmulator Emulator(Image);
              Emulator.setStrictMode(true);
              Emulator.setRegister(x86reg::RCX, Value);
              Emulator.setRegister(x86reg::R9, 0x12345678);
              ASSERT_EQ(Emulator.run(Ops), Ops.size());
              EXPECT_EQ(Emulator.getRegister(x86reg::RCX),
                        expected(E.Id, Value, 4));
              EXPECT_EQ(Emulator.getRegister(x86reg::R9), 0x12345678u);
              ++Cases;
            }
          }
  RecordProperty("ignored_32_bit_xop_field_cases", Cases);
}

TEST(X86TBM, MemoryAddressesAndSegmentOverridesRetainTheirEncoding) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (const auto &E : Encodings)
      for (bool AddressOverride : {false, true})
        for (uint8_t Segment : {0, 0x64, 0x65})
          for (unsigned Shape : {0u, 1u, 2u, 3u}) {
            auto Bytes = encode(E, Target == Arch::X64, 0, 0, true);
            // [base], [base+disp8], [base+disp16/32], and SIB (or the
            // address16 [si] form). No segment or address prefix is erased.
            if (Shape == 1) {
              Bytes.back() |= 0x40;
              Bytes.push_back(0xf0);
            } else if (Shape == 2) {
              Bytes.back() |= 0x80;
              Bytes.insert(Bytes.end(), {0x34, 0x12});
              if (Target == Arch::X64 || !AddressOverride)
                Bytes.insert(Bytes.end(), {0, 0});
            } else if (Shape == 3) {
              Bytes.back() |= 4;
              if (Target == Arch::X64 || !AddressOverride)
                Bytes.push_back(0x88);
            }
            if (AddressOverride)
              Bytes.insert(Bytes.begin(), 0x67);
            if (Segment)
              Bytes.insert(Bytes.begin(), Segment);
            SCOPED_TRACE(testing::PrintToString(Bytes));
            SCOPED_TRACE(static_cast<unsigned>(Target));
            for (unsigned Route : {0u, 1u, 2u}) {
              Decoder D;
              ASSERT_TRUE(D.init(Target));
              D.setStrict(true);
              DecodedInsn Insn{};
              ASSERT_EQ(decode(D, Bytes, Route, Insn), Bytes.size());
              std::vector<LowOp> Ops;
              LowInstructionUndefinedEffects Effects;
              ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
              EXPECT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
                  << Effects.Diagnostic;
              unsigned Loads = 0;
              for (const auto &Op : Ops)
                if (Op.Opcode == NdOp::LOAD)
                  ++Loads;
              EXPECT_EQ(Loads, 1u);
              ++Cases;
            }
          }
  RecordProperty("memory_encoding_cases", Cases);
}

TEST(X86TBM, FaultingReadDoesNotPublishFlagsOrDestination) {
  for (const auto &E : Encodings) {
    Decoder D;
    ASSERT_TRUE(D.init(Arch::X64));
    auto Bytes = encode(E, true, 0, 0, true);
    DecodedInsn Insn{};
    ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
    std::vector<LowOp> Ops;
    D.liftToLow(Insn, Ops);
    auto Image = image(Arch::X64, 0, 8);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::RAX, 0xdead0000);
    for (auto Flag : Flags)
      Emulator.setRegister(Flag, 1);
    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::RAX), 0xdead0000u);
    for (auto Flag : Flags)
      EXPECT_EQ(Emulator.getRegister(Flag), 1u);
  }
}

TEST(X86TBM, InconsistentBytesOrDetailFailBeforePublishingEffects) {
  const std::vector<std::function<void(cs_insn &)>> Mutations = {
      [](auto &I) { I.bytes[0] = 0xc4; },
      [](auto &I) { I.bytes[1] ^= 1; },
      [](auto &I) { I.bytes[2] ^= 4; },
      [](auto &I) { I.bytes[2] ^= 1; },
      [](auto &I) { I.bytes[2] ^= 8; },
      [](auto &I) { I.bytes[3] ^= 3; },
      [](auto &I) {
        I.bytes[2] |= 4;
        I.detail->x86.opcode[2] |= 4;
      },
      [](auto &I) {
        I.bytes[2] |= 1;
        I.detail->x86.opcode[2] |= 1;
      },
      [](auto &I) {
        I.bytes[1] ^= 1;
        I.detail->x86.opcode[1] ^= 1;
      },
      [](auto &I) {
        I.bytes[4] ^= 8;
        I.detail->x86.modrm ^= 8;
      },
      [](auto &I) { I.bytes[4] ^= 8; },
      [](auto &I) { --I.size; },
      [](auto &I) { I.bytes[I.size++] = 0; },
      [](auto &I) { I.detail->x86.op_count = 1; },
      [](auto &I) { I.detail->x86.op_count = 3; },
      [](auto &I) { I.detail->x86.operands[0].size = 2; },
      [](auto &I) { I.detail->x86.operands[0].reg = X86_REG_RBX; },
      [](auto &I) { I.detail->x86.operands[0].access = CS_AC_READ; },
      [](auto &I) { I.detail->x86.operands[1].size = 4; },
      [](auto &I) { I.detail->x86.operands[1].reg = X86_REG_RDX; },
      [](auto &I) { I.detail->x86.operands[1].access = CS_AC_WRITE; },
      [](auto &I) { I.detail->x86.prefix[0] = 0xf0; },
      [](auto &I) { I.detail->x86.addr_size = 2; },
      [](auto &I) { I.detail->x86.rex ^= 1; },
      [](auto &I) { I.detail->x86.encoding.imm_size = 1; },
      [](auto &I) { I.detail->x86.encoding.modrm_offset = 3; },
      [](auto &I) { I.detail->x86.encoding.disp_size = 1; },
      [](auto &I) { I.detail->x86.eflags = X86_EFLAGS_RESET_CF; },
      [](auto &I) { I.detail->regs_write_count = 0; },
      [](auto &I) { I.detail->x86.avx_sae = true; },
      [](auto &I) { I.detail->x86.xop_cc = X86_XOP_CC_LT; },
      [](auto &I) { I.detail->x86.operands[0].avx_zero_opmask = true; },
      [](auto &I) { I.detail->x86.operands[1].avx_bcast = X86_AVX_BCAST_2; }};
  unsigned Cases = 0;
  for (const auto &E : Encodings)
    for (bool Strict : {false, true})
      for (unsigned Route : {0u, 1u, 2u})
        for (const auto &Mutate : Mutations) {
          Decoder D;
          ASSERT_TRUE(D.init(Arch::X64));
          D.setStrict(Strict);
          const auto Bytes = encode(E, true, 0, 1);
          DecodedInsn Insn{};
          ASSERT_EQ(decode(D, Bytes, Route, Insn), Bytes.size());
          ASSERT_NE(Insn.Raw, nullptr);
          Mutate(*Insn.Raw);
          LowOp Sentinel;
          Sentinel.Opcode = NdOp::COPY;
          Sentinel.Output = NdVar::tmp(999, 4);
          Sentinel.addInput(NdVar::scalar(42, 4));
          std::vector<LowOp> Ops{Sentinel};
          const auto Digest = lowUndefinedOperationDigest(Ops);
          EXPECT_THROW(D.liftToLow(Insn, Ops), UnliftedInstruction);
          EXPECT_EQ(lowUndefinedOperationDigest(Ops), Digest);
          ++Cases;
        }
  RecordProperty("malformed_instruction_cases", Cases);
}
} // namespace
