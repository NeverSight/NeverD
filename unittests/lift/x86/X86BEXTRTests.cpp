//===- X86BEXTRTests.cpp - Bit extraction values, flags and admission ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Qualify XOP, VEX and APX BEXTR against a bit-by-bit reference.
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
constexpr std::array<unsigned, 7> Flags = {x86reg::CF, x86reg::ZF, x86reg::OF,
                                           x86reg::SF, x86reg::AF, x86reg::PF,
                                           x86reg::DF};
enum class Form { Xop, Vex, Apx, ApxNF };

uint64_t mask(unsigned Width) { return Width == 8 ? UINT64_MAX : UINT32_MAX; }
uint64_t reg(unsigned Index) {
  return Index < 16 ? Index * 8 : x86reg::extendedGeneralReg(Index - 16);
}

uint64_t expected(uint64_t Source, uint64_t Control, unsigned Width) {
  const unsigned Start = Control & 0xff;
  const unsigned Count = (Control >> 8) & 0xff;
  uint64_t Result = 0;
  for (unsigned I = 0; I < Count && I + Start < Width * 8; ++I)
    if ((Source >> (I + Start)) & 1)
      Result |= UINT64_C(1) << I;
  return Result;
}

std::vector<uint8_t> encode(Form F, bool W, unsigned Destination = 0,
                            unsigned Source = 1, unsigned Control = 2,
                            uint32_t Immediate = 0x804, bool Memory = false) {
  const uint8_t ModRM =
      (Memory ? 0 : 0xc0) | ((Destination & 7) << 3) | (Source & 7);
  std::vector<uint8_t> Bytes;
  if (F == Form::Apx || F == Form::ApxNF) {
    Bytes = {0x62,
             static_cast<uint8_t>(0xf2 ^ ((Destination & 8) ? 0x80 : 0) ^
                                  ((Destination & 16) ? 0x10 : 0) ^
                                  ((Source & 8) ? 0x20 : 0) ^
                                  ((Source & 16) ? 8 : 0)),
             static_cast<uint8_t>((W ? 0x80 : 0) | 4 | ((~Control & 15) << 3)),
             static_cast<uint8_t>((Control & 16 ? 0 : 8) |
                                  (F == Form::ApxNF ? 4 : 0)),
             0xf7,
             ModRM};
  } else {
    Bytes = {
        static_cast<uint8_t>(F == Form::Xop ? 0x8f : 0xc4),
        static_cast<uint8_t>((F == Form::Xop ? 0xea : 0xe2) ^
                             ((Destination & 8) ? 0x80 : 0) ^
                             ((Source & 8) ? 0x20 : 0)),
        static_cast<uint8_t>((W ? 0x80 : 0) |
                             (F == Form::Xop ? 0x78 : (~Control & 15) << 3)),
        static_cast<uint8_t>(F == Form::Xop ? 0x10 : 0xf7), ModRM};
    if (F == Form::Xop)
      for (unsigned I = 0; I != 4; ++I)
        Bytes.push_back(Immediate >> (I * 8));
  }
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

void checkAudit(const DecodedInsn &Insn, const std::vector<LowOp> &Ops,
                const LowInstructionUndefinedEffects &Effects, bool NF) {
  ASSERT_EQ(Effects.Coverage, LowUndefinedCoverage::Complete)
      << Effects.Diagnostic << ' ' << Insn.Raw->op_str;
  EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
  ASSERT_EQ(Effects.Effects.size(), NF ? 0u : 3u);
  if (NF)
    return;
  for (unsigned I = 0; I != 3; ++I) {
    const auto &E = Effects.Effects[I];
    EXPECT_EQ(E.Output, NdVar::reg(Flags[I + 3], 1));
    EXPECT_EQ(E.BitOffset, 0u);
    EXPECT_EQ(E.BitCount, 1u);
    EXPECT_FALSE(E.When);
    EXPECT_GT(E.AfterOp, 0u);
    EXPECT_LE(E.AfterOp, Ops.size());
  }
}

TEST(X86BEXTR, ValuesAliasesMemoryAndDefinedFlagsAcrossDecodeRoutes) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (Form F : {Form::Xop, Form::Vex, Form::Apx, Form::ApxNF}) {
      if (Target == Arch::X86 && (F == Form::Apx || F == Form::ApxNF))
        continue;
      for (bool W : {false, true}) {
        const unsigned Width = Target == Arch::X64 && W ? 8 : 4;
        const unsigned High = F == Form::Apx || F == Form::ApxNF ? 16 : 8;
        for (auto Registers :
             {std::array{0u, 1u, 2u}, std::array{1u, 1u, 2u},
              std::array{2u, 1u, 2u}, std::array{High, High + 1, High + 2}}) {
          const auto [Dst, Src, Ctrl] = Registers;
          if (Target == Arch::X86 && Src >= 8)
            continue;
          for (bool Memory : {false, true})
            for (unsigned Route : {0u, 1u, 2u})
              for (uint32_t Control :
                   {0u, 1u, 0x100u, 0x1001u, 0x1f1fu, 0x2020u, 0x3f3fu, 0x4000u,
                    0x4040u, 0xff00u, 0xff3fu, 0xffffffffu, 0xabcd0804u}) {
                const auto Bytes =
                    encode(F, W, Dst, Src, Ctrl, Control, Memory);
                SCOPED_TRACE(testing::PrintToString(Bytes));
                SCOPED_TRACE(static_cast<unsigned>(Target));
                SCOPED_TRACE(Route);
                Decoder D;
                ASSERT_TRUE(D.init(Target));
                D.setStrict(true);
                DecodedInsn Insn{};
                ASSERT_EQ(decode(D, Bytes, Route, Insn), Bytes.size());
                ASSERT_EQ(Insn.Id, X86_INS_BEXTR);
                std::vector<LowOp> Ops, Plain;
                LowInstructionUndefinedEffects Effects;
                ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
                ASSERT_NO_THROW(D.liftToLow(Insn, Plain));
                EXPECT_EQ(lowUndefinedOperationDigest(Ops),
                          lowUndefinedOperationDigest(Plain));
                checkAudit(Insn, Ops, Effects, F == Form::ApxNF);
                for (uint64_t Value :
                     {UINT64_C(0), UINT64_C(1), mask(Width),
                      UINT64_C(0xa5195ac3fe817204) & mask(Width)})
                  for (unsigned Old : {0u, 0x7fu}) {
                    auto Image = image(Target, Value, Width);
                    NdOpEmulator E(Image);
                    E.setStrictMode(true);
                    E.setRegister(reg(Dst), Target == Arch::X64
                                                ? UINT64_C(0xccbbaa9988776655)
                                                : UINT64_C(0x88776655));
                    E.setRegister(reg(Src), Memory ? Data : Value);
                    if (F != Form::Xop)
                      E.setRegister(reg(Ctrl), Control);
                    for (unsigned I = 0; I != Flags.size(); ++I)
                      E.setRegister(Flags[I], (Old >> I) & 1);
                    ASSERT_EQ(E.run(Ops), Ops.size());
                    const auto Result = expected(Value, Control, Width);
                    EXPECT_EQ(E.getRegister(reg(Dst)), Result);
                    for (unsigned I = 0; I != Flags.size(); ++I) {
                      const bool Preserved = F == Form::ApxNF || I == 6;
                      EXPECT_EQ(E.getRegister(Flags[I]), Preserved
                                                             ? (Old >> I) & 1
                                                         : I == 1 ? Result == 0
                                                                  : 0);
                    }
                    ++Cases;
                  }
              }
        }
      }
    }
  RecordProperty("value_flag_cases", Cases);
}

TEST(X86BEXTR, EveryStartAndLengthPairAvoidsShiftPoison) {
  unsigned Cases = 0;
  for (Form F : {Form::Vex, Form::Apx, Form::ApxNF})
    for (bool W : {false, true}) {
      const unsigned Width = W ? 8 : 4;
      const auto Bytes = encode(F, W);
      Decoder D;
      ASSERT_TRUE(D.init(Arch::X64));
      DecodedInsn Insn{};
      ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
      std::vector<LowOp> Ops;
      ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
      auto Image = image(Arch::X64, 0, Width);
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      constexpr uint64_t Value = UINT64_C(0xa5195ac3fe817204);
      E.setRegister(x86reg::RCX, Value);
      for (unsigned Control = 0; Control != 0x10000; ++Control) {
        E.setRegister(x86reg::RDX, UINT64_C(0xffeeddcc00000000) | Control);
        ASSERT_EQ(E.run(Ops), Ops.size());
        ASSERT_EQ(E.getRegister(x86reg::RAX), expected(Value, Control, Width))
            << Control;
        ++Cases;
      }
    }
  RecordProperty("exhaustive_control_cases", Cases);
}

TEST(X86BEXTR, I386IgnoresWAndRegisterExtensionsWithoutExtendingTheBank) {
  unsigned Cases = 0;
  for (Form F : {Form::Xop, Form::Vex})
    for (bool W : {false, true})
      for (bool B : {false, true})
        for (bool HighControl : {false, true}) {
          if (F == Form::Xop && HighControl)
            continue; // Unused XOP.vvvv must remain 1111, unlike VEX's input.
          const auto Bytes = encode(F, W, 0, B ? 9 : 1, HighControl ? 10 : 2);
          for (unsigned Route : {0u, 1u, 2u}) {
            Decoder D;
            ASSERT_TRUE(D.init(Arch::X86));
            DecodedInsn Insn{};
            ASSERT_EQ(decode(D, Bytes, Route, Insn), Bytes.size());
            std::vector<LowOp> Ops;
            LowInstructionUndefinedEffects Effects;
            ASSERT_NO_THROW(D.liftToLow(Insn, Ops, {}, {}, &Effects));
            checkAudit(Insn, Ops, Effects, false);
            auto Image = image(Arch::X86, 0, 4);
            NdOpEmulator E(Image);
            E.setStrictMode(true);
            E.setRegister(x86reg::RCX, 0xfedcba98);
            E.setRegister(x86reg::RDX, 0x804);
            E.setRegister(x86reg::R9, 0x12345678);
            ASSERT_EQ(E.run(Ops), Ops.size());
            EXPECT_EQ(E.getRegister(x86reg::RAX), 0xa9u);
            EXPECT_EQ(E.getRegister(x86reg::R9), 0x12345678u);
            ++Cases;
          }
        }
  RecordProperty("ignored_i386_field_cases", Cases);
}

TEST(X86BEXTR, MemoryTailsKeepDisplacementsSegmentsAndImmediatePosition) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (Form F : {Form::Xop, Form::Vex, Form::Apx, Form::ApxNF}) {
      if (Target == Arch::X86 && (F == Form::Apx || F == Form::ApxNF))
        continue;
      for (bool AddressOverride : {false, true})
        for (uint8_t Segment : {0, 0x64, 0x65})
          for (unsigned Shape : {0u, 1u, 2u, 3u, 4u}) {
            auto Bytes = encode(F, Target == Arch::X64, 0, 0, 2, 0x804, true);
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
              checkAudit(Insn, Ops, Effects, F == Form::ApxNF);
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

TEST(X86BEXTR, FaultingMemoryPublishesNoDestinationOrFlags) {
  for (Form F : {Form::Xop, Form::Vex, Form::Apx, Form::ApxNF}) {
    Decoder D;
    ASSERT_TRUE(D.init(Arch::X64));
    const auto Bytes = encode(F, true, 1, 1, 2, 0, true);
    DecodedInsn Insn{};
    ASSERT_EQ(decode(D, Bytes, 2, Insn), Bytes.size());
    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(D.liftToLow(Insn, Ops));
    auto Image = image(Arch::X64, 0, 8);
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setRegister(x86reg::RCX, 0xdead0000);
    E.setRegister(x86reg::RDX, 0); // Even an empty field must read the source.
    for (auto Flag : Flags)
      E.setRegister(Flag, 1);
    EXPECT_LT(E.run(Ops), Ops.size());
    EXPECT_EQ(E.getRegister(x86reg::RCX), 0xdead0000u);
    for (auto Flag : Flags)
      EXPECT_EQ(E.getRegister(Flag), 1u);
  }
}

TEST(X86BEXTR, InconsistentBytesOrDetailFailWithoutPartialEffects) {
  const std::vector<std::function<void(cs_insn &)>> Mutations = {
      [](auto &I) { I.bytes[0] ^= 1; },
      [](auto &I) { I.bytes[1] ^= 1; },
      [](auto &I) { I.bytes[2] ^= 4; },
      [](auto &I) { I.bytes[2] ^= 1; },
      [](auto &I) {
        I.bytes[2] |= 1;
        I.detail->x86.opcode[2] |= 1;
      },
      [](auto &I) {
        // Keep bytes/detail consistent while setting reserved L or APX ND.
        const unsigned Byte = I.bytes[0] == 0x62 ? 3 : 2;
        const uint8_t Bit = I.bytes[0] == 0x62 ? 0x10 : 4;
        I.bytes[Byte] |= Bit;
        I.detail->x86.opcode[Byte] |= Bit;
      },
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
      [](auto &I) { I.detail->x86.operands[1].access = CS_AC_WRITE; },
      [](auto &I) { I.detail->x86.operands[2].imm ^= 1; },
      [](auto &I) { I.detail->x86.prefix[0] = 0xf0; },
      [](auto &I) { I.detail->x86.addr_size = 2; },
      [](auto &I) { I.detail->x86.rex ^= 1; },
      [](auto &I) { I.detail->x86.encoding.imm_size = 1; },
      [](auto &I) { I.detail->x86.encoding.modrm_offset = 3; },
      [](auto &I) { I.detail->x86.encoding.disp_size = 1; },
      [](auto &I) { I.detail->x86.eflags ^= X86_EFLAGS_RESET_CF; },
      [](auto &I) { I.detail->regs_read_count = 1; },
      [](auto &I) { I.detail->regs_write_count ^= 1; },
      [](auto &I) { I.detail->x86.avx_sae = true; },
      [](auto &I) { I.detail->x86.xop_cc = X86_XOP_CC_LT; },
      [](auto &I) { I.detail->x86.operands[0].avx_zero_opmask = true; },
      [](auto &I) { I.detail->x86.operands[1].avx_bcast = X86_AVX_BCAST_2; }};
  unsigned Cases = 0;
  for (Form F : {Form::Xop, Form::Vex, Form::Apx, Form::ApxNF})
    for (bool Strict : {false, true})
      for (unsigned Route : {0u, 1u, 2u})
        for (const auto &Mutate : Mutations) {
          Decoder D;
          ASSERT_TRUE(D.init(Arch::X64));
          D.setStrict(Strict);
          const auto Bytes = encode(F, true);
          DecodedInsn Insn{};
          ASSERT_EQ(decode(D, Bytes, Route, Insn), Bytes.size());
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
  RecordProperty("malformed_cases", Cases);
}

TEST(X86BEXTR, UnusedXopControlFieldRemainsReservedInBothModes) {
  unsigned Cases = 0;
  for (Arch Target : {Arch::X86, Arch::X64})
    for (bool Strict : {false, true})
      for (unsigned Route : {0u, 1u, 2u})
        for (unsigned Bit : {3u, 4u, 5u, 6u}) {
          Decoder D;
          ASSERT_TRUE(D.init(Target));
          D.setStrict(Strict);
          const auto Bytes = encode(Form::Xop, true);
          DecodedInsn Insn{};
          ASSERT_EQ(decode(D, Bytes, Route, Insn), Bytes.size());
          // Unlike a VEX control register, unused XOP.vvvv must be 1111,
          // including its high bit in 32-bit mode.
          Insn.Raw->bytes[2] ^= 1u << Bit;
          Insn.Raw->detail->x86.opcode[2] ^= 1u << Bit;
          std::vector<LowOp> Ops;
          EXPECT_THROW(D.liftToLow(Insn, Ops), UnliftedInstruction);
          EXPECT_TRUE(Ops.empty());
          ++Cases;
        }
  RecordProperty("reserved_xop_control_cases", Cases);
}
} // namespace
