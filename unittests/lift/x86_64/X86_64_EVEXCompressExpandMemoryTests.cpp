#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <cstring>
#include <array>
#include <algorithm>

using namespace neverd;

namespace {

struct Family {
  const char *Name;
  uint8_t ExpandOpcode;
  uint8_t CompressOpcode;
  uint8_t Element;
  bool W;
};
constexpr std::array<Family, 6> Families{{
    {"B", 0x62, 0x63, 1, false}, {"W", 0x62, 0x63, 2, true},
    {"D", 0x89, 0x8b, 4, false}, {"Q", 0x89, 0x8b, 8, true},
    {"PS", 0x88, 0x8a, 4, false}, {"PD", 0x88, 0x8a, 8, true},
}};

std::vector<uint8_t> encoding(const Family &F, bool Compress, bool Zero,
                              uint16_t VectorSize = 64) {
  const uint8_t Length = VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
  return {0x62, 0xf2, static_cast<uint8_t>(F.W ? 0xfd : 0x7d),
          static_cast<uint8_t>(0x0a | Length |
                               (!Compress && Zero ? 0x80 : 0)),
          static_cast<uint8_t>(Compress ? F.CompressOpcode : F.ExpandOpcode),
          static_cast<uint8_t>(Compress ? 0x18 : 0x08)};
}

std::vector<LowOp> lift(const std::vector<uint8_t> &Bytes) {
  Decoder D;
  EXPECT_TRUE(D.init(Arch::X64));
  DecodedInsn I{};
  EXPECT_EQ(D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I),
            static_cast<int>(Bytes.size()));
  std::vector<LowOp> Ops;
  EXPECT_NO_THROW(D.liftToLow(I, Ops));
  return Ops;
}

BinaryImage image(uint64_t Base, size_t Size) {
  BinaryImage I;
  I.Arch = Arch::X64;
  I.Bits = Bitness::Bits64;
  Segment S;
  S.VA = Base;
  S.Size = Size;
  S.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  S.Data.resize(Size);
  I.Segments.push_back(std::move(S));
  return I;
}

TEST(X86EVEXCompressExpandMemory, ExpandIsContiguousMaskedAndFaultAtomic) {
  const auto Ops = lift({0x62, 0xf2, 0x7d, 0xca, 0x89, 0x08});
  ASSERT_FALSE(Ops.empty());
  BinaryImage I = image(0x4000, 8);
  const uint32_t A = 0x11223344, B = 0x55667788;
  std::memcpy(I.Segments[0].Data.data(), &A, 4);
  std::memcpy(I.Segments[0].Data.data() + 4, &B, 4);
  NdOpEmulator E(I);
  E.setStrictMode(true);
  E.setLoadCollect(true);
  E.setRegister(x86reg::RAX, 0x4000);
  E.setRegister(x86reg::K2, 0x5);
  E.setRegisterBytes(x86reg::vectorReg(1), std::vector<uint8_t>(64, 0xaa));
  for (const LowOp &Op : Ops)
    ASSERT_TRUE(E.step(Op));
  const auto R = E.getRegisterBytes(x86reg::vectorReg(1));
  ASSERT_TRUE(R);
  ASSERT_EQ(R->size(), 64u);
  EXPECT_EQ(*reinterpret_cast<const uint32_t *>(R->data()), A);
  EXPECT_EQ(*reinterpret_cast<const uint32_t *>(R->data() + 4), 0u);
  EXPECT_EQ(*reinterpret_cast<const uint32_t *>(R->data() + 8), B);
  ASSERT_EQ(E.getLoadRecords().size(), 2u);
  EXPECT_EQ(E.getLoadRecords()[0].Addr, 0x4000u);
  EXPECT_EQ(E.getLoadRecords()[1].Addr, 0x4004u);

  NdOpEmulator Suppressed(I);
  Suppressed.setStrictMode(true);
  Suppressed.setLoadCollect(true);
  Suppressed.setRegister(x86reg::RAX, 0x9000);
  Suppressed.setRegister(x86reg::K2, 0);
  for (const LowOp &Op : Ops)
    ASSERT_TRUE(Suppressed.step(Op));
  EXPECT_TRUE(Suppressed.getLoadRecords().empty());
}

TEST(X86EVEXCompressExpandMemory, CompressStoresPackedActiveLanes) {
  const auto Ops = lift({0x62, 0xf2, 0x7d, 0x4a, 0x8b, 0x18});
  ASSERT_FALSE(Ops.empty());
  BinaryImage I = image(0x5000, 64);
  NdOpEmulator E(I);
  E.setStrictMode(true);
  E.setRegister(x86reg::RAX, 0x5000);
  E.setRegister(x86reg::K2, 0x5);
  std::vector<uint8_t> Source(64);
  const uint32_t A = 0x10203040, B = 0xa0b0c0d0;
  std::memcpy(Source.data(), &A, 4);
  std::memcpy(Source.data() + 8, &B, 4);
  E.setRegisterBytes(x86reg::vectorReg(3), Source);
  for (const LowOp &Op : Ops)
    ASSERT_TRUE(E.step(Op));
  for (unsigned Index = 0; Index != 2; ++Index) {
    LowOp L;
    L.Opcode = NdOp::LOAD;
    L.Output = NdVar::tmp(900 + Index, 4);
    L.addInput(NdVar::cst(0, 8));
    L.addInput(NdVar::cst(0x5000 + Index * 4, 8));
    ASSERT_TRUE(E.step(L));
  }
  EXPECT_EQ(E.getRegister(900), std::optional<uint64_t>(A));
  EXPECT_EQ(E.getRegister(901), std::optional<uint64_t>(B));
}

TEST(X86EVEXCompressExpandMemory, ExpandFaultDoesNotCommitDestinationOrLog) {
  const auto Ops = lift({0x62, 0xf2, 0x7d, 0x4a, 0x89, 0x08});
  BinaryImage I = image(0x5100, 4);
  const uint32_t First = 0x11223344;
  std::memcpy(I.Segments[0].Data.data(), &First, sizeof(First));
  NdOpEmulator E(I);
  E.setStrictMode(true);
  E.setLoadCollect(true);
  E.setRegister(x86reg::RAX, 0x5100);
  E.setRegister(x86reg::K2, 0x5);
  const std::vector<uint8_t> OldDestination(64, 0xa5);
  E.setRegisterBytes(x86reg::vectorReg(1), OldDestination);

  bool Failed = false;
  for (const LowOp &Op : Ops) {
    if (!E.step(Op)) {
      Failed = true;
      break;
    }
  }
  EXPECT_TRUE(Failed);
  EXPECT_EQ(E.getRegisterBytes(x86reg::vectorReg(1)), OldDestination);
  EXPECT_TRUE(E.getLoadRecords().empty());
}

TEST(X86EVEXCompressExpandMemory, CompressFaultKeepsEarlierStores) {
  const auto Ops = lift({0x62, 0xf2, 0x7d, 0x4a, 0x8b, 0x18});
  BinaryImage I = image(0x5200, 4);
  NdOpEmulator E(I);
  E.setStrictMode(true);
  E.setRegister(x86reg::RAX, 0x5200);
  E.setRegister(x86reg::K2, 0x5);
  const uint32_t First = 0x10203040, Second = 0xa0b0c0d0;
  std::vector<uint8_t> Source(64);
  std::memcpy(Source.data(), &First, sizeof(First));
  std::memcpy(Source.data() + 8, &Second, sizeof(Second));
  E.setRegisterBytes(x86reg::vectorReg(3), Source);

  bool Failed = false;
  for (const LowOp &Op : Ops) {
    if (!E.step(Op)) {
      Failed = true;
      break;
    }
  }
  EXPECT_TRUE(Failed);
  LowOp Probe;
  Probe.Opcode = NdOp::LOAD;
  Probe.Output = NdVar::tmp(950, 4);
  Probe.addInput(NdVar::cst(0, 8));
  Probe.addInput(NdVar::cst(0x5200, 8));
  ASSERT_TRUE(E.step(Probe));
  EXPECT_EQ(E.getRegister(950), std::optional<uint64_t>(First));
}

TEST(X86EVEXCompressExpandMemory, AllTwelveFamiliesLiftAndExecute) {
  for (const Family &F : Families) {
    SCOPED_TRACE(F.Name);
    for (uint16_t VectorSize : {16, 32, 64}) {
      SCOPED_TRACE(VectorSize);
      for (bool Compress : {false, true}) {
      SCOPED_TRACE(Compress ? "compress" : "expand");
      for (bool Zero : (Compress ? std::initializer_list<bool>{false}
                                 : std::initializer_list<bool>{false, true})) {
        SCOPED_TRACE(Zero ? "zero" : "merge");
        const auto Ops = lift(encoding(F, Compress, Zero, VectorSize));
        ASSERT_FALSE(Ops.empty());
        ASSERT_TRUE(std::any_of(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
          return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 0 &&
                 Op.Inputs[0].isConst() &&
                 Op.Inputs[0].Offset == static_cast<uint64_t>(
                     Compress ? Intrinsic::EVEXCompressStore
                              : Intrinsic::EVEXExpandLoad);
        }));

        BinaryImage I = image(0x6000, 64);
        for (size_t N = 0; N < I.Segments[0].Data.size(); ++N)
          I.Segments[0].Data[N] = static_cast<uint8_t>(0x20 + N);
        NdOpEmulator E(I);
        E.setStrictMode(true);
        E.setLoadCollect(true);
        E.setRegister(x86reg::RAX, 0x6000);
        const unsigned LaneCount = VectorSize / F.Element;
        const uint64_t ActiveMask = LaneCount > 2 ? 0x5 : 0x1;
        E.setRegister(x86reg::K2, ActiveMask);
        std::vector<uint8_t> Initial(64, 0xcc);
        E.setRegisterBytes(Compress ? x86reg::vectorReg(3)
                                    : x86reg::vectorReg(1), Initial);
        for (const LowOp &Op : Ops)
          ASSERT_TRUE(E.step(Op));
        if (Compress) {
        LowOp Probe;
        Probe.Opcode = NdOp::LOAD;
        Probe.Output = NdVar::tmp(1000 + F.Element, F.Element);
        Probe.addInput(NdVar::cst(0, 8));
        Probe.addInput(NdVar::cst(0x6000, 8));
        ASSERT_TRUE(E.step(Probe));
        EXPECT_EQ(E.getRegister(1000 + F.Element), 0xccccccccccccccccULL &
                  (F.Element == 8 ? UINT64_MAX
                                  : ((UINT64_C(1) << (F.Element * 8)) - 1)));

        NdOpEmulator Suppressed(I);
        Suppressed.setStrictMode(true);
        Suppressed.setRegister(x86reg::RAX, 0xdead0000);
        Suppressed.setRegister(x86reg::K2, 0);
        Suppressed.setRegisterBytes(x86reg::vectorReg(3), Initial);
        for (const LowOp &Op : Ops)
          ASSERT_TRUE(Suppressed.step(Op));
        } else {
        const auto Result = E.getRegisterBytes(x86reg::vectorReg(1));
        ASSERT_TRUE(Result);
        ASSERT_EQ(Result->size(), 64u);
        for (unsigned B = 0; B < F.Element; ++B) {
          EXPECT_EQ((*Result)[B], static_cast<uint8_t>(0x20 + B));
          EXPECT_EQ((*Result)[F.Element + B], Zero ? 0u : 0xccu);
          if (LaneCount > 2)
            EXPECT_EQ((*Result)[2 * F.Element + B],
                      static_cast<uint8_t>(0x20 + F.Element + B));
        }
        EXPECT_EQ(E.getLoadRecords().size(), LaneCount > 2 ? 2u : 1u);

        NdOpEmulator Suppressed(I);
        Suppressed.setStrictMode(true);
        Suppressed.setLoadCollect(true);
        Suppressed.setRegister(x86reg::RAX, 0xdead0000);
        Suppressed.setRegister(x86reg::K2, 0);
        Suppressed.setRegisterBytes(x86reg::vectorReg(1), Initial);
        for (const LowOp &Op : Ops)
          ASSERT_TRUE(Suppressed.step(Op));
        EXPECT_TRUE(Suppressed.getLoadRecords().empty());
        const auto SuppressedResult =
            Suppressed.getRegisterBytes(x86reg::vectorReg(1));
        ASSERT_TRUE(SuppressedResult);
        EXPECT_TRUE(std::all_of(
            SuppressedResult->begin(),
            SuppressedResult->begin() + VectorSize,
            [Zero](uint8_t Byte) { return Byte == (Zero ? 0 : 0xcc); }));
        EXPECT_TRUE(std::all_of(SuppressedResult->begin() + VectorSize,
                                SuppressedResult->end(),
                                [](uint8_t Byte) { return Byte == 0; }));
        }
      }
    }
    }
  }
}

TEST(X86EVEXCompressExpandMemory, RejectsZeroingMemoryDestination) {
  for (const Family &F : Families) {
    auto Bytes = encoding(F, /*Compress=*/true, /*Zero=*/false);
    Bytes[3] |= 0x80;
    Decoder D;
    ASSERT_TRUE(D.init(Arch::X64));
    DecodedInsn I{};
    if (D.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000, I) !=
        static_cast<int>(Bytes.size()))
      continue;
    std::vector<LowOp> Ops;
    try {
      D.liftToLow(I, Ops);
      EXPECT_FALSE(std::any_of(Ops.begin(), Ops.end(), [](const LowOp &Op) {
        return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 0 &&
               Op.Inputs[0].isConst() &&
               Op.Inputs[0].Offset ==
                   static_cast<uint64_t>(Intrinsic::EVEXCompressStore);
      }));
    } catch (const UnliftedInstruction &) {
    }
  }
}

} // namespace
