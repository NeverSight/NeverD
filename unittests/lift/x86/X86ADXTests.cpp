//===- X86ADXTests.cpp - ADCX/ADOX encoding and carry semantics -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Verify independent carry chains, aliases, fault ordering and admission.
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
constexpr std::array<unsigned, 2> Instructions = {X86_INS_ADCX, X86_INS_ADOX};
constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::PF, x86reg::AF,
                                           x86reg::ZF, x86reg::SF, x86reg::OF,
                                           x86reg::DF};
enum class Form { Legacy, Apx, Ndd };
uint64_t mask(unsigned Width) {
  return Width == 8 ? UINT64_MAX : UINT64_C(0xffffffff);
}
uint64_t reg(unsigned I) {
  return I < 16 ? I * 8 : x86reg::extendedGeneralReg(I - 16);
}
std::vector<uint8_t> encode(unsigned Id, Form F, unsigned Width, unsigned R = 0,
                            unsigned V = 2, unsigned B = 1,
                            bool Memory = false) {
  const uint8_t ModRM = (Memory ? 0 : 0xc0) | ((R & 7) << 3) | (B & 7);
  if (F == Form::Legacy) {
    std::vector<uint8_t> Bytes = {
        static_cast<uint8_t>(Id == X86_INS_ADCX ? 0x66 : 0xf3)};
    const uint8_t Rex =
        (Width == 8 ? 8 : 0) | (R & 8 ? 4 : 0) | (B & 8 ? 1 : 0);
    if (Rex)
      Bytes.push_back(0x40 | Rex);
    Bytes.insert(Bytes.end(), {0x0f, 0x38, 0xf6, ModRM});
    return Bytes;
  }
  if (F == Form::Apx)
    V = 0;
  return {0x62,
          static_cast<uint8_t>(0xf4 ^ (R & 8 ? 0x80 : 0) ^ (R & 16 ? 0x10 : 0) ^
                               (B & 8 ? 0x20 : 0) ^ (B & 16 ? 8 : 0)),
          static_cast<uint8_t>(((~V & 15) << 3) | (Width == 8 ? 0x80 : 0) | 4 |
                               (Id == X86_INS_ADCX ? 1 : 2)),
          static_cast<uint8_t>((V & 16 ? 0 : 8) | (F == Form::Ndd ? 0x10 : 0)),
          0x66,
          ModRM};
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

void checkAudit(const std::vector<LowOp> &Ops,
                const LowInstructionUndefinedEffects &E) {
  EXPECT_EQ(E.Coverage, LowUndefinedCoverage::Complete) << E.Diagnostic;
  EXPECT_TRUE(E.Effects.empty());
  EXPECT_EQ(E.OperationDigest, lowUndefinedOperationDigest(Ops));
}
std::pair<uint64_t, unsigned> sum(uint64_t A, uint64_t B, unsigned C,
                                  unsigned W) {
  const auto S =
      llvm::APInt(128, A & mask(W)) + llvm::APInt(128, B & mask(W)) + C;
  return {S.trunc(W * 8).getZExtValue(), static_cast<unsigned>(S[W * 8])};
}
TEST(X86ADX, ValuesAliasesMemoryAndEveryFlagAcrossDecodeRoutes) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (Form F : {Form::Legacy, Form::Apx, Form::Ndd}) {
      if (Target == Arch::X86 && F != Form::Legacy)
        continue;
      for (unsigned W : {4u, 8u}) {
        if (Target == Arch::X86 && W == 8)
          continue;
        for (unsigned Id : Instructions)
          for (auto Registers :
               {std::array{0u, 2u, 1u}, std::array{0u, 0u, 1u},
                std::array{0u, 1u, 1u}, std::array{0u, 0u, 0u}})
            for (bool Memory : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                auto [R, V, B] = Registers;
                const auto Bytes = encode(Id, F, W, R, V, B, Memory);
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
                for (uint64_t A :
                     {UINT64_C(0), UINT64_C(1), mask(W),
                      UINT64_C(1) << (W * 8 - 1), UINT64_C(0xeeee0000fffffffe)})
                  for (uint64_t BValue :
                       {UINT64_C(0), UINT64_C(1), mask(W), mask(W) - 1})
                    for (unsigned Old = 0; Old != 128; ++Old) {
                      auto Image = image(Target, BValue, W);
                      NdOpEmulator E(Image);
                      E.setStrictMode(true);
                      E.setRegister(reg(V), 0xaabbccdd);
                      E.setRegister(
                          reg(R),
                          A & (Target == Arch::X86 ? mask(4) : UINT64_MAX));
                      E.setRegister(reg(B), Memory ? Data : BValue);
                      const auto Left = *E.getRegister(reg(R));
                      const auto Right =
                          Memory ? BValue : *E.getRegister(reg(B));
                      const unsigned Flag =
                          Id == X86_INS_ADCX ? x86reg::CF : x86reg::OF;
                      for (unsigned J = 0; J != Flags.size(); ++J)
                        E.setRegister(Flags[J], (Old >> J) & 1);
                      const auto [Expected, Carry] =
                          sum(Left, Right, *E.getRegister(Flag), W);
                      ASSERT_EQ(E.run(Ops), Ops.size());
                      EXPECT_EQ(E.getRegister(reg(F == Form::Ndd ? V : R)),
                                Expected);
                      for (unsigned J = 0; J != Flags.size(); ++J)
                        EXPECT_EQ(E.getRegister(Flags[J]),
                                  Flags[J] == Flag ? Carry : (Old >> J) & 1);
                      EXPECT_FALSE(E.skips().any());
                      ++Cases;
                    }
              }
      }
    }
  RecordProperty("value_flag_cases", Cases);
}
TEST(X86ADX, FullWidthCarryAndOverflowAreIndependent) {
  unsigned Cases = 0;
  std::mt19937_64 Random(0x7ccf01);
  for (unsigned W : {4u, 8u})
    for (Form F : {Form::Legacy, Form::Apx, Form::Ndd}) {
      Decoder D;
      ASSERT_TRUE(D.init(Arch::X64));
      std::vector<LowOp> Ops;
      for (auto Id : {X86_INS_ADCX, X86_INS_ADOX, X86_INS_ADCX, X86_INS_ADOX}) {
        const unsigned R = Id == X86_INS_ADCX ? 0 : 2;
        const auto Bytes = encode(Id, F, W, R, R, 1);
        DecodedInsn I{};
        ASSERT_EQ(decode(D, Bytes, 2, I), Bytes.size());
        ASSERT_NO_THROW(D.liftToLow(I, Ops));
      }
      for (unsigned K = 0; K != 4096; ++K)
        for (unsigned CF : {0u, 1u})
          for (unsigned OF : {0u, 1u}) {
            const uint64_t A = Random(), B = Random(), C = Random();
            auto Image = image(Arch::X64, 0, W);
            NdOpEmulator E(Image);
            E.setStrictMode(true);
            E.setRegister(x86reg::RAX, A);
            E.setRegister(x86reg::RCX, B);
            E.setRegister(x86reg::RDX, C);
            for (auto Flag : Flags)
              E.setRegister(Flag, 1);
            E.setRegister(x86reg::CF, CF);
            E.setRegister(x86reg::OF, OF);
            const auto AX1 = sum(A, B, CF, W), DX1 = sum(C, B, OF, W);
            const auto AX2 = sum(AX1.first, B, AX1.second, W),
                       DX2 = sum(DX1.first, B, DX1.second, W);
            ASSERT_EQ(E.run(Ops), Ops.size());
            EXPECT_EQ(E.getRegister(x86reg::RAX), AX2.first);
            EXPECT_EQ(E.getRegister(x86reg::RDX), DX2.first);
            EXPECT_EQ(E.getRegister(x86reg::CF), AX2.second);
            EXPECT_EQ(E.getRegister(x86reg::OF), DX2.second);
            for (auto Flag : Flags)
              if (Flag != x86reg::CF && Flag != x86reg::OF)
                EXPECT_EQ(E.getRegister(Flag), 1u);
            ++Cases;
          }
    }
  RecordProperty("interleaved_chain_cases", Cases);
}
TEST(X86ADX, AllRegisterBanksAndWidthsHaveCompleteAudit) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (Form F : {Form::Legacy, Form::Apx, Form::Ndd}) {
      if (Target == Arch::X86 && F != Form::Legacy)
        continue;
      Decoder D;
      ASSERT_TRUE(D.init(Target));
      const unsigned Count = Target == Arch::X86 ? 8
                             : F == Form::Legacy ? 16
                                                 : 32;
      for (unsigned W : {4u, 8u}) {
        if (Target == Arch::X86 && W == 8)
          continue;
        for (unsigned Id : Instructions)
          for (unsigned R = 0; R != Count; ++R)
            for (unsigned B = 0; B != Count; ++B)
              for (unsigned V = 0; V != (F == Form::Ndd ? Count : 1); ++V) {
                const auto Bytes = encode(Id, F, W, R, V, B);
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
TEST(X86ADX, MandatorySelectorsIgnored66AndRexCancellation) {
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (unsigned Shape = 0; Shape != 6; ++Shape)
      for (unsigned Route : {0u, 1u, 2u}) {
        auto Bytes = encode(Id, Form::Legacy, 8);
        unsigned W = 8;
        if (Shape == 1) {
          Bytes.insert(Bytes.begin(), 0x49);
        }
        if (Shape == 2) {
          Bytes.erase(Bytes.begin() + 1);
          Bytes.insert(Bytes.begin(), 0x48);
          W = 4;
        }
        if (Shape == 3) {
          Bytes.insert(Bytes.begin() + 2, 0x40);
          W = 4;
        }
        if (Shape >= 4) {
          if (Id == X86_INS_ADCX)
            continue;
          Bytes.insert(Bytes.begin() + (Shape == 4 ? 0 : 1), 0x66);
        }
        Decoder D;
        ASSERT_TRUE(D.init(Arch::X64));
        DecodedInsn I{};
        ASSERT_EQ(decode(D, Bytes, Route, I), Bytes.size());
        std::vector<LowOp> Ops;
        LowInstructionUndefinedEffects Effects;
        ASSERT_NO_THROW(D.liftToLow(I, Ops, {}, {}, &Effects));
        checkAudit(Ops, Effects);
        auto Image = image(Arch::X64, 0, W);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(x86reg::RAX, UINT64_MAX);
        E.setRegister(x86reg::RCX, 0);
        for (auto Flag : Flags)
          E.setRegister(Flag, 1);
        ASSERT_EQ(E.run(Ops), Ops.size());
        EXPECT_EQ(E.getRegister(x86reg::RAX), 0u);
        EXPECT_EQ(E.getRegister(Id == X86_INS_ADCX ? x86reg::CF : x86reg::OF),
                  1u);
        ++Cases;
      }
  RecordProperty("prefix_cases", Cases);
}
TEST(X86ADX, MemoryTailsKeepDisplacementsSegmentsAndAddressSizes) {
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (Arch Target : {Arch::X86, Arch::X64})
      for (Form F : {Form::Legacy, Form::Apx, Form::Ndd}) {
        if (Target == Arch::X86 && F != Form::Legacy)
          continue;
        for (bool AddressOverride : {false, true})
          for (uint8_t Segment : {0, 0x64, 0x65})
            for (unsigned Shape : {0u, 1u, 2u, 3u, 4u}) {
              auto Bytes =
                  encode(Id, F, Target == Arch::X64 ? 8 : 4, 0, 0, 0, true);
              const unsigned ModRM = Bytes.size() - 1;
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

TEST(X86ADX, MemoryFaultPrecedesEveryDestinationAndFlagWrite) {
  for (unsigned Id : Instructions)
    for (Form F : {Form::Legacy, Form::Apx, Form::Ndd}) {
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
      for (auto Flag : Flags)
        E.setRegister(Flag, 1);
      EXPECT_LT(E.run(Ops), Ops.size());
      EXPECT_EQ(E.getRegister(x86reg::RAX), 0x1234u);
      EXPECT_EQ(E.getRegister(x86reg::RDX), 0x5678u);
      for (auto Flag : Flags)
        EXPECT_EQ(E.getRegister(Flag), 1u);
    }
}
TEST(X86ADX, RawAndDetailMutationsFailWithoutPartialEffects) {
  const std::vector<std::function<void(cs_insn &)>> Mutations = {
      [](auto &I) { I.bytes[0] ^= 1; },
      [](auto &I) { I.bytes[I.detail->x86.encoding.modrm_offset - 1] ^= 1; },
      [](auto &I) { I.bytes[I.detail->x86.encoding.modrm_offset] ^= 8; },
      [](auto &I) { --I.size; },
      [](auto &I) { I.bytes[I.size++] = 0; },
      [](auto &I) { I.illegal = true; },
      [](auto &I) { I.detail->x86.op_count = 0; },
      [](auto &I) { I.detail->x86.op_count = 4; },
      [](auto &I) { I.detail->x86.operands[0].size = 2; },
      [](auto &I) { I.detail->x86.operands[0].reg = X86_REG_RBX; },
      [](auto &I) { I.detail->x86.operands[0].access = CS_AC_READ; },
      [](auto &I) { I.detail->x86.operands[1].access = CS_AC_WRITE; },
      [](auto &I) { I.detail->x86.operands[1].size = 4; },
      [](auto &I) { I.detail->x86.prefix[0] = 0xf0; },
      [](auto &I) { I.detail->x86.rex ^= 1; },
      [](auto &I) { I.detail->x86.rex2 = 1; },
      [](auto &I) { I.detail->x86.addr_size = 2; },
      [](auto &I) { I.detail->x86.encoding.imm_size = 1; },
      [](auto &I) { I.detail->x86.encoding.imm_offset = 1; },
      [](auto &I) { I.detail->x86.encoding.disp_size = 1; },
      [](auto &I) { I.detail->x86.eflags ^= X86_EFLAGS_RESET_CF; },
      [](auto &I) { I.detail->regs_read_count = 0; },
      [](auto &I) { I.detail->regs_write_count = 0; },
      [](auto &I) { I.detail->x86.avx_sae = true; },
      [](auto &I) { I.detail->x86.avx_cc = X86_AVX_CC_EQ; },
      [](auto &I) { I.detail->x86.operands[0].avx_zero_opmask = true; },
      [](auto &I) { I.detail->x86.operands[1].avx_bcast = X86_AVX_BCAST_2; }};
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (Form F : {Form::Legacy, Form::Apx, Form::Ndd})
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
TEST(X86ADX, ApxReservedBitsAndUnusedVFieldRejectEvenConsistentMetadata) {
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (Form F : {Form::Apx, Form::Ndd})
      for (auto Field :
           {std::array{2u, 4u}, std::array{3u, 1u}, std::array{3u, 2u},
            std::array{3u, 4u}, std::array{3u, 32u}, std::array{3u, 64u},
            std::array{3u, 128u}, std::array{2u, 8u}, std::array{3u, 8u}}) {
        const auto [Byte, Bit] = Field;
        if (F == Form::Ndd &&
            ((Byte == 2 && Bit == 8) || (Byte == 3 && Bit == 8)))
          continue;
        Decoder D;
        ASSERT_TRUE(D.init(Arch::X64));
        const auto Bytes = encode(Id, F, 8);
        DecodedInsn I{};
        ASSERT_EQ(decode(D, Bytes, 2, I), Bytes.size());
        I.Raw->bytes[Byte] ^= Bit;
        I.Raw->detail->x86.opcode[Byte] ^= Bit;
        std::vector<LowOp> Ops;
        EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
        EXPECT_TRUE(Ops.empty());
        ++Cases;
      }
  RecordProperty("reserved_field_cases", Cases);
}
} // namespace
