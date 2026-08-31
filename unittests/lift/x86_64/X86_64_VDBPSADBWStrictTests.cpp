//===- X86_64_VDBPSADBWStrictTests.cpp - exact EVEX DBPSADBW semantics ---===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <utility>
#include <vector>

using namespace neverd;

namespace {
constexpr va_t kAddress = 0x1000;

std::vector<LowOp> liftForArch(const std::vector<uint8_t> &Bytes, Arch A) {
  Decoder D;
  EXPECT_TRUE(D.init(A));
  DecodedInsn I{};
  EXPECT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, I),
            static_cast<int>(Bytes.size()));
  std::vector<LowOp> Ops;
  D.liftToLow(I, Ops);
  return Ops;
}

std::vector<LowOp> lift(const std::vector<uint8_t> &Bytes) {
  return liftForArch(Bytes, Arch::X64);
}

std::vector<uint8_t> reference(const std::vector<uint8_t> &A,
                               const std::vector<uint8_t> &B, uint8_t Imm) {
  std::vector<uint8_t> R(A.size(), 0), P(B.size(), 0);
  for (size_t Lane = 0; Lane < B.size(); Lane += 16)
    for (size_t Dword = 0; Dword < 4; ++Dword) {
      const size_t Source = Lane + ((Imm >> (2 * Dword)) & 3) * 4;
      std::copy_n(B.begin() + Source, 4, P.begin() + Lane + Dword * 4);
    }
  for (size_t Base = 0; Base < A.size(); Base += 8)
    for (size_t Word = 0; Word < 4; ++Word) {
      const size_t ABase = Base + (Word >= 2 ? 4 : 0);
      const size_t BBase = Base + Word;
      uint16_t Sum = 0;
      for (size_t Byte = 0; Byte < 4; ++Byte)
        Sum += static_cast<uint16_t>(
            std::abs(int(A[ABase + Byte]) - int(P[BBase + Byte])));
      R[Base + Word * 2] = static_cast<uint8_t>(Sum);
      R[Base + Word * 2 + 1] = static_cast<uint8_t>(Sum >> 8);
    }
  return R;
}

using DetailMutation = std::function<void(cs_insn &, cs_x86 &)>;

void expectDetailRejected(const std::vector<uint8_t> &Bytes,
                          const DetailMutation &Mutate, Arch A = Arch::X64) {
  Decoder D;
  ASSERT_TRUE(D.init(A));
  DecodedInsn I{};
  ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, I),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(I.Raw, nullptr);
  ASSERT_NE(I.Raw->detail, nullptr);
  Mutate(*I.Raw, I.Raw->detail->x86);
  std::vector<LowOp> Ops;
  EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

void expectMalformedRejected(const std::vector<uint8_t> &Bytes) {
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  DecodedInsn I{};
  const int Size = D.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, I);
  if (Size == 0)
    return;
  ASSERT_EQ(Size, static_cast<int>(Bytes.size()));
  std::vector<LowOp> Ops;
  EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST(X86VDBPSADBWStrict, ZmmHighRegistersPreserveImmediateAndLaneSemantics) {
  // vdbpsadbw zmm17, zmm18, zmm19, 0xe4
  const auto Ops = lift({0x62, 0xa3, 0x6d, 0x40, 0x42, 0xcb, 0xe4});
  ASSERT_FALSE(Ops.empty());
  std::vector<uint8_t> A(64), B(64);
  for (size_t I = 0; I < 64; ++I) {
    A[I] = static_cast<uint8_t>(3 * I + 1);
    B[I] = static_cast<uint8_t>(255 - 2 * I);
  }
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  const RegInfo RA = mapCapstoneReg(X86_REG_ZMM18);
  const RegInfo RB = mapCapstoneReg(X86_REG_ZMM19);
  const RegInfo RD = mapCapstoneReg(X86_REG_ZMM17);
  E.setRegisterBytes(RA.Offset, A);
  E.setRegisterBytes(RB.Offset, B);
  EXPECT_EQ(E.run(Ops), Ops.size());
  EXPECT_FALSE(E.skips().any());
  const auto Got = E.getRegisterBytes(RD.Offset);
  ASSERT_TRUE(Got);
  const auto Want = reference(A, B, 0xe4);
  EXPECT_TRUE(std::equal(Want.begin(), Want.end(), Got->begin()));
}

TEST(X86VDBPSADBWStrict, YmmZeroMaskUsesWordGranularity) {
  // vdbpsadbw ymm1{k2}{z}, ymm2, ymm3, 0x1b
  const auto Ops = lift({0x62, 0xf3, 0x6d, 0xaa, 0x42, 0xcb, 0x1b});
  ASSERT_FALSE(Ops.empty());
  std::vector<uint8_t> A(32), B(32), Old(32, 0xcc);
  for (size_t I = 0; I < 32; ++I) {
    A[I] = I * 5;
    B[I] = 200 - I * 3;
  }
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setRegisterBytes(mapCapstoneReg(X86_REG_YMM2).Offset, A);
  E.setRegisterBytes(mapCapstoneReg(X86_REG_YMM3).Offset, B);
  E.setRegisterBytes(mapCapstoneReg(X86_REG_YMM1).Offset, Old);
  E.setRegister(mapCapstoneReg(X86_REG_K2).Offset, 0x5a5a);
  EXPECT_EQ(E.run(Ops), Ops.size());
  EXPECT_FALSE(E.skips().any());
  auto Want = reference(A, B, 0x1b);
  for (size_t W = 0; W < 16; ++W)
    if (((0x5a5a >> W) & 1) == 0)
      Want[2 * W] = Want[2 * W + 1] = 0;
  const auto Got = E.getRegisterBytes(mapCapstoneReg(X86_REG_YMM1).Offset);
  ASSERT_TRUE(Got);
  EXPECT_TRUE(std::equal(Want.begin(), Want.end(), Got->begin()));
}

TEST(X86VDBPSADBWStrict, CompressedDispMemoryMergeWithoutFaultSuppression) {
  // vdbpsadbw xmm1{k3}, xmm2, [rax + 4*16], 0x39
  const auto Ops = lift({0x62, 0xf3, 0x6d, 0x0b, 0x42, 0x48, 0x04, 0x39});
  ASSERT_FALSE(Ops.empty());
  EXPECT_FALSE(
      lift({0x67, 0x62, 0xf3, 0x6d, 0x0b, 0x42, 0x48, 0x04, 0x39}).empty());
  EXPECT_FALSE(
      lift({0x64, 0x62, 0xf3, 0x6d, 0x0b, 0x42, 0x48, 0x04, 0x39}).empty());
  std::vector<uint8_t> A(16), B(16), Old(16, 0xa5);
  for (size_t I = 0; I < 16; ++I) {
    A[I] = 7 * I;
    B[I] = 240 - 9 * I;
  }

  BinaryImage Empty;
  Empty.Arch = Arch::X64;
  Empty.Bits = Bitness::Bits64;
  NdOpEmulator Suppressed(Empty);
  Suppressed.setStrictMode(true);
  Suppressed.setRegister(mapCapstoneReg(X86_REG_RAX).Offset, 0xdead0000);
  Suppressed.setRegister(mapCapstoneReg(X86_REG_K3).Offset, 0);
  Suppressed.setRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset, Old);
  Suppressed.setRegisterBytes(mapCapstoneReg(X86_REG_XMM2).Offset, A);
  // VDBPSADBW is exception type E4NF: writemask bits control destination
  // updates, but never suppress the Full Mem tuple read.
  EXPECT_LT(Suppressed.run(Ops), Ops.size());
  const auto SuppressedGot =
      Suppressed.getRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset);
  ASSERT_TRUE(SuppressedGot);
  EXPECT_TRUE(std::equal(Old.begin(), Old.end(), SuppressedGot->begin()));

  BinaryImage Fault;
  Fault.Arch = Arch::X64;
  Fault.Bits = Bitness::Bits64;
  Segment OneByte;
  OneByte.VA = 0x5004;
  OneByte.Size = 1;
  OneByte.Flags = SegmentFlags::Readable;
  OneByte.Data = {0x44};
  Fault.Segments.push_back(std::move(OneByte));
  NdOpEmulator Atomic(Fault);
  Atomic.setStrictMode(true);
  Atomic.setRegister(mapCapstoneReg(X86_REG_RAX).Offset, 0x5000 - 64);
  Atomic.setRegister(mapCapstoneReg(X86_REG_K3).Offset, 1);
  Atomic.setRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset, Old);
  Atomic.setRegisterBytes(mapCapstoneReg(X86_REG_XMM2).Offset, A);
  EXPECT_LT(Atomic.run(Ops), Ops.size());
  const auto AtomicGot =
      Atomic.getRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset);
  ASSERT_TRUE(AtomicGot);
  EXPECT_TRUE(std::equal(Old.begin(), Old.end(), AtomicGot->begin()));

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Segment Data;
  Data.VA = 0x4040;
  Data.Size = B.size();
  Data.Flags = SegmentFlags::Readable;
  Data.Data = B;
  Image.Segments.push_back(std::move(Data));
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setLoadCollect(true);
  E.setRegister(mapCapstoneReg(X86_REG_RAX).Offset, 0x4000);
  E.setRegister(mapCapstoneReg(X86_REG_K3).Offset, 0x81);
  E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset, Old);
  E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM2).Offset, A);
  EXPECT_EQ(E.run(Ops), Ops.size());
  EXPECT_FALSE(E.skips().any());
  auto Want = reference(A, B, 0x39);
  for (size_t W = 0; W < 8; ++W)
    if (((0x81 >> W) & 1) == 0) {
      Want[2 * W] = Old[2 * W];
      Want[2 * W + 1] = Old[2 * W + 1];
    }
  const auto Got = E.getRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset);
  ASSERT_TRUE(Got);
  EXPECT_TRUE(std::equal(Want.begin(), Want.end(), Got->begin()));
  ASSERT_EQ(E.getLoadRecords().size(), 1U);
  EXPECT_EQ(E.getLoadRecords()[0].Addr, 0x4040U);
  EXPECT_EQ(E.getLoadRecords()[0].Size, 16U);
}

TEST(X86VDBPSADBWStrict, RawEncodingAndDecoderDetailMustRemainBound) {
  const std::vector<uint8_t> MemoryBytes = {0x64, 0x67, 0x62, 0xf3, 0x6d, 0x0b,
                                            0x42, 0x4c, 0xb5, 0x04, 0x39};
  const std::vector<DetailMutation> Mutations = {
      [](cs_insn &, cs_x86 &X) { X.operands[0].reg = X86_REG_XMM2; },
      [](cs_insn &, cs_x86 &X) { X.operands[1].reg = X86_REG_K2; },
      [](cs_insn &, cs_x86 &X) { X.operands[2].reg = X86_REG_XMM3; },
      [](cs_insn &, cs_x86 &X) { X.operands[3].mem.segment = X86_REG_GS; },
      [](cs_insn &, cs_x86 &X) { X.operands[3].mem.base = X86_REG_EAX; },
      [](cs_insn &, cs_x86 &X) { X.operands[3].mem.index = X86_REG_EDI; },
      [](cs_insn &, cs_x86 &X) { X.operands[3].mem.scale = 2; },
      [](cs_insn &, cs_x86 &X) { X.operands[3].mem.disp += 16; },
      [](cs_insn &, cs_x86 &X) { X.disp += 16; },
      [](cs_insn &, cs_x86 &X) { ++X.encoding.disp_offset; },
      [](cs_insn &, cs_x86 &X) { X.sib ^= 1; },
      [](cs_insn &, cs_x86 &X) { X.operands[4].imm ^= 1; },
      [](cs_insn &, cs_x86 &X) { --X.encoding.imm_offset; },
      [](cs_insn &, cs_x86 &X) { X.operands[1].avx_zero_opmask = true; },
      [](cs_insn &, cs_x86 &X) { X.prefix[1] = 0; },
      [](cs_insn &, cs_x86 &X) { X.addr_size = 8; },
      [](cs_insn &, cs_x86 &X) { X.opcode[3] ^= 1; },
      [](cs_insn &I, cs_x86 &) { --I.size; },
  };
  for (const DetailMutation &Mutate : Mutations)
    expectDetailRejected(MemoryBytes, Mutate);
  expectMalformedRejected(
      {0x67, 0x67, 0x62, 0xf3, 0x6d, 0x0b, 0x42, 0x08, 0x39});

  const std::vector<uint8_t> RegisterBytes = {0x62, 0xa3, 0x6d, 0x40,
                                              0x42, 0xcb, 0xe4};
  expectDetailRejected(RegisterBytes, [](cs_insn &, cs_x86 &X) {
    X.operands[2].reg = X86_REG_ZMM20;
  });

  expectDetailRejected(
      {0x62, 0xf3, 0x6d, 0x0b, 0x42, 0x08, 0x39},
      [](cs_insn &I, cs_x86 &X) {
        I.bytes[1] &= static_cast<uint8_t>(~0x10U);
        X.opcode[1] = I.bytes[1];
        X.operands[0].reg = X86_REG_XMM17;
      },
      Arch::X86);
}

TEST(X86VDBPSADBWStrict, X86Address32AndAddress16ExecuteExactly) {
  const std::array<std::pair<std::vector<uint8_t>, bool>, 2> Cases = {{
      {{0x64, 0x62, 0xf3, 0x6d, 0x0b, 0x42, 0x08, 0x39}, false},
      {{0x67, 0x62, 0xf3, 0x6d, 0x0b, 0x42, 0x08, 0x39}, true},
  }};
  for (const auto &[Bytes, Address16] : Cases) {
    const auto Ops = liftForArch(Bytes, Arch::X86);
    ASSERT_FALSE(Ops.empty());
    std::vector<uint8_t> A(16), B(16), Old(16, 0xcc);
    for (size_t I = 0; I < 16; ++I) {
      A[I] = static_cast<uint8_t>(I * 7);
      B[I] = static_cast<uint8_t>(240 - I * 5);
    }
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Segment Data;
    Data.VA = 0x5000;
    Data.Size = B.size();
    Data.Flags = SegmentFlags::Readable;
    Data.Data = B;
    Image.Segments.push_back(std::move(Data));
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    if (Address16) {
      E.setRegister(mapCapstoneReg(X86_REG_BX).Offset, 0x12344000U);
      E.setRegister(mapCapstoneReg(X86_REG_SI).Offset, 0xabcd1000U);
    } else {
      E.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, 0x1000);
      E.setRegister(mapCapstoneReg(X86_REG_EAX).Offset, UINT64_C(0x100004000));
    }
    E.setRegister(mapCapstoneReg(X86_REG_K3).Offset, 0xff);
    E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset, Old);
    E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM2).Offset, A);
    EXPECT_EQ(E.run(Ops), Ops.size());
    EXPECT_FALSE(E.skips().any());
    const auto Got = E.getRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset);
    ASSERT_TRUE(Got);
    const auto Want = reference(A, B, 0x39);
    EXPECT_TRUE(std::equal(Want.begin(), Want.end(), Got->begin()));
  }
}
} // namespace
