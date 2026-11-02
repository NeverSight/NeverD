//===- X86BMI2Tests.cpp - BMI2 encoding and value semantics --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check BMI2 with independent bit loops and full-width multiplication.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/APInt.h"

#include <array>
#include <functional>
#include <random>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t Entry = 0x1000;
constexpr va_t Data = 0x7000;
constexpr std::array<unsigned, 7> Instructions = {
    X86_INS_MULX, X86_INS_PDEP, X86_INS_PEXT, X86_INS_RORX,
    X86_INS_SARX, X86_INS_SHLX, X86_INS_SHRX};
constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::OF,
                                           x86reg::DF};
enum class Form { Vex, Apx };
uint64_t mask(unsigned Width) {
  return Width == 8 ? UINT64_MAX : UINT64_C(0xffffffff);
}
uint64_t reg(unsigned Index) {
  return Index < 16 ? Index * 8 : x86reg::extendedGeneralReg(Index - 16);
}

// R/V/B are encoded fields, not architectural operand positions.
std::vector<uint8_t> encode(unsigned Id, Form F, unsigned Width, unsigned R = 0,
                            unsigned V = 1, unsigned B = 3, bool Memory = false,
                            unsigned Immediate = 0xab) {
  const unsigned PP = Id == X86_INS_SHLX                         ? 1
                      : Id == X86_INS_SARX || Id == X86_INS_PEXT ? 2
                                                                 : 3;
  const bool Rotate = Id == X86_INS_RORX;
  const unsigned Map = Rotate ? 3 : 2;
  if (Rotate)
    V = 0;
  const uint8_t Opcode = Id == X86_INS_MULX                         ? 0xf6
                         : Id == X86_INS_PDEP || Id == X86_INS_PEXT ? 0xf5
                         : Rotate                                   ? 0xf0
                                                                    : 0xf7;
  const uint8_t ModRM = (Memory ? 0 : 0xc0) | ((R & 7) << 3) | (B & 7);
  const uint8_t P1 = ((~V & 15) << 3) | (Width == 8 ? 0x80 : 0) | PP;
  std::vector<uint8_t> Bytes;
  if (F == Form::Vex)
    Bytes = {0xc4,
             static_cast<uint8_t>((0xe0 | Map) ^ ((R & 8) ? 0x80 : 0) ^
                                  ((B & 8) ? 0x20 : 0)),
             P1, Opcode, ModRM};
  else
    Bytes = {0x62,
             static_cast<uint8_t>((0xf0 | Map) ^ ((R & 8) ? 0x80 : 0) ^
                                  ((R & 16) ? 0x10 : 0) ^ ((B & 8) ? 0x20 : 0) ^
                                  ((B & 16) ? 8 : 0)),
             static_cast<uint8_t>(P1 | 4),
             static_cast<uint8_t>(V & 16 ? 0 : 8),
             Opcode,
             ModRM};
  if (Rotate)
    Bytes.push_back(Immediate);
  return Bytes;
}

int decode(Decoder &D, const std::vector<uint8_t> &Bytes, unsigned Route,
           DecodedInsn &I) {
  return Route == 0 ? D.decodeOne(Bytes.data(), Bytes.size(), Entry, I)
         : Route == 1
             ? D.decodeOneLight(Bytes.data(), Bytes.size(), Entry, I)
             : D.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, I);
}
BinaryImage image(Arch Target, uint64_t Value, unsigned Width) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Bits = Target == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Image.Format = BinaryFormat::ELF;
  Segment S;
  S.VA = Data;
  S.Size = Width;
  S.Flags = SegmentFlags::Readable;
  for (unsigned I = 0; I != Width; ++I)
    S.Data.push_back(Value >> (I * 8));
  Image.Segments.push_back(std::move(S));
  return Image;
}

std::pair<uint64_t, uint64_t> product(uint64_t A, uint64_t B, unsigned Width) {
  const auto P =
      llvm::APInt(128, A & mask(Width)) * llvm::APInt(128, B & mask(Width));
  return {P.trunc(Width * 8).getZExtValue(), P.lshr(Width * 8).getZExtValue()};
}
uint64_t expected(unsigned Id, uint64_t A, uint64_t B, unsigned Width) {
  const unsigned Bits = Width * 8;
  const unsigned Count = B % Bits;
  unsigned PackedBit = 0;
  uint64_t Result = 0;
  for (unsigned Bit = 0; Bit != Bits; ++Bit) {
    if (Id == X86_INS_PDEP || Id == X86_INS_PEXT) {
      if (!(B & (UINT64_C(1) << Bit)))
        continue;
      const unsigned Source = Id == X86_INS_PDEP ? PackedBit : Bit;
      const unsigned Destination = Id == X86_INS_PDEP ? Bit : PackedBit;
      Result |= ((A >> Source) & 1) << Destination;
      ++PackedBit;
    } else {
      const int Source = Id == X86_INS_SHLX
                             ? static_cast<int>(Bit) - static_cast<int>(Count)
                             : Bit + Count;
      const bool Set = Id == X86_INS_RORX ? (A >> (Source % Bits)) & 1
                       : Source < 0       ? false
                       : static_cast<unsigned>(Source) >= Bits
                           ? Id == X86_INS_SARX && ((A >> (Bits - 1)) & 1)
                           : (A >> Source) & 1;
      if (Set)
        Result |= UINT64_C(1) << Bit;
    }
  }
  return Result;
}
void checkAudit(const std::vector<LowOp> &Ops,
                const LowInstructionUndefinedEffects &E) {
  EXPECT_EQ(E.Coverage, LowUndefinedCoverage::Complete) << E.Diagnostic;
  EXPECT_TRUE(E.Effects.empty());
  EXPECT_EQ(E.OperationDigest, lowUndefinedOperationDigest(Ops));
}

TEST(X86BMI2, ValuesAliasesMemoryAndEveryFlagAcrossDecodeRoutes) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (Form F : {Form::Vex, Form::Apx}) {
      if (Target == Arch::X86 && F == Form::Apx)
        continue;
      for (unsigned Width : {4u, 8u}) {
        if (Target == Arch::X86 && Width == 8)
          continue;
        for (unsigned Id : Instructions)
          for (auto Registers :
               {std::array{0u, 1u, 3u}, std::array{1u, 1u, 3u},
                std::array{3u, 1u, 3u}, std::array{0u, 3u, 3u},
                std::array{2u, 2u, 2u}, std::array{0u, 2u, 3u}})
            for (bool Memory : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                auto [R, V, B] = Registers;
                const auto Bytes = encode(Id, F, Width, R, V, B, Memory);
                SCOPED_TRACE(testing::PrintToString(Bytes));
                Decoder D;
                ASSERT_TRUE(D.init(Target));
                DecodedInsn I{};
                ASSERT_EQ(decode(D, Bytes, Route, I), Bytes.size());
                ASSERT_EQ(I.Id, Id);
                std::vector<LowOp> Ops, Plain;
                LowInstructionUndefinedEffects Effects;
                ASSERT_NO_THROW(D.liftToLow(I, Ops, {}, {}, &Effects));
                ASSERT_NO_THROW(D.liftToLow(I, Plain));
                EXPECT_EQ(lowUndefinedOperationDigest(Ops),
                          lowUndefinedOperationDigest(Plain));
                checkAudit(Ops, Effects);
                for (uint64_t A : {UINT64_C(0), UINT64_C(1), mask(Width),
                                   UINT64_C(1) << (Width * 8 - 1),
                                   UINT64_C(0xfedcba9876543210)})
                  for (uint64_t C : {UINT64_C(0), UINT64_C(31), UINT64_C(64),
                                     UINT64_C(0xf519ac80ff803f20)})
                    for (unsigned Old : {0u, 0x7fu, 0x55u}) {
                      auto Image = image(Target, C, Width);
                      NdOpEmulator E(Image);
                      E.setStrictMode(true);
                      const auto RegisterMask =
                          Target == Arch::X64 ? UINT64_MAX : mask(4);
                      E.setRegister(reg(R), UINT64_C(0xaabbccddeeff1020) &
                                                RegisterMask);
                      E.setRegister(reg(V), A & RegisterMask);
                      E.setRegister(reg(B), C & RegisterMask);
                      E.setRegister(x86reg::RDX, A & RegisterMask);
                      if (Memory)
                        E.setRegister(reg(B), Data);
                      const auto VV = *E.getRegister(reg(V)) & mask(Width);
                      const auto RM =
                          (Memory ? C : *E.getRegister(reg(B))) & mask(Width);
                      const auto DX = *E.getRegister(x86reg::RDX) & mask(Width);
                      for (unsigned J = 0; J != Flags.size(); ++J)
                        E.setRegister(Flags[J], (Old >> J) & 1);
                      ASSERT_EQ(E.run(Ops), Ops.size());
                      if (Id == X86_INS_MULX) {
                        const auto [Lo, Hi] = product(DX, RM, Width);
                        EXPECT_EQ(E.getRegister(reg(R)), Hi);
                        EXPECT_EQ(E.getRegister(reg(V)), R == V ? Hi : Lo);
                      } else {
                        const bool Scatter =
                            Id == X86_INS_PDEP || Id == X86_INS_PEXT;
                        EXPECT_EQ(E.getRegister(reg(R)),
                                  expected(Id, Scatter ? VV : RM,
                                           Scatter              ? RM
                                           : Id == X86_INS_RORX ? 0xab
                                                                : VV,
                                           Width));
                      }
                      for (unsigned J = 0; J != Flags.size(); ++J)
                        EXPECT_EQ(E.getRegister(Flags[J]), (Old >> J) & 1);
                      EXPECT_FALSE(E.skips().any());
                      ++Cases;
                    }
              }
      }
    }
  RecordProperty("value_flag_cases", Cases);
}

TEST(X86BMI2, EveryLowMaskAndCountAndFullWidthProducts) {
  unsigned Cases = 0;
  for (Form F : {Form::Vex, Form::Apx})
    for (unsigned Width : {4u, 8u})
      for (unsigned Id : Instructions) {
        Decoder D;
        ASSERT_TRUE(D.init(Arch::X64));
        auto Image = image(Arch::X64, 0, Width);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        const auto Bytes = encode(Id, F, Width);
        DecodedInsn I{};
        ASSERT_EQ(decode(D, Bytes, 2, I), Bytes.size());
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
        std::mt19937_64 Random(0x103a89c4);
        for (unsigned Value = 0; Value != (Id == X86_INS_RORX ? 256u : 65536u);
             ++Value) {
          uint64_t A = Random(), B = Value;
          if (Id == X86_INS_MULX)
            B = Random();
          if (Id == X86_INS_RORX) {
            const auto RotateBytes =
                encode(Id, F, Width, 0, 1, 3, false, Value);
            ASSERT_EQ(decode(D, RotateBytes, 2, I), RotateBytes.size());
            Ops.clear();
            ASSERT_NO_THROW(D.liftToLow(I, Ops));
          }
          const bool Scatter = Id == X86_INS_PDEP || Id == X86_INS_PEXT;
          E.setRegister(x86reg::RCX, Scatter ? A : B);
          E.setRegister(x86reg::RBX, Scatter || Id == X86_INS_MULX ? B : A);
          E.setRegister(x86reg::RDX, A);
          ASSERT_EQ(E.run(Ops), Ops.size());
          if (Id == X86_INS_MULX) {
            const auto [Lo, Hi] = product(A, B, Width);
            ASSERT_EQ(E.getRegister(x86reg::RAX), Hi);
            ASSERT_EQ(E.getRegister(x86reg::RCX), Lo);
          } else
            ASSERT_EQ(E.getRegister(x86reg::RAX), expected(Id, A, B, Width));
          ++Cases;
        }
      }
  RecordProperty("exhaustive_and_random_cases", Cases);
}

TEST(X86BMI2, EveryRegisterFieldBankAndWidthHasCompleteAudit) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (Form F : {Form::Vex, Form::Apx}) {
      if (Target == Arch::X86 && F == Form::Apx)
        continue;
      const unsigned Count = Target == Arch::X86 ? 8 : F == Form::Vex ? 16 : 32;
      Decoder D;
      ASSERT_TRUE(D.init(Target));
      for (unsigned Width : {4u, 8u}) {
        if (Target == Arch::X86 && Width == 8)
          continue;
        for (unsigned Id : Instructions)
          for (unsigned R = 0; R != Count; ++R)
            for (unsigned B = 0; B != Count; ++B)
              for (unsigned V : {0u, Count - 1}) {
                const auto Bytes = encode(Id, F, Width, R, V, B);
                DecodedInsn I{};
                ASSERT_EQ(decode(D, Bytes, 2, I), Bytes.size());
                std::vector<LowOp> Ops;
                LowInstructionUndefinedEffects Effects;
                ASSERT_NO_THROW(D.liftToLow(I, Ops, {}, {}, &Effects));
                checkAudit(Ops, Effects);
                ++Cases;
              }
      }
    }
  RecordProperty("register_form_cases", Cases);
}

TEST(X86BMI2, I386IgnoredWBaseAndUsedVFieldsKeepLowRegisters) {
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (bool W : {false, true})
      for (bool B : {false, true})
        for (bool V : {false, true}) {
          if (Id == X86_INS_RORX && V)
            continue; // Unused vvvv is reserved, not ignored.
          auto Bytes = encode(Id, Form::Vex, W ? 8 : 4);
          if (B)
            Bytes[1] ^= 0x20;
          if (V)
            Bytes[2] ^= 0x40;
          Decoder D;
          ASSERT_TRUE(D.init(Arch::X86));
          DecodedInsn I{};
          ASSERT_EQ(decode(D, Bytes, 2, I), Bytes.size());
          std::vector<LowOp> Ops;
          LowInstructionUndefinedEffects Effects;
          ASSERT_NO_THROW(D.liftToLow(I, Ops, {}, {}, &Effects));
          checkAudit(Ops, Effects);
          auto Image = image(Arch::X86, 0, 4);
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setRegister(x86reg::RCX, 0xaabbddee);
          E.setRegister(x86reg::RBX, 0x9f802344);
          E.setRegister(x86reg::RDX, 0xfedcba98);
          E.setRegister(x86reg::R9, 0x12345678);
          ASSERT_EQ(E.run(Ops), Ops.size());
          if (Id == X86_INS_MULX) {
            const auto [Lo, Hi] = product(0xfedcba98, 0x9f802344, 4);
            EXPECT_EQ(E.getRegister(x86reg::RAX), Hi);
            EXPECT_EQ(E.getRegister(x86reg::RCX), Lo);
          } else {
            const bool Scatter = Id == X86_INS_PDEP || Id == X86_INS_PEXT;
            EXPECT_EQ(E.getRegister(x86reg::RAX),
                      expected(Id, Scatter ? 0xaabbddee : 0x9f802344,
                               Scatter              ? 0x9f802344
                               : Id == X86_INS_RORX ? 0xab
                                                    : 0xaabbddee,
                               4));
          }
          EXPECT_EQ(E.getRegister(x86reg::R9), 0x12345678u);
          ++Cases;
        }
  RecordProperty("ignored_i386_field_cases", Cases);
}

TEST(X86BMI2, FaultingMemoryPreservesBothDestinationsAndFlags) {
  for (unsigned Id : Instructions)
    for (Form F : {Form::Vex, Form::Apx}) {
      Decoder D;
      ASSERT_TRUE(D.init(Arch::X64));
      const auto Bytes = encode(Id, F, 8, 0, 2, 1, true);
      DecodedInsn I{};
      ASSERT_EQ(decode(D, Bytes, 2, I), Bytes.size());
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(I, Ops));
      auto Image = image(Arch::X64, 0, 8);
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setRegister(x86reg::RAX, 0x1234);
      E.setRegister(x86reg::RDX, 0x5678);
      E.setRegister(x86reg::RCX, 0xdead0000);
      for (unsigned Flag : Flags)
        E.setRegister(Flag, 1);
      EXPECT_LT(E.run(Ops), Ops.size());
      EXPECT_EQ(E.getRegister(x86reg::RAX), 0x1234u);
      EXPECT_EQ(E.getRegister(x86reg::RDX), 0x5678u);
      for (unsigned Flag : Flags)
        EXPECT_EQ(E.getRegister(Flag), 1u);
    }
}

TEST(X86BMI2, MemoryTailsKeepDisplacementsSegmentsAndAddressSizes) {
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (Arch Target : {Arch::X86, Arch::X64})
      for (Form F : {Form::Vex, Form::Apx}) {
        if (Target == Arch::X86 && F == Form::Apx)
          continue;
        for (bool AddressOverride : {false, true})
          for (uint8_t Segment : {0, 0x64, 0x65})
            for (unsigned Shape : {0u, 1u, 2u, 3u, 4u}) {
              auto Bytes =
                  encode(Id, F, Target == Arch::X64 ? 8 : 4, 0, 0, 0, true);
              const unsigned ModRM = F == Form::Apx ? 5 : 4;
              const bool Address16 = Target == Arch::X86 && AddressOverride;
              std::vector<uint8_t> Tail;
              if (Shape == 1) {
                Bytes[ModRM] |= 0x40;
                Tail = {0xf0};
              } else if (Shape == 2) {
                Bytes[ModRM] |= 0x80;
                Tail = {0x34, 0x12};
                if (!Address16)
                  Tail.insert(Tail.end(), {0, 0});
              } else if (Shape == 3) {
                Bytes[ModRM] |= 4;
                if (!Address16)
                  Tail = {0x88};
              } else if (Shape == 4) {
                Bytes[ModRM] |= Address16 ? 6 : 5;
                Tail = {0x34, 0x12};
                if (!Address16)
                  Tail.insert(Tail.end(), {0, 0});
              }
              Bytes.insert(Bytes.begin() + ModRM + 1, Tail.begin(), Tail.end());
              if (AddressOverride)
                Bytes.insert(Bytes.begin(), 0x67);
              if (Segment)
                Bytes.insert(Bytes.begin(), Segment);
              SCOPED_TRACE(testing::PrintToString(Bytes));
              SCOPED_TRACE(static_cast<unsigned>(Target));
              for (unsigned Route : {0u, 1u, 2u}) {
                Decoder D;
                ASSERT_TRUE(D.init(Target));
                DecodedInsn Insn{};
                ASSERT_EQ(decode(D, Bytes, Route, Insn), Bytes.size());
                std::vector<LowOp> Ops;
                LowInstructionUndefinedEffects Effects;
                ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
                checkAudit(Ops, Effects);
                unsigned Loads = 0;
                for (const auto &Op : Ops)
                  if (Op.Opcode == NdOp::LOAD)
                    ++Loads;
                EXPECT_EQ(Loads, 1u);
                ++Cases;
              }
            }
      }
  RecordProperty("memory_tail_cases", Cases);
}

TEST(X86BMI2, InconsistentBytesOrDetailFailWithoutPartialEffects) {
  const std::vector<std::function<void(cs_insn &)>> Mutations = {
      [](auto &I) { I.bytes[0] ^= 1; },
      [](auto &I) {
        I.bytes[2] ^= 1;
        I.detail->x86.opcode[2] ^= 1;
      },
      [](auto &I) {
        const unsigned Byte = I.bytes[0] == 0x62 ? 3 : 2;
        const uint8_t Bit = I.bytes[0] == 0x62 ? 0x10 : 4;
        I.bytes[Byte] |= Bit;
        I.detail->x86.opcode[Byte] |= Bit;
      },
      [](auto &I) { I.bytes[I.detail->x86.encoding.modrm_offset - 1] ^= 1; },
      [](auto &I) { I.bytes[I.detail->x86.encoding.modrm_offset] ^= 8; },
      [](auto &I) { --I.size; },
      [](auto &I) { I.bytes[I.size++] = 0; },
      [](auto &I) { I.detail->x86.op_count = 2; },
      [](auto &I) { I.detail->x86.op_count = 4; },
      [](auto &I) { I.detail->x86.operands[0].size = 2; },
      [](auto &I) { I.detail->x86.operands[0].reg = X86_REG_RBX; },
      [](auto &I) { I.detail->x86.operands[0].access = CS_AC_READ; },
      [](auto &I) { I.detail->x86.operands[1].size = 4; },
      [](auto &I) { I.detail->x86.operands[1].reg = X86_REG_RDX; },
      [](auto &I) {
        auto &Access = I.detail->x86.operands[1].access;
        Access = Access == CS_AC_READ ? CS_AC_WRITE : CS_AC_READ;
      },
      [](auto &I) { I.detail->x86.operands[2].size ^= 16; },
      [](auto &I) { I.detail->x86.prefix[0] = 0xf0; },
      [](auto &I) { I.detail->x86.addr_size = 2; },
      [](auto &I) { I.detail->x86.rex ^= 1; },
      [](auto &I) { I.detail->x86.rex2 ^= 1; },
      [](auto &I) { I.detail->x86.encoding.imm_size ^= 1; },
      [](auto &I) { I.detail->x86.encoding.modrm_offset = 1; },
      [](auto &I) { I.detail->x86.encoding.disp_size = 1; },
      [](auto &I) { I.detail->x86.eflags ^= X86_EFLAGS_RESET_CF; },
      [](auto &I) { I.detail->regs_read_count ^= 1; },
      [](auto &I) { I.detail->regs_write_count = 1; },
      [](auto &I) { I.detail->x86.avx_sae = true; },
      [](auto &I) { I.detail->x86.xop_cc = X86_XOP_CC_LT; },
      [](auto &I) { I.detail->x86.operands[0].avx_zero_opmask = true; },
      [](auto &I) { I.detail->x86.operands[1].avx_bcast = X86_AVX_BCAST_2; }};
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (Form F : {Form::Vex, Form::Apx})
      for (bool Strict : {false, true})
        for (unsigned Route : {0u, 1u, 2u})
          for (const auto &Mutate : Mutations) {
            Decoder D;
            ASSERT_TRUE(D.init(Arch::X64));
            D.setStrict(Strict);
            const auto Bytes = encode(Id, F, 8);
            DecodedInsn I{};
            ASSERT_EQ(decode(D, Bytes, Route, I), Bytes.size());
            Mutate(*I.Raw);
            LowOp Sentinel;
            Sentinel.Opcode = NdOp::NOP;
            std::vector<LowOp> Ops{Sentinel};
            const auto Digest = lowUndefinedOperationDigest(Ops);
            EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
            EXPECT_EQ(lowUndefinedOperationDigest(Ops), Digest);
            ++Cases;
          }
  RecordProperty("malformed_cases", Cases);
}

TEST(X86BMI2, ReservedApxFlagsAndUnusedRotateFieldsFailClosed) {
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (auto Field :
         {std::array{2u, 4u}, std::array{3u, 1u}, std::array{3u, 2u},
          std::array{3u, 4u}, std::array{3u, 16u}, std::array{3u, 32u},
          std::array{3u, 64u}, std::array{3u, 128u}}) {
      Decoder D;
      ASSERT_TRUE(D.init(Arch::X64));
      const auto Bytes = encode(Id, Form::Apx, 8);
      DecodedInsn I{};
      ASSERT_EQ(decode(D, Bytes, 2, I), Bytes.size());
      const auto [Byte, Bit] = Field;
      I.Raw->bytes[Byte] ^= Bit;
      I.Raw->detail->x86.opcode[Byte] ^= Bit;
      std::vector<LowOp> Ops;
      EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
      ++Cases;
    }
  for (Arch Target : {Arch::X86, Arch::X64})
    for (Form F : {Form::Vex, Form::Apx}) {
      if (Target == Arch::X86 && F == Form::Apx)
        continue;
      for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
        if (F == Form::Vex && Mutation == 4)
          continue;
        Decoder D;
        ASSERT_TRUE(D.init(Target));
        const auto Bytes = encode(X86_INS_RORX, F, 4);
        DecodedInsn I{};
        ASSERT_EQ(decode(D, Bytes, 2, I), Bytes.size());
        if (Mutation == 5)
          I.Raw->detail->x86.operands[2].imm += 256;
        else {
          const unsigned Byte = Mutation == 4 ? 3 : 2;
          const unsigned Bit = Mutation == 4 ? 8 : 8 << Mutation;
          I.Raw->bytes[Byte] ^= Bit;
          I.Raw->detail->x86.opcode[Byte] ^= Bit;
        }
        std::vector<LowOp> Ops;
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
        ++Cases;
      }
    }
  RecordProperty("reserved_field_cases", Cases);
}

} // namespace
