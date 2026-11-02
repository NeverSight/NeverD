//===- X86FPEncodingTests.cpp - FP prefix and register-role semantics ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercise real REX2 and 32-bit VEX forms across the floating-point families.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Lifter.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

using namespace neverd;

namespace {
std::vector<LowOp> lift(Arch Target, const std::vector<uint8_t> &Bytes) {
  Decoder Decode;
  EXPECT_TRUE(Decode.init(Target));
  DecodedInsn Insn{};
  EXPECT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, Insn),
            Bytes.size());
  std::vector<LowOp> Ops;
  EXPECT_NO_THROW(Decode.liftToLow(Insn, Ops));
  return Ops;
}

TEST(X86FPEncoding, Rex2FormsAcrossFamiliesStrictlyLift) {
  const std::vector<std::vector<uint8_t>> Cases = {
      {0x66, 0xd5, 0xd0, 0x58, 0x08}, // ADDPD xmm1,[r16]; R4 ignored.
      {0x66, 0xd5, 0xf5, 0x58, 0x4c, 0x67, 0xf0},       // [r31+r20*2-16].
      {0x67, 0x66, 0xd5, 0xf5, 0x58, 0x4c, 0x67, 0xf0}, // Address32.
      {0x66, 0xd5, 0xd5, 0x58, 0xca},       // ADDPD xmm9,xmm10; R4/B4 ignored.
      {0xf2, 0xd5, 0x99, 0x2a, 0xc8},       // CVTSI2SD xmm1,r24.
      {0xf2, 0xd5, 0xd9, 0x2d, 0xc9},       // CVTSD2SI r17,xmm9; B4 ignored.
      {0xf3, 0xd5, 0xdc, 0x2c, 0x3c, 0x24}, // CVTTSS2SI r31,[r20].
      {0x66, 0xd5, 0x94, 0x2f, 0x08},       // COMISD xmm9,[r16].
      {0x66, 0xd5, 0xd0, 0xc2, 0x08, 0x07}, // CMPORDPD xmm1,[r16].
      {0xd5, 0x90, 0x53, 0x08},             // RCPPS xmm1,[r16].
      {0xf3, 0xd5, 0x90, 0xe6, 0x08},       // CVTDQ2PD xmm1,[r16].
      {0xd5, 0x90, 0x5a, 0x08},             // CVTPS2PD xmm1,[r16].
      {0x66, 0xd5, 0x90, 0x5a, 0x08}};      // CVTPD2PS xmm1,[r16].
  for (const auto &Bytes : Cases) {
    SCOPED_TRACE(testing::PrintToString(Bytes));
    EXPECT_FALSE(lift(Arch::X64, Bytes).empty());
  }
}

TEST(X86FPEncoding, Mode32IgnoredVexFieldsStrictlyLift) {
  const std::vector<std::vector<uint8_t>> Cases = {
      {0xc4, 0xc1, 0x71, 0x58, 0x00},        // VADDPD, B ignored.
      {0xc4, 0xe1, 0x31, 0x58, 0xc2},        // VADDPD, used vvvv[3] ignored.
      {0xc4, 0xc1, 0x79, 0x2f, 0x00},        // VCOMISD.
      {0xc4, 0xc1, 0x73, 0x2a, 0x00},        // VCVTSI2SD.
      {0xc4, 0xc1, 0x7b, 0x2c, 0x00},        // VCVTTSD2SI.
      {0xc4, 0xc1, 0x7a, 0xe6, 0x00},        // VCVTDQ2PD.
      {0xc4, 0xc1, 0x71, 0xc2, 0x00, 0x00},  // VCMPEQPD.
      {0xc4, 0xc1, 0x78, 0x53, 0x00},        // VRCPPS.
      {0xc4, 0xc3, 0x79, 0x09, 0x00, 0x00}}; // VROUNDPD.
  for (const auto &Bytes : Cases) {
    SCOPED_TRACE(testing::PrintToString(Bytes));
    EXPECT_FALSE(lift(Arch::X86, Bytes).empty());
  }
}

uint64_t gpr(unsigned Index) {
  return Index < 16 ? x86reg::generalReg(Index)
                    : x86reg::extendedGeneralReg(Index - 16);
}

template <typename T, size_t N>
std::vector<uint8_t> bytes(const std::array<T, N> &Values) {
  std::vector<uint8_t> Result(sizeof(Values));
  std::memcpy(Result.data(), Values.data(), sizeof(Values));
  return Result;
}

BinaryImage memory(Arch Target, uint64_t Address, std::vector<uint8_t> Bytes) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Bits = Target == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Segment Data;
  Data.VA = Address;
  Data.Size = Data.FileSz = Bytes.size();
  Data.Data = std::move(Bytes);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Data));
  return Image;
}

void execute(NdOpEmulator &Emulator, const std::vector<LowOp> &Ops) {
  ASSERT_FALSE(Ops.empty());
  for (const auto &Op : Ops)
    ASSERT_TRUE(Emulator.step(Op)) << static_cast<unsigned>(Op.Opcode);
}

TEST(X86FPEncoding, Rex2AddressesRetainBaseIndexWrappingAndSegments) {
  for (const bool Address32 : {false, true})
    for (const unsigned Base : {16u, 20u, 31u})
      for (const unsigned Index : {17u, 20u, 28u, 31u})
        for (const uint8_t Segment : {0u, 0x64u, 0x65u})
          for (const bool R4 : {false, true}) {
            // Independent values distinguish base and index extensions.
            if (Base == Index)
              continue;
            SCOPED_TRACE(testing::Message()
                         << Address32 << ':' << Base << ':' << Index << ':'
                         << unsigned(Segment) << ':' << R4);
            std::vector<uint8_t> Encoding;
            if (Address32)
              Encoding.push_back(0x67);
            if (Segment)
              Encoding.push_back(Segment);
            const uint8_t Payload =
                0x84 | (R4 ? 0x40 : 0) | ((Base & 16) ? 0x10 : 0) |
                ((Base & 8) ? 1 : 0) | ((Index & 16) ? 0x20 : 0) |
                ((Index & 8) ? 2 : 0);
            Encoding.insert(Encoding.end(),
                            {0x66, 0xd5, Payload, 0x58, 0x4c,
                             uint8_t(0x40 | ((Index & 7) << 3) | (Base & 7)),
                             0xf0});
            const auto Ops = lift(Arch::X64, Encoding);
            const uint64_t SegmentBase = Segment ? 0x10000 : 0;
            NdOpEmulator Emulator(
                memory(Arch::X64, SegmentBase + 0x4000,
                       bytes(std::array<uint64_t, 2>{0x4000000000000000,
                                                     0x4010000000000000})));
            Emulator.setStrictMode(true);
            ASSERT_TRUE(Emulator.setX86LinearAddressBits(48));
            Emulator.setMXCSR(0x1f80);
            if (Segment)
              ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(
                  Segment == 0x64 ? NdMemoryAddressSpace::X86FS
                                  : NdMemoryAddressSpace::X86GS,
                  SegmentBase));
            // Distinct values in all register banks expose a lost extension.
            for (unsigned R = 0; R != 32; ++R)
              Emulator.setRegister(gpr(R), 1);
            Emulator.setRegister(gpr(Base),
                                 Address32 ? 0x9abc0000fffffff0ULL : 0x3fd0);
            Emulator.setRegister(gpr(Index),
                                 Address32 ? 0xdef0000000002010ULL : 0x20);
            auto Left = bytes(std::array<uint64_t, 2>{0x3ff0000000000000,
                                                      0x4008000000000000});
            Left.resize(64, 0xa5);
            Emulator.setRegisterBytes(x86reg::XMM9, Left);
            Emulator.setRegisterBytes(x86reg::XMM25,
                                      std::vector<uint8_t>(64, 0xbb));
            for (const auto F : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                                 x86reg::SF, x86reg::OF})
              Emulator.setRegister(F, 1);
            ASSERT_NO_FATAL_FAILURE(execute(Emulator, Ops));
            const auto Actual = Emulator.getRegisterBytes(x86reg::XMM9);
            ASSERT_TRUE(Actual);
            auto Expected = bytes(std::array<uint64_t, 2>{0x4008000000000000,
                                                          0x401c000000000000});
            Expected.resize(64, 0xa5);
            EXPECT_EQ(*Actual, Expected);
            EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM25),
                      std::vector<uint8_t>(64, 0xbb));
            EXPECT_EQ(Emulator.getMXCSR(), 0x1f80u);
            for (const auto F : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                                 x86reg::SF, x86reg::OF})
              EXPECT_EQ(Emulator.getRegister(F), 1u);
          }
}

TEST(X86FPEncoding, Rex2ConversionExtendsGprsButNotVectors) {
  for (unsigned Index = 16; Index != 32; ++Index)
    for (const unsigned Width : {4u, 8u}) {
      SCOPED_TRACE(testing::Message() << Index << ':' << Width);
      const uint8_t W = Width == 8 ? 8 : 0;
      const auto ToFP = lift(
          Arch::X64, {0xf2, 0xd5, uint8_t(0xd4 | W | ((Index & 8) ? 1 : 0)),
                      0x2a, uint8_t(0xc8 | (Index & 7))});
      NdOpEmulator Emulator(memory(Arch::X64, 0x4000, {}));
      Emulator.setStrictMode(true);
      Emulator.setMXCSR(0x1f80);
      Emulator.setRegister(gpr(Index), uint64_t(-7));
      Emulator.setRegister(gpr(Index - 16), 99);
      Emulator.setRegisterBytes(x86reg::XMM9, std::vector<uint8_t>(64, 0xa5));
      execute(Emulator, ToFP);
      auto Expected = bytes(std::array<uint64_t, 1>{0xc01c000000000000});
      Expected.resize(64, 0xa5);
      EXPECT_EQ(Emulator.getRegisterBytes(x86reg::XMM9), Expected);
      const auto ToInt = lift(
          Arch::X64, {0xf2, 0xd5, uint8_t(0xd1 | W | ((Index & 8) ? 4 : 0)),
                      0x2c, uint8_t(0xc1 | ((Index & 7) << 3))});
      Emulator.setRegister(gpr(Index), 0xfeedface12345678ULL);
      execute(Emulator, ToInt);
      EXPECT_EQ(Emulator.getRegister(gpr(Index)),
                Width == 8 ? uint64_t(-7) : uint64_t(0xfffffff9));
      EXPECT_EQ(Emulator.getRegister(gpr(Index - 16)), 99u);
      EXPECT_EQ(Emulator.getMXCSR(), 0x1f80u);
    }
}

TEST(X86FPEncoding, IgnoredVexFieldsPreserveEveryRegisterAndCompletion) {
  const std::vector<std::vector<uint8_t>> Canonical = {
      {0xc4, 0xe1, 0x71, 0x58, 0x00},
      {0xc4, 0xe1, 0x71, 0x58, 0xc2},
      {0xc4, 0xe1, 0x79, 0x2f, 0x00},
      {0xc4, 0xe1, 0x73, 0x2a, 0x00},
      {0xc4, 0xe1, 0x7b, 0x2c, 0x00},
      {0xc4, 0xe1, 0x7a, 0xe6, 0x00},
      {0xc4, 0xe1, 0x71, 0xc2, 0x00, 0},
      {0xc4, 0xe1, 0x78, 0x53, 0x00},
      {0xc4, 0xe3, 0x79, 0x09, 0x00, 0},
      {0xc4, 0xe2, 0x71, 0x98, 0x00}}; // FMA shares the arithmetic parser.
  for (const auto &Normal : Canonical)
    for (const bool B : {false, true})
      for (const bool V : {false, true}) {
        // Only NDS forms have a used vvvv field; unused 1111 is reserved.
        const bool UsedV = Normal[2] == 0x71 || Normal[2] == 0x73;
        if (V && !UsedV)
          continue;
        auto Alternate = Normal;
        if (B)
          Alternate[1] &= ~0x20;
        if (V)
          Alternate[2] &= ~0x40;
        SCOPED_TRACE(testing::PrintToString(Alternate));
        // RCP's ordinary finite results depend on the CPU approximation.
        // Zero and infinity have exact architectural results in strict mode.
        const auto Data =
            Normal[3] == 0x53
                ? std::array<uint32_t, 8>{0, 0x80000000, 0x7f800000, 0xff800000}
                : std::array<uint32_t, 8>{0x3fc00000, 0x40200000, 0x40600000,
                                          0x40900000, 0x40b00000, 0x40d00000,
                                          0x40f00000, 0x41080000};
        NdOpEmulator Expected(memory(Arch::X86, 0x4000, bytes(Data)));
        NdOpEmulator Actual = Expected;
        for (auto *E : {&Expected, &Actual}) {
          E->setStrictMode(true);
          E->setMXCSR(0x1fa0);
          for (unsigned R = 0; R != 16; ++R) {
            E->setRegister(gpr(R), R == 0 ? 0x4000 : 0x12340000 + R);
            auto Values = bytes(std::array<uint64_t, 2>{
                0x3ff0000000000000ULL + (uint64_t(R) << 48),
                0x4008000000000000ULL + (uint64_t(R) << 48)});
            Values.resize(64, 0xa5 + R);
            E->setRegisterBytes(x86reg::vectorReg(R), Values);
          }
          for (const auto F : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                               x86reg::SF, x86reg::OF})
            E->setRegister(F, 1);
        }
        ASSERT_NO_FATAL_FAILURE(execute(Expected, lift(Arch::X86, Normal)));
        ASSERT_NO_FATAL_FAILURE(execute(Actual, lift(Arch::X86, Alternate)));
        for (unsigned R = 0; R != 16; ++R) {
          EXPECT_EQ(Actual.getRegister(gpr(R)), Expected.getRegister(gpr(R)));
          EXPECT_EQ(Actual.getRegisterBytes(x86reg::vectorReg(R)),
                    Expected.getRegisterBytes(x86reg::vectorReg(R)));
        }
        for (const auto F : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                             x86reg::SF, x86reg::OF})
          EXPECT_EQ(Actual.getRegister(F), Expected.getRegister(F));
        EXPECT_EQ(Actual.getMXCSR(), Expected.getMXCSR());
      }
}

TEST(X86FPEncoding, Rex2RawAndStructuredDisagreementRefusesBeforeEmission) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X64));
  const std::vector<uint8_t> Encoding = {0x67, 0x64, 0x66, 0xd5, 0xf5,
                                         0x58, 0x4c, 0x67, 0xf0};
  DecodedInsn Insn{};
  ASSERT_EQ(
      Decode.decodeOneForLift(Encoding.data(), Encoding.size(), 0x1000, Insn),
      Encoding.size());
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    cs_insn Raw = *Insn.Raw;
    cs_detail Detail = *Raw.detail;
    Raw.detail = &Detail;
    auto &X = Detail.x86;
    switch (Mutation) {
    case 0:
      X.rex2 ^= 0x10;
      break;
    case 1:
      X.operands[0].reg = X86_REG_XMM25;
      break;
    case 2:
      X.operands[1].mem.base = X86_REG_R15D;
      break;
    case 3:
      X.operands[1].mem.index = X86_REG_ESP;
      break;
    case 4:
      X.opcode[0] ^= 1;
      break;
    case 5:
      Raw.bytes[4] ^= 0x10;
      break;
    case 6:
      X.addr_size = 8;
      break;
    case 7:
      X.prefix[1] = 0x65;
      break;
    case 8:
      --Raw.size;
      break;
    case 9:
      ++X.operands[1].mem.disp;
      break;
    }
    X86Lifter L(Arch::X64);
    L.setStrict(true);
    std::vector<LowOp> Ops;
    EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
}

TEST(X86FPEncoding, UnusedVvvvAndLdsDisambiguationRemainReserved) {
  for (const bool Vex2 : {false, true}) {
    Decoder Decode;
    ASSERT_TRUE(Decode.init(Arch::X86));
    const std::vector<uint8_t> Encoding =
        Vex2 ? std::vector<uint8_t>{0xc5, 0xf1, 0x58, 0xc2}
             : std::vector<uint8_t>{0xc4, 0xe1, 0x79, 0x2f, 0x00};
    DecodedInsn Insn{};
    ASSERT_EQ(
        Decode.decodeOneForLift(Encoding.data(), Encoding.size(), 0x1000, Insn),
        Encoding.size());
    Insn.Raw->bytes[Vex2 ? 1 : 2] &= ~0x40;
    Insn.Raw->detail->x86.opcode[Vex2 ? 1 : 2] &= ~0x40;
    X86Lifter L(Arch::X86);
    L.setStrict(true);
    std::vector<LowOp> Ops;
    EXPECT_THROW(L.lift(Insn.Raw, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }
}

TEST(X86FPEncoding, Rex2MapModeAndPrefixBoundariesRemainReserved) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(Arch::X64));
  const std::vector<uint8_t> Add = {0x66, 0xd5, 0x90, 0x58, 0x08};
  DecodedInsn Insn{};
  ASSERT_EQ(Decode.decodeOneForLift(Add.data(), Add.size(), 0x1000, Insn),
            Add.size());
  const std::vector<std::vector<uint8_t>> Invalid = {
      {0x66, 0xd5, 0x10, 0x58, 0x08},       // Map zero cannot encode ADDPD.
      {0x66, 0xd5, 0x90, 0x0f, 0x58, 0x08}, // REX2 owns the map escape.
      {0x66, 0x48, 0xd5, 0x90, 0x58, 0x08}, // REX cannot precede REX2.
      {0x66, 0xd5},                         // Truncated prefix.
      Add};                                 // REX2 is not a 32-bit prefix.
  for (size_t I = 0; I != Invalid.size(); ++I) {
    SCOPED_TRACE(I);
    cs_insn Raw = *Insn.Raw;
    cs_detail Detail = *Raw.detail;
    Raw.detail = &Detail;
    const auto &Encoding = Invalid[I];
    Raw.size = Encoding.size();
    std::copy(Encoding.begin(), Encoding.end(), Raw.bytes);
    if (I == 0)
      Detail.x86.rex2 = 0x10;
    if (I == 1 || I == 2)
      ++Detail.x86.encoding.modrm_offset;
    if (I == 2)
      Detail.x86.rex = 0x48;
    if (I == 4)
      Detail.x86.addr_size = 4;
    X86Lifter L(I == 4 ? Arch::X86 : Arch::X64);
    L.setStrict(true);
    std::vector<LowOp> Ops;
    EXPECT_THROW(L.lift(&Raw, Ops), UnliftedInstruction);
    EXPECT_TRUE(Ops.empty());
  }

  const std::vector<uint8_t> Round = {0x66, 0x0f, 0x3a, 0x09, 0xc1, 0};
  ASSERT_EQ(Decode.decodeOneForLift(Round.data(), Round.size(), 0x1000, Insn),
            Round.size());
  // Map 3 instructions have no REX2 form, even with otherwise matching detail.
  Insn.Raw->bytes[1] = 0xd5;
  Insn.Raw->bytes[2] = 0x80;
  auto &X = Insn.Raw->detail->x86;
  X.rex2 = 0x80;
  std::fill_n(X.opcode, 4, 0);
  X.opcode[0] = 0x09;
  X86Lifter L(Arch::X64);
  L.setStrict(true);
  std::vector<LowOp> Ops;
  EXPECT_THROW(L.lift(Insn.Raw, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST(X86FPEncoding, LegacyMapEscapeIsCheckedEvenWhenDetailOmitsIt) {
  for (const auto Target : {Arch::X86, Arch::X64})
    for (const uint8_t Opcode : {0x08u, 0x09u, 0x0au, 0x0bu}) {
      const std::vector<uint8_t> Encoding = {0x66, 0x0f, 0x3a, Opcode, 0xc1, 4};
      EXPECT_FALSE(lift(Target, Encoding).empty());
      Decoder Decode;
      ASSERT_TRUE(Decode.init(Target));
      DecodedInsn Insn{};
      ASSERT_EQ(Decode.decodeOneForLift(Encoding.data(), Encoding.size(),
                                        0x1000, Insn),
                Encoding.size());
      // The opcode detail is unchanged, but map 2 cannot encode ROUND.
      Insn.Raw->bytes[2] = 0x38;
      std::vector<LowOp> Ops;
      EXPECT_THROW(Decode.liftToLow(Insn, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
}
} // namespace
