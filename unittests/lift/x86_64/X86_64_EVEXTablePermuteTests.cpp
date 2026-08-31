//===- X86_64_EVEXTablePermuteTests.cpp - EVEX table permutes -----------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <optional>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kAddress = 0x1000;

enum class MaskMode : uint8_t { None, Merge, Zero };

struct PermuteFamily {
  const char *Name;
  unsigned Id;
  uint8_t Opcode;
  unsigned ElementSize;
  bool W;
  bool IndexInDestination;
};

constexpr PermuteFamily kFamilies[] = {
    {"vpermi2b", X86_INS_VPERMI2B, 0x75, 1, false, true},
    {"vpermi2w", X86_INS_VPERMI2W, 0x75, 2, true, true},
    {"vpermi2d", X86_INS_VPERMI2D, 0x76, 4, false, true},
    {"vpermi2q", X86_INS_VPERMI2Q, 0x76, 8, true, true},
    {"vpermi2ps", X86_INS_VPERMI2PS, 0x77, 4, false, true},
    {"vpermi2pd", X86_INS_VPERMI2PD, 0x77, 8, true, true},
    {"vpermt2b", X86_INS_VPERMT2B, 0x7d, 1, false, false},
    {"vpermt2w", X86_INS_VPERMT2W, 0x7d, 2, true, false},
    {"vpermt2d", X86_INS_VPERMT2D, 0x7e, 4, false, false},
    {"vpermt2q", X86_INS_VPERMT2Q, 0x7e, 8, true, false},
    {"vpermt2ps", X86_INS_VPERMT2PS, 0x7f, 4, false, false},
    {"vpermt2pd", X86_INS_VPERMT2PD, 0x7f, 8, true, false},
};

std::vector<uint8_t> registerEncoding(const PermuteFamily &Family,
                                      unsigned VectorSize, MaskMode Mode) {
  const uint8_t Length =
      VectorSize == 16 ? 0 : (VectorSize == 32 ? 0x20 : 0x40);
  const uint8_t Mask = Mode == MaskMode::None ? 0 : 7;
  const uint8_t Zero = Mode == MaskMode::Zero ? 0x80 : 0;
  // Destination 30, source 1/index 28, source 2 29, and writemask K7.
  return {0x62,
          0x02,
          static_cast<uint8_t>(Family.W ? 0x9d : 0x1d),
          static_cast<uint8_t>(Length | Zero | Mask),
          Family.Opcode,
          0xf5};
}

std::vector<uint8_t> memoryEncoding(const PermuteFamily &Family,
                                    bool Broadcast) {
  // Destination 30, source 1/index 28, source 2 [RAX], and writemask K7.
  return {0x62,
          0x62,
          static_cast<uint8_t>(Family.W ? 0x9d : 0x1d),
          static_cast<uint8_t>(0x47 | (Broadcast ? 0x10 : 0)),
          Family.Opcode,
          0x30};
}

void setLane(std::vector<uint8_t> &Bytes, unsigned Lane, unsigned ElementSize,
             uint64_t Value) {
  ASSERT_LE(ElementSize, sizeof(Value));
  ASSERT_LE((Lane + 1) * ElementSize, Bytes.size());
  std::memcpy(Bytes.data() + Lane * ElementSize, &Value, ElementSize);
}

uint64_t getLane(const std::vector<uint8_t> &Bytes, unsigned Lane,
                 unsigned ElementSize) {
  uint64_t Value = 0;
  EXPECT_LE(ElementSize, sizeof(Value));
  EXPECT_LE((Lane + 1) * ElementSize, Bytes.size());
  std::memcpy(&Value, Bytes.data() + Lane * ElementSize, ElementSize);
  return Value;
}

BinaryImage emptyImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  return Image;
}

void addReadableBytes(BinaryImage &Image, uint64_t Address, const void *Data,
                      size_t Size) {
  Segment Memory;
  Memory.VA = Address;
  Memory.Size = Size;
  Memory.Flags = SegmentFlags::Readable;
  Memory.Data.resize(Size);
  std::memcpy(Memory.Data.data(), Data, Size);
  Image.Segments.push_back(std::move(Memory));
}

std::optional<std::vector<LowOp>> liftX64(const std::vector<uint8_t> &Bytes,
                                          unsigned ExpectedId) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "x86-64 decoder initialization failed";
    return std::nullopt;
  }
  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn) !=
      static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "instruction decode failed";
    return std::nullopt;
  }
  if (!Insn.Raw || Insn.Raw->id != ExpectedId) {
    ADD_FAILURE() << "unexpected instruction id";
    return std::nullopt;
  }
  std::vector<LowOp> Ops;
  try {
    Dec.liftToLow(Insn, Ops);
  } catch (const UnliftedInstruction &) {
    ADD_FAILURE() << "instruction was rejected";
    return std::nullopt;
  }
  return Ops;
}

void expectDecodeOrLiftRejected(const std::vector<uint8_t> &Bytes,
                                unsigned ExpectedId) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn) !=
      static_cast<int>(Bytes.size()))
    return;
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_EQ(Insn.Raw->id, ExpectedId);
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

void expectMalformedShapeRejected(const std::vector<uint8_t> &Bytes,
                                  const std::function<void(cs_x86 &)> &Mutate) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  Mutate(Insn.Raw->detail->x86);
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

std::vector<uint8_t> referencePermute(const std::vector<uint8_t> &Old,
                                      const std::vector<uint8_t> &Source1,
                                      const std::vector<uint8_t> &Source2,
                                      bool IndexInDestination,
                                      unsigned ActiveSize, unsigned ElementSize,
                                      MaskMode Mode = MaskMode::None,
                                      uint64_t Mask = 0) {
  std::vector<uint8_t> Result(64, 0);
  const unsigned LaneCount = ActiveSize / ElementSize;
  const std::vector<uint8_t> &Indices = IndexInDestination ? Old : Source1;
  const std::vector<uint8_t> &Table1 = IndexInDestination ? Source1 : Old;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    const bool Active =
        Mode == MaskMode::None || (Mask & (UINT64_C(1) << Lane)) != 0;
    if (!Active) {
      if (Mode == MaskMode::Merge)
        setLane(Result, Lane, ElementSize, getLane(Old, Lane, ElementSize));
      continue;
    }
    const uint64_t Index = getLane(Indices, Lane, ElementSize);
    const unsigned Offset = static_cast<unsigned>(Index) & (LaneCount - 1);
    const std::vector<uint8_t> &Table =
        (Index & LaneCount) != 0 ? Source2 : Table1;
    setLane(Result, Lane, ElementSize, getLane(Table, Offset, ElementSize));
  }
  return Result;
}

TEST(X86EVEXTablePermute, DwordI2AndT2UseDifferentDestinationSources) {
  constexpr unsigned ActiveSize = 64;
  constexpr unsigned ElementSize = 4;
  constexpr unsigned LaneCount = ActiveSize / ElementSize;
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM30);
  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM28);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM29);

  std::vector<uint8_t> Indices(64, 0);
  std::vector<uint8_t> Table1(64, 0);
  std::vector<uint8_t> Table2(64, 0);
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
    const uint64_t Index = ((Lane * 5 + 3) & (LaneCount - 1)) |
                           (Lane % 3 == 1 ? LaneCount : 0) |
                           UINT64_C(0xa5a50000);
    setLane(Indices, Lane, ElementSize, Index);
    setLane(Table1, Lane, ElementSize, UINT32_C(0x11000000) + Lane);
    setLane(Table2, Lane, ElementSize, UINT32_C(0x22000000) + Lane);
  }

  struct Case {
    std::vector<uint8_t> Encoding;
    unsigned Id;
    bool IndexInDestination;
  };
  const std::vector<Case> Cases = {
      {{0x62, 0x02, 0x1d, 0x40, 0x76, 0xf5}, X86_INS_VPERMI2D, true},
      {{0x62, 0x02, 0x1d, 0x40, 0x7e, 0xf5}, X86_INS_VPERMT2D, false},
  };

  for (const Case &Current : Cases) {
    SCOPED_TRACE(Current.IndexInDestination ? "I2" : "T2");
    const auto Ops = liftX64(Current.Encoding, Current.Id);
    ASSERT_TRUE(Ops);
    ASSERT_FALSE(Ops->empty());

    const std::vector<uint8_t> &Old =
        Current.IndexInDestination ? Indices : Table1;
    const std::vector<uint8_t> &FirstSource =
        Current.IndexInDestination ? Table1 : Indices;
    const std::vector<uint8_t> Expected =
        referencePermute(Old, FirstSource, Table2, Current.IndexInDestination,
                         ActiveSize, ElementSize);

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(Destination.Offset, Old);
    Emulator.setRegisterBytes(Source1.Offset, FirstSource);
    Emulator.setRegisterBytes(Source2.Offset, Table2);
    ASSERT_EQ(Emulator.run(*Ops), Ops->size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), Expected);
    EXPECT_EQ(Emulator.getRegisterBytes(Source1.Offset), FirstSource);
    EXPECT_EQ(Emulator.getRegisterBytes(Source2.Offset), Table2);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86EVEXTablePermute, AllFamiliesWidthsAndMaskModesMatchReference) {
  constexpr unsigned VectorSizes[] = {16, 32, 64};
  constexpr MaskMode MaskModes[] = {MaskMode::None, MaskMode::Merge,
                                    MaskMode::Zero};
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM30);
  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM28);
  const RegInfo Source2 = mapCapstoneReg(X86_REG_ZMM29);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K7);

  for (const PermuteFamily &Family : kFamilies) {
    for (unsigned VectorSize : VectorSizes) {
      const unsigned LaneCount = VectorSize / Family.ElementSize;
      const uint64_t ElementMask =
          Family.ElementSize == 8
              ? UINT64_MAX
              : (UINT64_C(1) << (Family.ElementSize * 8)) - 1;
      const uint64_t IgnoredIndexBits =
          UINT64_C(0xa5a5a5a5a5a5a5a5) &
          ~(static_cast<uint64_t>(LaneCount * 2) - 1) & ElementMask;
      uint64_t MaskValue = UINT64_C(0xf0f0f0f0f0f0f0f0);
      std::vector<uint8_t> Indices(64, 0xcc);
      std::vector<uint8_t> Table1(64, 0xdd);
      std::vector<uint8_t> Table2(64, 0xee);
      for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
        const uint64_t Index = ((Lane * 5 + 3) & (LaneCount - 1)) |
                               (Lane % 3 == 1 ? LaneCount : 0) |
                               IgnoredIndexBits;
        setLane(Indices, Lane, Family.ElementSize, Index);
        setLane(Table1, Lane, Family.ElementSize,
                (UINT64_C(0x1111111111111100) + Lane) & ElementMask);
        setLane(Table2, Lane, Family.ElementSize,
                (UINT64_C(0x8888888888888880) + Lane) & ElementMask);
        if (Lane % 3 != 1)
          MaskValue |= UINT64_C(1) << Lane;
        else
          MaskValue &= ~(UINT64_C(1) << Lane);
      }

      for (MaskMode Mode : MaskModes) {
        SCOPED_TRACE(testing::Message()
                     << Family.Name << ", vector bytes=" << VectorSize
                     << ", mask mode=" << static_cast<unsigned>(Mode));
        const auto Ops =
            liftX64(registerEncoding(Family, VectorSize, Mode), Family.Id);
        ASSERT_TRUE(Ops);
        ASSERT_FALSE(Ops->empty());

        const std::vector<uint8_t> &Old =
            Family.IndexInDestination ? Indices : Table1;
        const std::vector<uint8_t> &FirstSource =
            Family.IndexInDestination ? Table1 : Indices;
        const std::vector<uint8_t> Expected = referencePermute(
            Old, FirstSource, Table2, Family.IndexInDestination, VectorSize,
            Family.ElementSize, Mode, MaskValue);

        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegisterBytes(Destination.Offset, Old);
        Emulator.setRegisterBytes(Source1.Offset, FirstSource);
        Emulator.setRegisterBytes(Source2.Offset, Table2);
        Emulator.setRegister(WriteMask.Offset, MaskValue);
        ASSERT_EQ(Emulator.run(*Ops), Ops->size());
        EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), Expected);
        EXPECT_EQ(Emulator.getRegisterBytes(Source1.Offset), FirstSource);
        EXPECT_EQ(Emulator.getRegisterBytes(Source2.Offset), Table2);
        EXPECT_EQ(Emulator.getRegister(WriteMask.Offset), MaskValue);
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }
}

TEST(X86EVEXTablePermute, AllLegalMemoryFormsLift) {
  for (const PermuteFamily &Family : kFamilies) {
    SCOPED_TRACE(Family.Name);
    const auto FullTuple = liftX64(memoryEncoding(Family, false), Family.Id);
    ASSERT_TRUE(FullTuple);
    EXPECT_FALSE(FullTuple->empty());
    if (Family.ElementSize >= 4) {
      const auto Broadcast = liftX64(memoryEncoding(Family, true), Family.Id);
      ASSERT_TRUE(Broadcast);
      EXPECT_FALSE(Broadcast->empty());
    } else {
      expectDecodeOrLiftRejected(memoryEncoding(Family, true), Family.Id);
    }
  }
}

TEST(X86EVEXTablePermute,
     FullTupleLoadsOnlyMemoryTableLanesSelectedByActiveOutputs) {
  constexpr uint64_t Base = UINT64_C(0x8000);
  constexpr unsigned VectorSize = 64;
  constexpr unsigned ElementSize = 4;
  constexpr unsigned LaneCount = VectorSize / ElementSize;
  constexpr uint64_t ActiveMask = (UINT64_C(1) << 1) | (UINT64_C(1) << 5);
  constexpr unsigned MemoryLaneA = 7;
  constexpr unsigned MemoryLaneB = 2;
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM30);
  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM28);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K7);

  for (const PermuteFamily *Family : {&kFamilies[2], &kFamilies[8]}) {
    SCOPED_TRACE(Family->Name);
    const auto Ops = liftX64(memoryEncoding(*Family, false), Family->Id);
    ASSERT_TRUE(Ops);

    std::vector<uint8_t> Indices(64, 0);
    std::vector<uint8_t> Table1(64, 0);
    std::vector<uint8_t> Table2(64, 0);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      setLane(Indices, Lane, ElementSize, Lane & (LaneCount - 1));
      setLane(Table1, Lane, ElementSize, UINT32_C(0x11000000) + Lane);
      setLane(Table2, Lane, ElementSize, UINT32_C(0x22000000) + Lane);
    }
    setLane(Indices, 1, ElementSize, LaneCount | MemoryLaneA);
    setLane(Indices, 5, ElementSize, LaneCount | MemoryLaneB);

    const std::vector<uint8_t> &Old =
        Family->IndexInDestination ? Indices : Table1;
    const std::vector<uint8_t> &FirstSource =
        Family->IndexInDestination ? Table1 : Indices;
    const std::vector<uint8_t> Expected =
        referencePermute(Old, FirstSource, Table2, Family->IndexInDestination,
                         VectorSize, ElementSize, MaskMode::Merge, ActiveMask);

    BinaryImage Unmapped = emptyImage();
    NdOpEmulator Suppressed(Unmapped);
    Suppressed.setStrictMode(true);
    Suppressed.setLoadCollect(true);
    Suppressed.setRegister(x86reg::RAX, Base);
    Suppressed.setRegister(WriteMask.Offset, 0);
    Suppressed.setRegisterBytes(Destination.Offset, Old);
    Suppressed.setRegisterBytes(Source1.Offset, FirstSource);
    ASSERT_EQ(Suppressed.run(*Ops), Ops->size());
    EXPECT_EQ(Suppressed.getRegisterBytes(Destination.Offset), Old);
    EXPECT_TRUE(Suppressed.getLoadRecords().empty());
    EXPECT_FALSE(Suppressed.skips().any());

    std::vector<uint8_t> RegisterIndices = Indices;
    setLane(RegisterIndices, 1, ElementSize, 3);
    setLane(RegisterIndices, 5, ElementSize, 9);
    const std::vector<uint8_t> &RegisterOld =
        Family->IndexInDestination ? RegisterIndices : Table1;
    const std::vector<uint8_t> &RegisterSource =
        Family->IndexInDestination ? Table1 : RegisterIndices;
    const std::vector<uint8_t> RegisterExpected = referencePermute(
        RegisterOld, RegisterSource, Table2, Family->IndexInDestination,
        VectorSize, ElementSize, MaskMode::Merge, ActiveMask);
    NdOpEmulator RegisterOnly(Unmapped);
    RegisterOnly.setStrictMode(true);
    RegisterOnly.setLoadCollect(true);
    RegisterOnly.setRegister(x86reg::RAX, Base);
    RegisterOnly.setRegister(WriteMask.Offset, ActiveMask);
    RegisterOnly.setRegisterBytes(Destination.Offset, RegisterOld);
    RegisterOnly.setRegisterBytes(Source1.Offset, RegisterSource);
    ASSERT_EQ(RegisterOnly.run(*Ops), Ops->size());
    EXPECT_EQ(RegisterOnly.getRegisterBytes(Destination.Offset),
              RegisterExpected);
    EXPECT_TRUE(RegisterOnly.getLoadRecords().empty());
    EXPECT_FALSE(RegisterOnly.skips().any());

    BinaryImage Image = emptyImage();
    const uint32_t ValueA =
        static_cast<uint32_t>(getLane(Table2, MemoryLaneA, ElementSize));
    const uint32_t ValueB =
        static_cast<uint32_t>(getLane(Table2, MemoryLaneB, ElementSize));
    addReadableBytes(Image, Base + MemoryLaneA * ElementSize, &ValueA,
                     sizeof(ValueA));
    addReadableBytes(Image, Base + MemoryLaneB * ElementSize, &ValueB,
                     sizeof(ValueB));
    NdOpEmulator MemorySelected(Image);
    MemorySelected.setStrictMode(true);
    MemorySelected.setLoadCollect(true);
    MemorySelected.setRegister(x86reg::RAX, Base);
    MemorySelected.setRegister(WriteMask.Offset, ActiveMask);
    MemorySelected.setRegisterBytes(Destination.Offset, Old);
    MemorySelected.setRegisterBytes(Source1.Offset, FirstSource);
    ASSERT_EQ(MemorySelected.run(*Ops), Ops->size());
    EXPECT_EQ(MemorySelected.getRegisterBytes(Destination.Offset), Expected);
    ASSERT_EQ(MemorySelected.getLoadRecords().size(), 2u);
    EXPECT_EQ(MemorySelected.getLoadRecords()[0].Addr,
              Base + MemoryLaneB * ElementSize);
    EXPECT_EQ(MemorySelected.getLoadRecords()[1].Addr,
              Base + MemoryLaneA * ElementSize);
    for (const auto &Load : MemorySelected.getLoadRecords())
      EXPECT_EQ(Load.Size, ElementSize);
    EXPECT_FALSE(MemorySelected.skips().any());
  }
}

TEST(X86EVEXTablePermute,
     BroadcastLoadsOneScalarOnlyWhenAnActiveOutputSelectsMemoryTable) {
  constexpr uint64_t Base = UINT64_C(0x9000);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM30);
  const RegInfo Source1 = mapCapstoneReg(X86_REG_ZMM28);
  const RegInfo WriteMask = mapCapstoneReg(X86_REG_K7);

  for (const PermuteFamily *Family :
       {&kFamilies[2], &kFamilies[3], &kFamilies[8], &kFamilies[9]}) {
    SCOPED_TRACE(Family->Name);
    const auto Ops = liftX64(memoryEncoding(*Family, true), Family->Id);
    ASSERT_TRUE(Ops);
    const unsigned LaneCount = 64 / Family->ElementSize;
    const uint64_t ActiveMask = (UINT64_C(1) << 2) | (UINT64_C(1) << 7);
    const uint64_t ElementMask =
        Family->ElementSize == 8 ? UINT64_MAX : UINT32_MAX;
    const uint64_t Scalar = UINT64_C(0x8877665544332211) & ElementMask;
    std::vector<uint8_t> Indices(64, 0);
    std::vector<uint8_t> Table1(64, 0);
    std::vector<uint8_t> BroadcastTable(64, 0);
    for (unsigned Lane = 0; Lane < LaneCount; ++Lane) {
      setLane(Indices, Lane, Family->ElementSize, Lane & (LaneCount - 1));
      setLane(Table1, Lane, Family->ElementSize,
              (UINT64_C(0x1111111111111100) + Lane) & ElementMask);
      setLane(BroadcastTable, Lane, Family->ElementSize, Scalar);
    }
    setLane(Indices, 2, Family->ElementSize, LaneCount | 1);
    setLane(Indices, 7, Family->ElementSize, LaneCount | (LaneCount - 2));
    const std::vector<uint8_t> &Old =
        Family->IndexInDestination ? Indices : Table1;
    const std::vector<uint8_t> &FirstSource =
        Family->IndexInDestination ? Table1 : Indices;
    const std::vector<uint8_t> Expected = referencePermute(
        Old, FirstSource, BroadcastTable, Family->IndexInDestination, 64,
        Family->ElementSize, MaskMode::Merge, ActiveMask);

    BinaryImage Image = emptyImage();
    addReadableBytes(Image, Base, &Scalar, Family->ElementSize);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Base);
    Emulator.setRegister(WriteMask.Offset, ActiveMask);
    Emulator.setRegisterBytes(Destination.Offset, Old);
    Emulator.setRegisterBytes(Source1.Offset, FirstSource);
    ASSERT_EQ(Emulator.run(*Ops), Ops->size());
    EXPECT_EQ(Emulator.getRegisterBytes(Destination.Offset), Expected);
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Base);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, Family->ElementSize);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86EVEXTablePermute, InconsistentEncodingsAndDetailsFailClosed) {

  const PermuteFamily &WordI2 = kFamilies[1];
  const std::vector<uint8_t> Valid =
      registerEncoding(WordI2, 32, MaskMode::Zero);
  expectMalformedShapeRejected(
      Valid, [](cs_x86 &X86) { X86.operands[0].reg = X86_REG_YMM31; });
  expectMalformedShapeRejected(
      Valid, [](cs_x86 &X86) { X86.operands[1].reg = X86_REG_K6; });
  expectMalformedShapeRejected(
      Valid, [](cs_x86 &X86) { X86.operands[1].avx_zero_opmask = false; });
  expectMalformedShapeRejected(
      Valid, [](cs_x86 &X86) { X86.operands[2].reg = X86_REG_YMM27; });
  expectMalformedShapeRejected(
      Valid, [](cs_x86 &X86) { X86.operands[3].reg = X86_REG_YMM27; });
  expectMalformedShapeRejected(Valid,
                               [](cs_x86 &X86) { X86.operands[2].size = 64; });

  std::vector<uint8_t> ReservedLength = Valid;
  ReservedLength[3] =
      static_cast<uint8_t>((ReservedLength[3] & ~0x60u) | 0x60u);
  expectDecodeOrLiftRejected(ReservedLength, WordI2.Id);

  std::vector<uint8_t> RegisterBroadcast = Valid;
  RegisterBroadcast[3] |= 0x10;
  expectDecodeOrLiftRejected(RegisterBroadcast, WordI2.Id);

  std::vector<uint8_t> ZeroWithoutMask =
      registerEncoding(WordI2, 32, MaskMode::None);
  ZeroWithoutMask[3] |= 0x80;
  expectDecodeOrLiftRejected(ZeroWithoutMask, WordI2.Id);
}

} // namespace
