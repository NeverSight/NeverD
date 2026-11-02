//===- X86BMILogicTests.cpp - BMI logical forms and flags ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Compare VEX/APX logical instructions with independent bit-loop semantics.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <array>
#include <functional>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t Entry = 0x1000;
constexpr va_t Data = 0x7000;
constexpr std::array<unsigned, 5> Instructions = {
    X86_INS_ANDN, X86_INS_BLSI, X86_INS_BLSMSK, X86_INS_BLSR, X86_INS_BZHI};
constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::ZF, x86reg::SF,
                                           x86reg::OF, x86reg::AF, x86reg::PF,
                                           x86reg::DF};
enum class Form { Vex, Apx, ApxNF };

bool group(unsigned Id) { return Id != X86_INS_ANDN && Id != X86_INS_BZHI; }
uint64_t mask(unsigned Width) {
  return Width == 8 ? UINT64_MAX : UINT64_C(0xffffffff);
}
uint64_t reg(unsigned Index) {
  return Index < 16 ? Index * 8 : x86reg::extendedGeneralReg(Index - 16);
}

uint64_t expected(unsigned Id, uint64_t A, uint64_t B, unsigned Width) {
  uint64_t Result = 0;
  bool Found = false;
  for (unsigned Bit = 0; Bit != Width * 8; ++Bit) {
    const bool Set = (A >> Bit) & 1;
    const bool Output = Id == X86_INS_ANDN   ? !Set && ((B >> Bit) & 1)
                        : Id == X86_INS_BZHI ? Set && Bit < (B & 0xff)
                        : Id == X86_INS_BLSI ? Set && !Found
                        : Id == X86_INS_BLSR ? Set && Found
                                             : !Found;
    if (Output)
      Result |= UINT64_C(1) << Bit;
    Found |= Set;
  }
  return Result;
}

std::vector<uint8_t> encode(unsigned Id, Form F, unsigned Width,
                            unsigned Dst = 0, unsigned A = 1, unsigned B = 2,
                            bool Memory = false) {
  const unsigned RM = Id == X86_INS_ANDN ? B : A;
  const unsigned V = group(Id) ? Dst : Id == X86_INS_ANDN ? A : B;
  const unsigned R = group(Id) ? (Id == X86_INS_BLSR     ? 1
                                  : Id == X86_INS_BLSMSK ? 2
                                                         : 3)
                               : Dst;
  const uint8_t Opcode = group(Id) ? 0xf3 : Id == X86_INS_ANDN ? 0xf2 : 0xf5;
  const uint8_t ModRM = (Memory ? 0 : 0xc0) | ((R & 7) << 3) | (RM & 7);
  const uint8_t P1 = ((~V & 15) << 3) | (Width == 8 ? 0x80 : 0);
  if (F == Form::Vex)
    return {0xc4,
            static_cast<uint8_t>(0xe2 ^ ((R & 8) ? 0x80 : 0) ^
                                 ((RM & 8) ? 0x20 : 0)),
            P1, Opcode, ModRM};
  return {0x62,
          static_cast<uint8_t>(0xf2 ^ ((R & 8) ? 0x80 : 0) ^
                               ((R & 16) ? 0x10 : 0) ^ ((RM & 8) ? 0x20 : 0) ^
                               ((RM & 16) ? 8 : 0)),
          static_cast<uint8_t>(P1 | 4),
          static_cast<uint8_t>((V & 16 ? 0 : 8) | (F == Form::ApxNF ? 4 : 0)),
          Opcode,
          ModRM};
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
  Segment S;
  S.VA = Data;
  S.Size = Width;
  S.Flags = SegmentFlags::Readable;
  for (unsigned I = 0; I != Width; ++I)
    S.Data.push_back(Value >> (I * 8));
  Image.Segments.push_back(std::move(S));
  return Image;
}

void checkAudit(Form F, const std::vector<LowOp> &Ops,
                const LowInstructionUndefinedEffects &Effects) {
  ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic;
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  ASSERT_EQ(Effects.Effects.size(), F == Form::ApxNF ? 0 : 2);
  for (unsigned I = 0; I != Effects.Effects.size(); ++I) {
    const auto &E = Effects.Effects[I];
    EXPECT_EQ(E.Output, NdVar::reg(Flags[I + 4], 1));
    EXPECT_EQ(E.BitOffset, 0u);
    EXPECT_EQ(E.BitCount, 1u);
    EXPECT_FALSE(E.When);
    EXPECT_GT(E.AfterOp, 0u);
    EXPECT_LE(E.AfterOp, Ops.size());
  }
}

TEST(X86BMILogic, ValuesAliasesMemoryAndFlagsAcrossDecodeRoutes) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Id : Instructions)
      for (Form F : {Form::Vex, Form::Apx, Form::ApxNF}) {
        if (Target == Arch::X86 && F != Form::Vex)
          continue;
        for (unsigned Width : {4u, 8u}) {
          if (Target == Arch::X86 && Width == 8)
            continue;
          for (auto Registers :
               {std::array{0u, 1u, 2u}, std::array{1u, 1u, 2u},
                std::array{2u, 1u, 2u}, std::array{1u, 1u, 1u}})
            for (bool Memory : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                const auto [Dst, A, B] = Registers;
                const auto Bytes = encode(Id, F, Width, Dst, A, B, Memory);
                SCOPED_TRACE(testing::PrintToString(Bytes));
                Decoder D;
                ASSERT_TRUE(D.init(Target));
                DecodedInsn Insn{};
                ASSERT_EQ(decode(D, Bytes, Route, Insn), Bytes.size());
                ASSERT_EQ(Insn.Id, Id);
                std::vector<LowOp> Ops, Plain;
                LowInstructionUndefinedEffects Effects;
                ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
                ASSERT_NO_THROW(D.liftToLow(Insn, Plain));
                EXPECT_EQ(lowUndefinedOperationDigest(Ops),
                          lowUndefinedOperationDigest(Plain));
                checkAudit(F, Ops, Effects);
                for (uint64_t Value : {UINT64_C(0), UINT64_C(1), mask(Width),
                                       UINT64_C(1) << (Width * 8 - 1),
                                       UINT64_C(0xa5195ac3fe817204)})
                  for (uint64_t Control : {0u, 31u, 32u, 63u, 64u, 255u})
                    for (unsigned Old : {0u, 0x7fu}) {
                      auto Image = image(Target, Value, Width);
                      NdOpEmulator E(Image);
                      E.setStrictMode(true);
                      const uint64_t RegisterMask =
                          Target == Arch::X64 ? UINT64_MAX : mask(4);
                      E.setRegister(reg(Dst), UINT64_C(0xccbbaa9988776655) &
                                                  RegisterMask);
                      E.setRegister(reg(A), Value & RegisterMask);
                      E.setRegister(reg(B),
                                    (Id == X86_INS_ANDN ? ~Value : Control) &
                                        RegisterMask);
                      if (Memory)
                        E.setRegister(reg(Id == X86_INS_ANDN ? B : A), Data);
                      const uint64_t AV = (Memory && Id != X86_INS_ANDN
                                               ? Value
                                               : *E.getRegister(reg(A))) &
                                          mask(Width);
                      const uint64_t BV = (Memory && Id == X86_INS_ANDN
                                               ? Value
                                               : *E.getRegister(reg(B))) &
                                          mask(Width);
                      for (unsigned I = 0; I != Flags.size(); ++I)
                        E.setRegister(Flags[I], (Old >> I) & 1);
                      ASSERT_EQ(E.run(Ops), Ops.size());
                      const uint64_t Result = expected(Id, AV, BV, Width);
                      EXPECT_EQ(E.getRegister(reg(Dst)), Result);
                      const bool CF = Id == X86_INS_ANDN ? false
                                      : Id == X86_INS_BZHI
                                          ? (BV & 0xff) >= Width * 8
                                      : Id == X86_INS_BLSI ? AV != 0
                                                           : AV == 0;
                      for (unsigned I = 0; I != Flags.size(); ++I) {
                        const uint64_t Flag =
                            F == Form::ApxNF || I == 6 ? (Old >> I) & 1
                            : I == 0                   ? CF
                            : I == 1                   ? Result == 0
                            : I == 2 ? Result >> (Width * 8 - 1)
                                     : 0;
                        EXPECT_EQ(E.getRegister(Flags[I]), Flag);
                      }
                      ++Cases;
                    }
              }
        }
      }
  RecordProperty("value_flag_cases", Cases);
}

TEST(X86BMILogic, EveryLowWordAndIndexMatchesTheBitLoopReference) {
  unsigned Cases = 0;
  for (unsigned Id : {X86_INS_BLSI, X86_INS_BLSMSK, X86_INS_BLSR, X86_INS_BZHI})
    for (Form F : {Form::Vex, Form::Apx, Form::ApxNF})
      for (unsigned Width : {4u, 8u}) {
        Decoder D;
        ASSERT_TRUE(D.init(Arch::X64));
        const auto Bytes = encode(Id, F, Width);
        DecodedInsn Insn{};
        ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
        auto Image = image(Arch::X64, 0, Width);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        for (unsigned Input = 0; Input != 0x10000; ++Input) {
          const uint64_t A = Id == X86_INS_BZHI
                                 ? UINT64_C(0xfedcba9876543210) & mask(Width)
                                 : Input;
          const uint64_t B = UINT64_C(0xabcdef0000000000) | Input;
          E.setRegister(x86reg::RCX, A);
          E.setRegister(x86reg::RDX, B);
          ASSERT_EQ(E.run(Ops), Ops.size());
          ASSERT_EQ(E.getRegister(x86reg::RAX), expected(Id, A, B, Width))
              << Input;
          ++Cases;
        }
      }
  RecordProperty("exhaustive_value_cases", Cases);
}

TEST(X86BMILogic, EveryRegisterBankAndWidthHasACompleteAudit) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Id : Instructions)
      for (Form F : {Form::Vex, Form::Apx, Form::ApxNF}) {
        if (Target == Arch::X86 && F != Form::Vex)
          continue;
        const unsigned Count = Target == Arch::X86 ? 8
                               : F == Form::Vex    ? 16
                                                   : 32;
        Decoder D;
        ASSERT_TRUE(D.init(Target));
        for (unsigned Width : {4u, 8u}) {
          if (Target == Arch::X86 && Width == 8)
            continue;
          for (unsigned Dst = 0; Dst != Count; ++Dst)
            for (unsigned A = 0; A != Count; ++A)
              for (unsigned B : {0u, Count - 1}) {
                const auto Bytes = encode(Id, F, Width, Dst, A, B);
                DecodedInsn Insn{};
                ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
                std::vector<LowOp> Ops;
                LowInstructionUndefinedEffects Effects;
                ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
                checkAudit(F, Ops, Effects);
                ++Cases;
              }
        }
      }
  RecordProperty("register_form_cases", Cases);
}

TEST(X86BMILogic, I386IgnoredWBaseAndVFieldsKeepTheLowRegisterBank) {
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (bool W : {false, true})
      for (bool B : {false, true})
        for (bool V : {false, true}) {
          auto Bytes = encode(Id, Form::Vex, W ? 8 : 4);
          if (B)
            Bytes[1] ^= 0x20;
          if (V)
            Bytes[2] ^= 0x40;
          Decoder D;
          ASSERT_TRUE(D.init(Arch::X86));
          DecodedInsn Insn{};
          ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
          std::vector<LowOp> Ops;
          LowInstructionUndefinedEffects Effects;
          ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
          checkAudit(Form::Vex, Ops, Effects);
          auto Image = image(Arch::X86, 0, 4);
          NdOpEmulator E(Image);
          E.setStrictMode(true);
          E.setRegister(x86reg::RCX, 0xfedcba98);
          E.setRegister(x86reg::RDX, 9);
          E.setRegister(x86reg::R9, 0x12345678);
          ASSERT_EQ(E.run(Ops), Ops.size());
          EXPECT_EQ(E.getRegister(x86reg::RAX), expected(Id, 0xfedcba98, 9, 4));
          EXPECT_EQ(E.getRegister(x86reg::R9), 0x12345678u);
          ++Cases;
        }
  RecordProperty("ignored_i386_field_cases", Cases);
}

TEST(X86BMILogic, FaultingMemoryPublishesNoDestinationOrFlags) {
  for (unsigned Id : Instructions)
    for (Form F : {Form::Vex, Form::Apx, Form::ApxNF}) {
      Decoder D;
      ASSERT_TRUE(D.init(Arch::X64));
      const auto Bytes = encode(Id, F, 8, 1, 1, 1, true);
      DecodedInsn Insn{};
      ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
      auto Image = image(Arch::X64, 0, 8);
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setRegister(x86reg::RCX, 0xdead0000);
      for (unsigned Flag : Flags)
        E.setRegister(Flag, 1);
      EXPECT_LT(E.run(Ops), Ops.size());
      EXPECT_EQ(E.getRegister(x86reg::RCX), 0xdead0000u);
      for (unsigned Flag : Flags)
        EXPECT_EQ(E.getRegister(Flag), 1u);
    }
}

TEST(X86BMILogic, MemoryTailsKeepDisplacementsSegmentsAndAddressSizes) {
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (Arch Target : {Arch::X86, Arch::X64})
      for (Form F : {Form::Vex, Form::Apx, Form::ApxNF}) {
        if (Target == Arch::X86 && (F == Form::Apx || F == Form::ApxNF))
          continue;
        for (bool AddressOverride : {false, true})
          for (uint8_t Segment : {0, 0x64, 0x65})
            for (unsigned Shape : {0u, 1u, 2u, 3u, 4u}) {
              auto Bytes =
                  encode(Id, F, Target == Arch::X64 ? 8 : 4, 0, 0, 0, true);
              const unsigned ModRM = F == Form::Apx || F == Form::ApxNF ? 5 : 4;
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
                checkAudit(F, Ops, Effects);
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

TEST(X86BMILogic, InconsistentBytesOrDetailFailWithoutPartialEffects) {
  const std::vector<std::function<void(cs_insn &)>> Mutations = {
      [](auto &I) { I.bytes[0] ^= 1; },
      [](auto &I) {
        I.bytes[2] |= 1;
        I.detail->x86.opcode[2] |= 1;
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
      [](auto &I) { I.detail->x86.op_count = 1; },
      [](auto &I) { I.detail->x86.op_count = 4; },
      [](auto &I) { I.detail->x86.operands[0].size = 2; },
      [](auto &I) { I.detail->x86.operands[0].reg = X86_REG_RBX; },
      [](auto &I) { I.detail->x86.operands[0].access = CS_AC_READ; },
      [](auto &I) { I.detail->x86.operands[1].size = 4; },
      [](auto &I) { I.detail->x86.operands[1].reg = X86_REG_RDX; },
      [](auto &I) { I.detail->x86.operands[1].access = CS_AC_WRITE; },
      [](auto &I) { I.detail->x86.prefix[0] = 0xf0; },
      [](auto &I) { I.detail->x86.addr_size = 2; },
      [](auto &I) { I.detail->x86.rex ^= 1; },
      [](auto &I) { I.detail->x86.rex2 ^= 1; },
      [](auto &I) { I.detail->x86.encoding.imm_size = 1; },
      [](auto &I) { I.detail->x86.encoding.modrm_offset = 1; },
      [](auto &I) { I.detail->x86.encoding.disp_size = 1; },
      [](auto &I) { I.detail->x86.eflags ^= X86_EFLAGS_RESET_CF; },
      [](auto &I) { I.detail->regs_read_count = 1; },
      [](auto &I) { I.detail->regs_write_count ^= 1; },
      [](auto &I) { I.detail->x86.avx_sae = true; },
      [](auto &I) { I.detail->x86.xop_cc = X86_XOP_CC_LT; },
      [](auto &I) { I.detail->x86.operands[0].avx_zero_opmask = true; },
      [](auto &I) { I.detail->x86.operands[1].avx_bcast = X86_AVX_BCAST_2; }};
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (Form F : {Form::Vex, Form::Apx, Form::ApxNF})
      for (bool Strict : {false, true})
        for (unsigned Route : {0u, 1u, 2u})
          for (const auto &Mutate : Mutations) {
            Decoder D;
            ASSERT_TRUE(D.init(Arch::X64));
            D.setStrict(Strict);
            const auto Bytes = encode(Id, F, 8);
            DecodedInsn Insn{};
            ASSERT_EQ(decode(D, Bytes, Route, Insn), Bytes.size());
            Mutate(*Insn.Raw);
            LowOp Sentinel;
            Sentinel.Opcode = NdOp::NOP;
            std::vector<LowOp> Ops{Sentinel};
            const auto Digest = lowUndefinedOperationDigest(Ops);
            EXPECT_THROW(D.liftToLow(Insn, Ops), UnliftedInstruction);
            EXPECT_EQ(lowUndefinedOperationDigest(Ops), Digest);
            ++Cases;
          }
  RecordProperty("malformed_cases", Cases);
}

TEST(X86BMILogic, IgnoredGroupExtensionsDoNotSelectAnotherRegister) {
  for (unsigned Id : {X86_INS_BLSI, X86_INS_BLSMSK, X86_INS_BLSR})
    for (Form F : {Form::Vex, Form::Apx, Form::ApxNF})
      for (unsigned Bits : {0u, 0x80u, 0x10u, 0x90u}) {
        if (F == Form::Vex && (Bits & 0x10))
          continue; // VEX has no R4 extension.
        auto Bytes = encode(Id, F, 8);
        Bytes[1] ^= Bits;
        Decoder D;
        ASSERT_TRUE(D.init(Arch::X64));
        DecodedInsn Insn{};
        ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
        ASSERT_EQ(Insn.Id, Id);
        ASSERT_EQ(Insn.Raw->detail->x86.operands[0].reg, X86_REG_RAX);
        std::vector<LowOp> Ops;
        LowInstructionUndefinedEffects Effects;
        ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
        checkAudit(F, Ops, Effects);
      }
}

TEST(X86BMILogic, ReservedApxFieldsCannotBeAuthorizedByMatchingDetail) {
  for (unsigned Id : Instructions)
    for (Form F : {Form::Apx, Form::ApxNF})
      for (auto Field :
           {std::array{1u, 1u}, std::array{2u, 1u}, std::array{2u, 2u},
            std::array{2u, 4u}, std::array{3u, 1u}, std::array{3u, 2u},
            std::array{3u, 16u}, std::array{3u, 32u}, std::array{3u, 64u},
            std::array{3u, 128u}}) {
        Decoder D;
        ASSERT_TRUE(D.init(Arch::X64));
        const auto Bytes = encode(Id, F, 8);
        DecodedInsn Insn{};
        ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
        const auto [Byte, Bit] = Field;
        Insn.Raw->bytes[Byte] ^= Bit;
        Insn.Raw->detail->x86.opcode[Byte] ^= Bit;
        std::vector<LowOp> Ops;
        EXPECT_THROW(D.liftToLow(Insn, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
      }
}

} // namespace
