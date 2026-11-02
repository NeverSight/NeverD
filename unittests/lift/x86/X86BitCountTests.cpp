//===- X86BitCountTests.cpp - Scalar bit-count values and flags ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualify legacy, REX2 and APX counts against independent bit-loop oracles.
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
constexpr va_t Data = 0x7000;
constexpr std::array<unsigned, 3> Instructions = {X86_INS_TZCNT, X86_INS_LZCNT,
                                                  X86_INS_POPCNT};
constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::ZF, x86reg::OF,
                                           x86reg::SF, x86reg::AF, x86reg::PF,
                                           x86reg::DF};
enum class Form { Legacy, Rex2, Apx, ApxNF };

bool apx(Form F) { return F == Form::Apx || F == Form::ApxNF; }
uint64_t mask(unsigned Width) {
  return Width == 8 ? UINT64_MAX : (UINT64_C(1) << (Width * 8)) - 1;
}
uint64_t reg(unsigned Index) {
  return Index < 16 ? Index * 8 : x86reg::extendedGeneralReg(Index - 16);
}
uint64_t expected(unsigned Id, uint64_t Source, unsigned Width) {
  uint64_t Count = 0;
  for (unsigned I = 0; I != Width * 8; ++I) {
    const unsigned Bit = Id == X86_INS_LZCNT ? Width * 8 - 1 - I : I;
    const bool Set = (Source >> Bit) & 1;
    if (Id == X86_INS_POPCNT)
      Count += Set;
    else if (Set)
      break;
    else
      ++Count;
  }
  return Count;
}

std::vector<uint8_t> encode(unsigned Id, Form F, unsigned Width,
                            unsigned Dst = 0, unsigned Src = 1,
                            bool Memory = false) {
  const uint8_t ModRM = (Memory ? 0 : 0xc0) | ((Dst & 7) << 3) | (Src & 7);
  if (apx(F))
    return {0x62,
            static_cast<uint8_t>(0xf4 ^ ((Dst & 8) ? 0x80 : 0) ^
                                 ((Dst & 16) ? 0x10 : 0) ^
                                 ((Src & 8) ? 0x20 : 0) ^ ((Src & 16) ? 8 : 0)),
            static_cast<uint8_t>(0x7c | (Width == 8 ? 0x80 : 0) |
                                 (Width == 2 ? 1 : 0)),
            static_cast<uint8_t>(F == Form::ApxNF ? 0x0c : 8),
            static_cast<uint8_t>(Id == X86_INS_TZCNT   ? 0xf4
                                 : Id == X86_INS_LZCNT ? 0xf5
                                                       : 0x88),
            ModRM};
  std::vector<uint8_t> Bytes{0xf3};
  if (Width == 2)
    Bytes.push_back(0x66);
  uint8_t Extension =
      (Width == 8 ? 8 : 0) | ((Dst & 8) ? 4 : 0) | ((Src & 8) ? 1 : 0);
  if (F == Form::Rex2) {
    Extension |= 0x80 | ((Dst & 16) ? 0x40 : 0) | ((Src & 16) ? 0x10 : 0);
    Bytes.insert(Bytes.end(), {0xd5, Extension});
  } else {
    if (Extension)
      Bytes.push_back(0x40 | Extension);
    Bytes.push_back(0x0f);
  }
  Bytes.push_back(Id == X86_INS_TZCNT   ? 0xbc
                  : Id == X86_INS_LZCNT ? 0xbd
                                        : 0xb8);
  Bytes.push_back(ModRM);
  return Bytes;
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

void checkAudit(unsigned Id, Form F, const std::vector<LowOp> &Ops,
                const LowInstructionUndefinedEffects &Effects) {
  ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic;
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  const unsigned Count = Id == X86_INS_POPCNT || F == Form::ApxNF ? 0 : 4;
  ASSERT_EQ(Effects.Effects.size(), Count);
  for (unsigned I = 0; I != Count; ++I) {
    const auto &E = Effects.Effects[I];
    EXPECT_EQ(E.Output, NdVar::reg(Flags[I + 2], 1));
    EXPECT_EQ(E.BitOffset, 0u);
    EXPECT_EQ(E.BitCount, 1u);
    EXPECT_FALSE(E.When);
    EXPECT_GT(E.AfterOp, 0u);
    EXPECT_LE(E.AfterOp, Ops.size());
  }
}

TEST(X86BitCount, ValuesAliasesWidthsAndFlagsAcrossDecodeRoutes) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Id : Instructions)
      for (Form F : {Form::Legacy, Form::Rex2, Form::Apx, Form::ApxNF}) {
        if (Target == Arch::X86 && F != Form::Legacy)
          continue;
        for (unsigned Width : {2u, 4u, 8u}) {
          if (Target == Arch::X86 && Width == 8)
            continue;
          const unsigned High = F == Form::Legacy ? 8 : 16;
          for (auto Registers : {std::array{0u, 1u}, std::array{1u, 1u},
                                 std::array{High, High + 1}}) {
            const auto [Dst, Src] = Registers;
            if (Target == Arch::X86 && Src >= 8)
              continue;
            for (bool Memory : {false, true})
              for (unsigned Route : {0u, 1u, 2u}) {
                const auto Bytes = encode(Id, F, Width, Dst, Src, Memory);
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
                checkAudit(Id, F, Ops, Effects);
                for (uint64_t Value :
                     {UINT64_C(0), UINT64_C(1), mask(Width),
                      UINT64_C(1) << (Width * 8 - 1),
                      UINT64_C(0xa5195ac3fe817204) & mask(Width)})
                  for (unsigned Old : {0u, 0x7fu}) {
                    auto Image = image(Target, Value, Width);
                    NdOpEmulator E(Image);
                    E.setStrictMode(true);
                    const uint64_t Seed = Target == Arch::X64
                                              ? UINT64_C(0xccbbaa9988776655)
                                              : UINT64_C(0x88776655);
                    E.setRegister(reg(Dst), Seed);
                    E.setRegister(reg(Src), Memory ? Data : Value);
                    const auto Original = E.getRegister(reg(Dst));
                    ASSERT_TRUE(Original.has_value());
                    for (unsigned I = 0; I != Flags.size(); ++I)
                      E.setRegister(Flags[I], (Old >> I) & 1);
                    ASSERT_EQ(E.run(Ops), Ops.size());
                    const auto Count = expected(Id, Value, Width);
                    EXPECT_EQ(E.getRegister(reg(Dst)),
                              Width == 2 ? (*Original & ~mask(2)) | Count
                                         : Count);
                    for (unsigned I = 0; I != Flags.size(); ++I) {
                      const bool Preserved = F == Form::ApxNF || I == 6;
                      const uint64_t Flag =
                          Preserved ? (Old >> I) & 1
                          : I == 0  ? Id != X86_INS_POPCNT && Value == 0
                          : I == 1  ? Count == 0
                                    : 0;
                      EXPECT_EQ(E.getRegister(Flags[I]), Flag);
                    }
                    ++Cases;
                  }
              }
          }
        }
      }
  RecordProperty("value_flag_cases", Cases);
}

TEST(X86BitCount, EveryWordValueMatchesTheBitLoopReference) {
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (Form F : {Form::Legacy, Form::Rex2, Form::Apx, Form::ApxNF}) {
      Decoder D;
      ASSERT_TRUE(D.init(Arch::X64));
      const auto Bytes = encode(Id, F, 2);
      DecodedInsn Insn{};
      ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
      auto Image = image(Arch::X64, 0, 2);
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      constexpr uint64_t Upper = UINT64_C(0x123456789abc0000);
      E.setRegister(x86reg::RAX, Upper);
      for (unsigned Value = 0; Value != 0x10000; ++Value) {
        E.setRegister(x86reg::RCX, ~mask(2) | Value);
        ASSERT_EQ(E.run(Ops), Ops.size());
        const auto Count = expected(Id, Value, 2);
        ASSERT_EQ(E.getRegister(x86reg::RAX), Upper | Count) << Value;
        if (F != Form::ApxNF) {
          ASSERT_EQ(E.getRegister(x86reg::CF),
                    Id != X86_INS_POPCNT && Value == 0);
          ASSERT_EQ(E.getRegister(x86reg::ZF), Count == 0);
        }
        ++Cases;
      }
    }
  RecordProperty("exhaustive_word_cases", Cases);
}

TEST(X86BitCount, EveryRegisterBankAndWidthHasACompleteAudit) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Id : Instructions)
      for (Form F : {Form::Legacy, Form::Rex2, Form::Apx, Form::ApxNF}) {
        if (Target == Arch::X86 && F != Form::Legacy)
          continue;
        const unsigned Registers = Target == Arch::X86 ? 8
                                   : F == Form::Legacy ? 16
                                                       : 32;
        Decoder D;
        ASSERT_TRUE(D.init(Target));
        for (unsigned Width : {2u, 4u, 8u}) {
          if (Target == Arch::X86 && Width == 8)
            continue;
          for (unsigned Dst = 0; Dst != Registers; ++Dst)
            for (unsigned Src = 0; Src != Registers; ++Src) {
              const auto Bytes = encode(Id, F, Width, Dst, Src);
              SCOPED_TRACE(testing::PrintToString(Bytes));
              DecodedInsn Insn{};
              ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
              ASSERT_EQ(Insn.Id, Id);
              std::vector<LowOp> Ops;
              LowInstructionUndefinedEffects Effects;
              ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
              checkAudit(Id, F, Ops, Effects);
              ++Cases;
            }
        }
      }
  RecordProperty("register_encoding_cases", Cases);
}

TEST(X86BitCount, MemoryTailsPreserveAddressSizesAndDisplacements) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Id : Instructions)
      for (Form F : {Form::Legacy, Form::Rex2, Form::Apx, Form::ApxNF}) {
        if (Target == Arch::X86 && F != Form::Legacy)
          continue;
        for (bool Address : {false, true})
          for (uint8_t Segment : {0, 0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65})
            for (unsigned Shape = 0; Shape != 5; ++Shape) {
              auto Bytes = encode(Id, F, 4, 0, 0, true);
              const unsigned ModRM = Bytes.size() - 1;
              const bool Address16 = Target == Arch::X86 && Address;
              if (Shape == 1) {
                Bytes[ModRM] |= 0x40;
                Bytes.push_back(0xf0);
              } else if (Shape == 2 || Shape == 4) {
                Bytes[ModRM] |= Shape == 2 ? 0x80 : Address16 ? 6 : 5;
                Bytes.insert(Bytes.end(), {0x34, 0x12});
                if (!Address16)
                  Bytes.insert(Bytes.end(), {0, 0});
              } else if (Shape == 3) {
                Bytes[ModRM] |= 4;
                if (!Address16)
                  Bytes.push_back(0x88);
              }
              if (Address)
                Bytes.insert(Bytes.begin(), 0x67);
              if (Segment)
                Bytes.insert(Bytes.begin(), Segment);
              for (unsigned Route : {0u, 1u, 2u}) {
                SCOPED_TRACE(testing::PrintToString(Bytes));
                Decoder D;
                ASSERT_TRUE(D.init(Target));
                DecodedInsn Insn{};
                ASSERT_EQ(decode(D, Bytes, Route, Insn), Bytes.size());
                std::vector<LowOp> Ops;
                LowInstructionUndefinedEffects Effects;
                ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
                checkAudit(Id, F, Ops, Effects);
                unsigned Loads = 0;
                for (const auto &Op : Ops)
                  Loads += Op.Opcode == NdOp::LOAD;
                EXPECT_EQ(Loads, 1u);
                ++Cases;
              }
            }
      }
  RecordProperty("memory_tail_cases", Cases);
}

TEST(X86BitCount, FaultingMemoryPublishesNoDestinationOrFlags) {
  for (unsigned Id : Instructions)
    for (Form F : {Form::Legacy, Form::Rex2, Form::Apx, Form::ApxNF})
      for (unsigned Width : {2u, 4u, 8u}) {
        Decoder D;
        ASSERT_TRUE(D.init(Arch::X64));
        const auto Bytes = encode(Id, F, Width, 1, 1, true);
        DecodedInsn Insn{};
        ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
        std::vector<LowOp> Ops;
        ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
        auto Image = image(Arch::X64, 0, Width);
        NdOpEmulator E(Image);
        E.setStrictMode(true);
        E.setRegister(x86reg::RCX, 0xdead0000);
        for (auto Flag : Flags)
          E.setRegister(Flag, 1);
        EXPECT_LT(E.run(Ops), Ops.size());
        EXPECT_EQ(E.getRegister(x86reg::RCX), 0xdead0000u);
        for (auto Flag : Flags)
          EXPECT_EQ(E.getRegister(Flag), 1u);
      }
}

TEST(X86BitCount, InconsistentBytesOrDetailFailWithoutPartialEffects) {
  const std::vector<std::function<void(cs_insn &)>> Mutations = {
      [](auto &I) { I.bytes[0] ^= 1; },
      [](auto &I) { I.bytes[I.detail->x86.encoding.modrm_offset - 1] ^= 1; },
      [](auto &I) { I.bytes[I.detail->x86.encoding.modrm_offset] ^= 8; },
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
    for (Form F : {Form::Legacy, Form::Rex2, Form::Apx, Form::ApxNF})
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

TEST(X86BitCount, LegacyPrefixOrderAndRexCancellationKeepTheActualWidth) {
  for (unsigned Id : Instructions) {
    const auto Canonical = encode(Id, Form::Legacy, 2);
    auto Reordered = Canonical;
    std::swap(Reordered[0], Reordered[1]);
    auto Cancelled = Canonical;
    Cancelled.insert(Cancelled.begin(), {0x4f, 0x48});
    for (const auto &Bytes : {Canonical, Reordered, Cancelled}) {
      Decoder D;
      ASSERT_TRUE(D.init(Arch::X64));
      DecodedInsn Insn{};
      ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
      ASSERT_EQ(Insn.Raw->detail->x86.operands[0].size, 2);
      std::vector<LowOp> Ops;
      LowInstructionUndefinedEffects Effects;
      ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
      checkAudit(Id, Form::Legacy, Ops, Effects);
    }
  }
}

TEST(X86BitCount, ReservedApxFieldsCannotBeAuthorizedByMatchingDetail) {
  unsigned Cases = 0;
  for (unsigned Id : Instructions)
    for (Form F : {Form::Apx, Form::ApxNF})
      for (auto Field :
           {std::array{1u, 1u}, std::array{2u, 2u}, std::array{2u, 4u},
            std::array{2u, 8u}, std::array{3u, 1u}, std::array{3u, 2u},
            std::array{3u, 8u}, std::array{3u, 16u}, std::array{3u, 32u},
            std::array{3u, 64u}, std::array{3u, 128u}}) {
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
        ++Cases;
      }
  RecordProperty("reserved_field_cases", Cases);
}

} // namespace
