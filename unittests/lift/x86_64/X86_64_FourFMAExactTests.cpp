#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>
#include <vector>

using namespace neverd;
namespace {
std::vector<LowOp> liftForArch(const std::vector<uint8_t> &B, Arch A) {
  Decoder D;
  EXPECT_TRUE(D.init(A));
  DecodedInsn I{};
  EXPECT_EQ(D.decodeOneForLift(B.data(), B.size(), 0x1000, I), (int)B.size());
  std::vector<LowOp> O;
  D.liftToLow(I, O);
  return O;
}
std::vector<LowOp> lift(const std::vector<uint8_t> &B) {
  return liftForArch(B, Arch::X64);
}
std::vector<uint8_t> floats(std::initializer_list<float> V, size_t N) {
  std::vector<uint8_t> R(N * 4);
  size_t I = 0;
  for (float F : V) {
    const uint32_t X = std::bit_cast<uint32_t>(F);
    std::memcpy(R.data() + 4 * I++, &X, 4);
  }
  return R;
}
BinaryImage image(const std::vector<uint8_t> &M) {
  BinaryImage I;
  I.Arch = Arch::X64;
  I.Bits = Bitness::Bits64;
  Segment S;
  S.VA = 0x4000;
  S.Size = M.size();
  S.Flags = SegmentFlags::Readable;
  S.Data = M;
  I.Segments.push_back(std::move(S));
  return I;
}
float getf(const std::vector<uint8_t> &V, size_t I) {
  uint32_t X;
  std::memcpy(&X, V.data() + 4 * I, 4);
  return std::bit_cast<float>(X);
}

using DetailMutation = std::function<void(cs_insn &, cs_x86 &)>;

void expectDetailRejected(const std::vector<uint8_t> &Bytes,
                          const DetailMutation &Mutate, Arch A = Arch::X64) {
  Decoder D;
  ASSERT_TRUE(D.init(A));
  DecodedInsn I{};
  ASSERT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I),
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
  const int Size = D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I);
  if (Size == 0)
    return;
  ASSERT_EQ(Size, static_cast<int>(Bytes.size()));
  std::vector<LowOp> Ops;
  EXPECT_THROW(D.liftToLow(I, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

TEST(X86FourFMAExact, PackedPositiveAndHighGroupNegativeAreSequentiallyFused) {
  EXPECT_TRUE(isSideeffectIntrinsic(Intrinsic::X86FourFMA));
  const std::vector<uint8_t> Memory = floats({2, 3, 4, 5}, 4);
  for (const auto &C : std::vector<std::pair<std::vector<uint8_t>, bool>>{
           {{0x62, 0xf2, 0x5f, 0x48, 0x9a, 0x08}, false},
           {{0x62, 0xe2, 0x5f, 0xc2, 0xaa, 0x08}, true}}) {
    const auto Ops = lift(C.first);
    ASSERT_FALSE(Ops.empty());
    BinaryImage Image = image(Memory);
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    const unsigned Base = C.second ? 20 : 4;
    const x86_reg Dst = C.second ? X86_REG_ZMM17 : X86_REG_ZMM1;
    std::vector<uint8_t> Old(64), Want(64);
    for (unsigned L = 0; L < 16; ++L) {
      float A = 1.0f + L;
      std::memcpy(Old.data() + 4 * L, &A, 4);
      float R = A;
      for (unsigned I = 0; I < 4; ++I)
        R = std::fma(C.second ? -(float)(I + 1) : (float)(I + 1),
                     (float)(I + 2), R);
      std::memcpy(Want.data() + 4 * L, &R, 4);
    }
    E.setRegisterBytes(mapCapstoneReg(Dst).Offset, Old);
    for (unsigned I = 0; I < 4; ++I) {
      std::vector<uint8_t> S(64);
      float F = I + 1;
      for (unsigned L = 0; L < 16; ++L)
        std::memcpy(S.data() + 4 * L, &F, 4);
      E.setRegisterBytes(
          mapCapstoneReg((x86_reg)(X86_REG_ZMM0 + Base + I)).Offset, S);
    }
    E.setRegister(mapCapstoneReg(X86_REG_RAX).Offset, 0x4000);
    if (C.second)
      E.setRegister(mapCapstoneReg(X86_REG_K2).Offset, 0xffff);
    EXPECT_EQ(E.run(Ops), Ops.size());
    EXPECT_FALSE(E.skips().any());
    auto Got = E.getRegisterBytes(mapCapstoneReg(Dst).Offset);
    ASSERT_TRUE(Got);
    EXPECT_TRUE(std::equal(Want.begin(), Want.end(), Got->begin()));
  }
}

TEST(X86FourFMAExact, ScalarMaskPreservesUpperAndSuppressesMemory) {
  for (uint8_t Length :
       {uint8_t{0x00}, uint8_t{0x20}, uint8_t{0x40}, uint8_t{0x60}}) {
    SCOPED_TRACE(static_cast<unsigned>(Length));
    const auto Ops = lift(
        {0x62, 0xe2, 0x5f, static_cast<uint8_t>(Length | 0x02), 0xab, 0x08});
    ASSERT_FALSE(Ops.empty());
    BinaryImage Empty;
    Empty.Arch = Arch::X64;
    Empty.Bits = Bitness::Bits64;
    NdOpEmulator E(Empty);
    E.setStrictMode(true);
    auto Low = floats({9, 10, 11, 12}, 4);
    std::vector<uint8_t> Old(64, 0xcc);
    std::copy(Low.begin(), Low.end(), Old.begin());
    E.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM17).Offset, Old);
    E.setRegister(mapCapstoneReg(X86_REG_RAX).Offset, 0xdead0000);
    E.setRegister(mapCapstoneReg(X86_REG_K2).Offset, 0);
    const uint32_t Before = E.getMXCSR();
    EXPECT_EQ(E.run(Ops), Ops.size());
    EXPECT_EQ(E.getMXCSR(), Before);
    auto Got = E.getRegisterBytes(mapCapstoneReg(X86_REG_ZMM17).Offset);
    ASSERT_TRUE(Got);
    EXPECT_TRUE(std::equal(Low.begin(), Low.end(), Got->begin()));
    EXPECT_TRUE(std::all_of(Got->begin() + 16, Got->begin() + 64,
                            [](uint8_t B) { return B == 0; }));
    EXPECT_EQ(E.getRegister(mapCapstoneReg(X86_REG_K2).Offset), 0u);
  }
}

TEST(X86FourFMAExact, ScalarNaNPayloadAndSegmentedMemoryAreArchitectural) {
  const auto Ops = lift({0x64, 0x62, 0xf2, 0x5f, 0x08, 0x9b, 0x08});
  ASSERT_FALSE(Ops.empty());
  std::vector<uint8_t> M(16, 0);
  float One = 1.0f;
  std::memcpy(M.data(), &One, 4);
  BinaryImage I = image(M);
  NdOpEmulator E(I);
  E.setStrictMode(true);
  E.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, 0x4000);
  E.setRegister(mapCapstoneReg(X86_REG_RAX).Offset, 0);
  std::vector<uint8_t> Old(16, 0);
  float Acc = 2.0f;
  std::memcpy(Old.data(), &Acc, 4);
  E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset, Old);
  for (unsigned N = 0; N < 4; ++N)
    E.setRegisterBytes(mapCapstoneReg((x86_reg)(X86_REG_XMM4 + N)).Offset,
                       std::vector<uint8_t>(16, 0));
  const uint32_t SNaN = 0x7fa12345U;
  std::vector<uint8_t> First(16, 0);
  std::memcpy(First.data(), &SNaN, 4);
  E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM4).Offset, First);
  EXPECT_EQ(E.run(Ops), Ops.size());
  EXPECT_NE(E.getMXCSR() & 1U, 0U);
  auto Got = E.getRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset);
  ASSERT_TRUE(Got);
  uint32_t Bits = 0;
  std::memcpy(&Bits, Got->data(), 4);
  EXPECT_EQ(Bits, 0x7fe12345U);

  // Address-size override before EVEX is a legal x86-64 memory form.
  EXPECT_FALSE(lift({0x67, 0x62, 0xf2, 0x5f, 0x08, 0x9b, 0x08}).empty());
}

TEST(X86FourFMAExact, NegativeFormsPreserveOriginalNaNSignAndPayload) {
  const auto Ops = lift({0x62, 0xf2, 0x5f, 0x08, 0xab, 0x08});
  ASSERT_FALSE(Ops.empty());
  const std::array<uint32_t, 2> Inputs = {0xffc12345U, 0xffa12345U};
  const std::array<uint32_t, 2> Expected = {0xffc12345U, 0xffe12345U};
  for (size_t Case = 0; Case < Inputs.size(); ++Case) {
    std::vector<uint8_t> Memory(16, 0);
    const uint32_t One = std::bit_cast<uint32_t>(1.0f);
    for (unsigned I = 0; I < 4; ++I)
      std::memcpy(Memory.data() + I * 4, &One, 4);
    BinaryImage Image = image(Memory);
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setRegister(mapCapstoneReg(X86_REG_RAX).Offset, 0x4000);
    std::vector<uint8_t> Old(16, 0);
    const uint32_t Two = std::bit_cast<uint32_t>(2.0f);
    std::memcpy(Old.data(), &Two, 4);
    E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset, Old);
    for (unsigned I = 0; I < 4; ++I)
      E.setRegisterBytes(
          mapCapstoneReg(static_cast<x86_reg>(X86_REG_XMM4 + I)).Offset,
          std::vector<uint8_t>(16, 0));
    std::vector<uint8_t> First(16, 0);
    std::memcpy(First.data(), &Inputs[Case], 4);
    E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM4).Offset, First);
    EXPECT_EQ(E.run(Ops), Ops.size());
    const auto Got = E.getRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset);
    ASSERT_TRUE(Got);
    uint32_t Bits = 0;
    std::memcpy(&Bits, Got->data(), 4);
    EXPECT_EQ(Bits, Expected[Case]);
    EXPECT_EQ(E.getMXCSR() & 1U, Case == 0 ? 0U : 1U);
  }
}

TEST(X86FourFMAExact, InvalidPrecomputationPrecedesDenormalExceptions) {
  const auto Ops = lift({0x62, 0xf2, 0x5f, 0x08, 0x9b, 0x08});
  ASSERT_FALSE(Ops.empty());
  struct Case {
    uint32_t Source;
    uint32_t Multiplier;
    uint32_t Addend;
    uint32_t Mxcsr;
    bool Completes;
    uint32_t Flags;
  };
  const std::array<Case, 6> Cases = {{
      {0x7fc12345U, 0x00000001U, 0x3f800000U, 0x1e80U, true, 0},
      {0x7fa12345U, 0x00000001U, 0x3f800000U, 0x1f80U, true, 1},
      {0x7fa12345U, 0x00000001U, 0x3f800000U, 0x1e00U, false, 1},
      {0x00000000U, 0x7f800000U, 0x00000001U, 0x1f80U, true, 1},
      {0x00000000U, 0x7f800000U, 0x00000001U, 0x1e00U, false, 1},
      {0x00000001U, 0x00800000U, 0x00000000U, 0x1e80U, false, 2},
  }};
  for (const Case &C : Cases) {
    std::vector<uint8_t> Memory(16, 0);
    std::memcpy(Memory.data(), &C.Multiplier, 4);
    const uint32_t One = 0x3f800000U;
    for (unsigned I = 1; I < 4; ++I)
      std::memcpy(Memory.data() + I * 4, &One, 4);
    BinaryImage Image = image(Memory);
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setMXCSR(C.Mxcsr);
    E.setRegister(mapCapstoneReg(X86_REG_RAX).Offset, 0x4000);
    std::vector<uint8_t> Old(16, 0), First(16, 0);
    std::memcpy(Old.data(), &C.Addend, 4);
    std::memcpy(First.data(), &C.Source, 4);
    E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset, Old);
    E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM4).Offset, First);
    for (unsigned I = 1; I < 4; ++I)
      E.setRegisterBytes(
          mapCapstoneReg(static_cast<x86_reg>(X86_REG_XMM4 + I)).Offset,
          std::vector<uint8_t>(16, 0));
    const size_t Executed = E.run(Ops);
    if (C.Completes)
      EXPECT_EQ(Executed, Ops.size());
    else
      EXPECT_LT(Executed, Ops.size());
    EXPECT_EQ(E.getMXCSR() & 0x3fU, C.Flags);
  }
}

TEST(X86FourFMAExact, ExactTinyResultObeysUnderflowMaskBeforeFTZ) {
  const auto Ops = lift({0x62, 0xf2, 0x5f, 0x08, 0x9b, 0x08});
  ASSERT_FALSE(Ops.empty());
  struct Case {
    uint32_t Mxcsr;
    bool Completes;
    uint32_t Result;
    uint32_t Flags;
  };
  const std::array<Case, 4> Cases = {{{0x1f80U, true, 0x00400000U, 0},
                                      {0x9f80U, true, 0, 0x30U},
                                      {0x1780U, false, 0, 0x10U},
                                      {0x9780U, false, 0, 0x10U}}};
  for (const Case &C : Cases) {
    std::vector<uint8_t> Memory(16, 0);
    const uint32_t One = 0x3f800000U;
    for (unsigned I = 0; I < 3; ++I)
      std::memcpy(Memory.data() + I * 4, &One, 4);
    const uint32_t Half = 0x3f000000U;
    std::memcpy(Memory.data() + 12, &Half, 4);
    BinaryImage Image = image(Memory);
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    E.setMXCSR(C.Mxcsr);
    E.setRegister(mapCapstoneReg(X86_REG_RAX).Offset, 0x4000);
    std::vector<uint8_t> Old(16, 0), First(16, 0);
    const uint32_t MinNormal = 0x00800000U;
    std::memcpy(First.data(), &MinNormal, 4);
    E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset, Old);
    for (unsigned I = 0; I < 3; ++I)
      E.setRegisterBytes(
          mapCapstoneReg(static_cast<x86_reg>(X86_REG_XMM4 + I)).Offset,
          std::vector<uint8_t>(16, 0));
    E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM7).Offset, First);
    const size_t Executed = E.run(Ops);
    if (C.Completes) {
      EXPECT_EQ(Executed, Ops.size());
      const auto Got = E.getRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset);
      ASSERT_TRUE(Got);
      uint32_t Bits = 0;
      std::memcpy(&Bits, Got->data(), 4);
      EXPECT_EQ(Bits, C.Result);
      EXPECT_EQ(E.getMXCSR() & 0x3fU, C.Flags);
    } else {
      EXPECT_LT(Executed, Ops.size());
      EXPECT_EQ(E.getMXCSR() & 0x3fU, C.Flags);
    }
  }
}

TEST(X86FourFMAExact, PackedPreFaultSuppressesEveryLanePostComputation) {
  const auto Ops = lift({0x62, 0xf2, 0x5f, 0x48, 0x9a, 0x08});
  ASSERT_FALSE(Ops.empty());
  std::vector<uint8_t> Memory(16, 0);
  const uint32_t MaxFinite = 0x7f7fffffU;
  const uint32_t One = 0x3f800000U;
  std::memcpy(Memory.data(), &MaxFinite, 4);
  for (unsigned I = 1; I < 4; ++I)
    std::memcpy(Memory.data() + I * 4, &One, 4);
  BinaryImage Image = image(Memory);
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  E.setMXCSR(0x1e80U); // Denormal unmasked; all post exceptions masked.
  E.setRegister(mapCapstoneReg(X86_REG_RAX).Offset, 0x4000);
  E.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset,
                     std::vector<uint8_t>(64, 0));
  std::vector<uint8_t> First(64, 0);
  const uint32_t MinDenormal = 1;
  std::memcpy(First.data(), &MinDenormal, 4);
  std::memcpy(First.data() + 4, &MaxFinite, 4);
  E.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM4).Offset, First);
  for (unsigned I = 1; I < 4; ++I)
    E.setRegisterBytes(
        mapCapstoneReg(static_cast<x86_reg>(X86_REG_ZMM4 + I)).Offset,
        std::vector<uint8_t>(64, 0));
  EXPECT_LT(E.run(Ops), Ops.size());
  EXPECT_EQ(E.getMXCSR() & 0x3fU, 1U << 1);
}

TEST(X86FourFMAExact, X86Address32AndAddress16ExecuteExactly) {
  const std::array<std::pair<std::vector<uint8_t>, bool>, 2> Cases = {{
      {{0x64, 0x62, 0xf2, 0x5f, 0x08, 0x9b, 0x08}, false},
      {{0x67, 0x62, 0xf2, 0x5f, 0x08, 0x9b, 0x08}, true},
  }};
  for (const auto &[Bytes, Address16] : Cases) {
    const auto Ops = liftForArch(Bytes, Arch::X86);
    ASSERT_FALSE(Ops.empty());
    std::vector<uint8_t> Memory(16, 0);
    const uint32_t One = 0x3f800000U;
    for (unsigned I = 0; I < 4; ++I)
      std::memcpy(Memory.data() + I * 4, &One, 4);
    BinaryImage Image;
    Image.Arch = Arch::X86;
    Image.Bits = Bitness::Bits32;
    Segment Data;
    Data.VA = 0x5000;
    Data.Size = Memory.size();
    Data.Flags = SegmentFlags::Readable;
    Data.Data = Memory;
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
    std::vector<uint8_t> Old(16, 0), First(16, 0);
    const uint32_t Two = 0x40000000U;
    std::memcpy(Old.data(), &Two, 4);
    std::memcpy(First.data(), &One, 4);
    E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset, Old);
    E.setRegisterBytes(mapCapstoneReg(X86_REG_XMM4).Offset, First);
    for (unsigned I = 1; I < 4; ++I)
      E.setRegisterBytes(
          mapCapstoneReg(static_cast<x86_reg>(X86_REG_XMM4 + I)).Offset,
          std::vector<uint8_t>(16, 0));
    EXPECT_EQ(E.run(Ops), Ops.size());
    const auto Got = E.getRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset);
    ASSERT_TRUE(Got);
    uint32_t Bits = 0;
    std::memcpy(&Bits, Got->data(), 4);
    EXPECT_EQ(Bits, 0x40400000U);
  }
}

TEST(X86FourFMAExact, RawEncodingAndDecoderDetailMustRemainBound) {
  const std::vector<uint8_t> Bytes = {0x64, 0x67, 0x62, 0xf2, 0x5f,
                                      0x09, 0x9b, 0x4c, 0xb5, 0x02};
  const std::vector<DetailMutation> Mutations = {
      [](cs_insn &, cs_x86 &X) { X.operands[0].reg = X86_REG_XMM2; },
      [](cs_insn &, cs_x86 &X) { X.operands[1].reg = X86_REG_K2; },
      [](cs_insn &, cs_x86 &X) { X.operands[2].reg = X86_REG_XMM8; },
      [](cs_insn &, cs_x86 &X) { X.operands[3].mem.segment = X86_REG_GS; },
      [](cs_insn &, cs_x86 &X) { X.operands[3].mem.base = X86_REG_EAX; },
      [](cs_insn &, cs_x86 &X) { X.operands[3].mem.index = X86_REG_EDI; },
      [](cs_insn &, cs_x86 &X) { X.operands[3].mem.scale = 2; },
      [](cs_insn &, cs_x86 &X) { X.operands[3].mem.disp += 16; },
      [](cs_insn &, cs_x86 &X) { X.disp += 16; },
      [](cs_insn &, cs_x86 &X) { ++X.encoding.disp_offset; },
      [](cs_insn &, cs_x86 &X) { X.sib ^= 1; },
      [](cs_insn &, cs_x86 &X) { X.prefix[1] = 0; },
      [](cs_insn &, cs_x86 &X) { X.addr_size = 8; },
      [](cs_insn &, cs_x86 &X) { X.opcode[3] ^= 1; },
      [](cs_insn &I, cs_x86 &) { --I.size; },
  };
  for (const DetailMutation &Mutate : Mutations)
    expectDetailRejected(Bytes, Mutate);
  expectMalformedRejected({0x64, 0x65, 0x62, 0xf2, 0x5f, 0x08, 0x9b, 0x08});

  expectDetailRejected(
      {0x62, 0xf2, 0x5f, 0x08, 0x9b, 0x08},
      [](cs_insn &I, cs_x86 &X) {
        I.bytes[1] &= static_cast<uint8_t>(~0x10U);
        X.opcode[1] = I.bytes[1];
        X.operands[0].reg = X86_REG_XMM17;
      },
      Arch::X86);
}
} // namespace
