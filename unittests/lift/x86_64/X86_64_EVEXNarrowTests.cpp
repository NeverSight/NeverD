//===- X86_64_EVEXNarrowTests.cpp - EVEX narrowing semantics -----------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kAddress = 0x1000;

enum class NarrowMode : uint8_t {
  Truncate,
  SignedSaturate,
  UnsignedSaturate,
};

enum class MaskMode : uint8_t {
  None,
  Merge,
  Zero,
};

struct NarrowFamily {
  const char *Name;
  uint8_t Opcode;
  uint8_t SourceElementSize;
  uint8_t DestinationElementSize;
  NarrowMode Mode;
};

constexpr std::array<NarrowFamily, 18> kFamilies = {{
    {"vpmovwb", 0x30, 2, 1, NarrowMode::Truncate},
    {"vpmovdb", 0x31, 4, 1, NarrowMode::Truncate},
    {"vpmovqb", 0x32, 8, 1, NarrowMode::Truncate},
    {"vpmovdw", 0x33, 4, 2, NarrowMode::Truncate},
    {"vpmovqw", 0x34, 8, 2, NarrowMode::Truncate},
    {"vpmovqd", 0x35, 8, 4, NarrowMode::Truncate},
    {"vpmovswb", 0x20, 2, 1, NarrowMode::SignedSaturate},
    {"vpmovsdb", 0x21, 4, 1, NarrowMode::SignedSaturate},
    {"vpmovsqb", 0x22, 8, 1, NarrowMode::SignedSaturate},
    {"vpmovsdw", 0x23, 4, 2, NarrowMode::SignedSaturate},
    {"vpmovsqw", 0x24, 8, 2, NarrowMode::SignedSaturate},
    {"vpmovsqd", 0x25, 8, 4, NarrowMode::SignedSaturate},
    {"vpmovuswb", 0x10, 2, 1, NarrowMode::UnsignedSaturate},
    {"vpmovusdb", 0x11, 4, 1, NarrowMode::UnsignedSaturate},
    {"vpmovusqb", 0x12, 8, 1, NarrowMode::UnsignedSaturate},
    {"vpmovusdw", 0x13, 4, 2, NarrowMode::UnsignedSaturate},
    {"vpmovusqw", 0x14, 8, 2, NarrowMode::UnsignedSaturate},
    {"vpmovusqd", 0x15, 8, 4, NarrowMode::UnsignedSaturate},
}};

uint64_t maskForBytes(size_t Size) {
  return Size == 8 ? UINT64_MAX : (UINT64_C(1) << (Size * 8)) - 1;
}

int64_t signedMinimum(size_t Size) {
  return Size == 8 ? std::numeric_limits<int64_t>::min()
                   : -(INT64_C(1) << (Size * 8 - 1));
}

int64_t signedMaximum(size_t Size) {
  return Size == 8 ? std::numeric_limits<int64_t>::max()
                   : (INT64_C(1) << (Size * 8 - 1)) - 1;
}

uint64_t encodeSigned(int64_t Value, size_t Size) {
  return static_cast<uint64_t>(Value) & maskForBytes(Size);
}

int64_t decodeSigned(uint64_t Value, size_t Size) {
  Value &= maskForBytes(Size);
  if (Size == 8)
    return static_cast<int64_t>(Value);
  const unsigned Bits = static_cast<unsigned>(Size * 8);
  const uint64_t Sign = UINT64_C(1) << (Bits - 1);
  return static_cast<int64_t>((Value ^ Sign) - Sign);
}

uint64_t sourceLaneValue(const NarrowFamily &Family, size_t Lane) {
  const int64_t DstMin = signedMinimum(Family.DestinationElementSize);
  const int64_t DstMax = signedMaximum(Family.DestinationElementSize);
  const int64_t SrcMin = signedMinimum(Family.SourceElementSize);
  const int64_t SrcMax = signedMaximum(Family.SourceElementSize);
  const uint64_t UnsignedMax = maskForBytes(Family.DestinationElementSize);

  if (Family.Mode == NarrowMode::SignedSaturate) {
    const std::array<int64_t, 8> Values = {
        SrcMin, DstMin - 1, DstMin, -1, 0, DstMax, DstMax + 1, SrcMax,
    };
    return encodeSigned(Values[Lane % Values.size()], Family.SourceElementSize);
  }
  if (Family.Mode == NarrowMode::UnsignedSaturate) {
    const std::array<int64_t, 8> Values = {
        SrcMin,
        -1,
        0,
        1,
        static_cast<int64_t>(UnsignedMax),
        static_cast<int64_t>(UnsignedMax + 1),
        SrcMax,
        0x5a,
    };
    return encodeSigned(Values[Lane % Values.size()], Family.SourceElementSize);
  }

  const uint64_t Pattern =
      UINT64_C(0xfedcba9876543210) ^
      (UINT64_C(0x102030405060708) * static_cast<uint64_t>(Lane + 1));
  return Pattern & maskForBytes(Family.SourceElementSize);
}

uint64_t convertedLaneValue(const NarrowFamily &Family, uint64_t Source) {
  const uint64_t DstMask = maskForBytes(Family.DestinationElementSize);
  if (Family.Mode == NarrowMode::Truncate)
    return Source & DstMask;

  const int64_t SignedSource = decodeSigned(Source, Family.SourceElementSize);
  if (Family.Mode == NarrowMode::SignedSaturate) {
    const int64_t Low = signedMinimum(Family.DestinationElementSize);
    const int64_t High = signedMaximum(Family.DestinationElementSize);
    const int64_t Clamped =
        SignedSource < Low ? Low : (SignedSource > High ? High : SignedSource);
    return encodeSigned(Clamped, Family.DestinationElementSize);
  }

  if (SignedSource <= 0)
    return 0;
  return static_cast<uint64_t>(SignedSource) > DstMask
             ? DstMask
             : static_cast<uint64_t>(SignedSource);
}

void setLane(std::vector<uint8_t> &Value, size_t Lane, size_t LaneSize,
             uint64_t LaneValue) {
  const size_t Offset = Lane * LaneSize;
  ASSERT_LE(Offset + LaneSize, Value.size());
  for (size_t Byte = 0; Byte < LaneSize; ++Byte)
    Value[Offset + Byte] =
        static_cast<uint8_t>(LaneValue >> static_cast<unsigned>(Byte * 8));
}

uint64_t getLane(const std::vector<uint8_t> &Value, size_t Lane,
                 size_t LaneSize) {
  const size_t Offset = Lane * LaneSize;
  EXPECT_LE(Offset + LaneSize, Value.size());
  uint64_t Result = 0;
  for (size_t Byte = 0; Byte < LaneSize; ++Byte)
    Result |= static_cast<uint64_t>(Value[Offset + Byte]) << (Byte * 8);
  return Result;
}

std::vector<uint8_t> narrowEncoding(const NarrowFamily &Family,
                                    size_t SourceSize, MaskMode Mask) {
  const uint8_t Length =
      SourceSize == 16 ? 0x00 : (SourceSize == 32 ? 0x20 : 0x40);
  uint8_t P2 = static_cast<uint8_t>(0x08 | Length);
  if (Mask != MaskMode::None)
    P2 = static_cast<uint8_t>(P2 | 0x02);
  if (Mask == MaskMode::Zero)
    P2 = static_cast<uint8_t>(P2 | 0x80);
  // vpmov* vec1{k2}{z}, vec3. The destination register class is implied by
  // the narrowing ratio and encoded source length.
  return {0x62, 0xf2, 0x7e, P2, Family.Opcode, 0xd9};
}

std::vector<uint8_t> narrowMemoryEncoding(const NarrowFamily &Family,
                                          size_t SourceSize, bool HasMask) {
  const uint8_t Length =
      SourceSize == 16 ? 0x00 : (SourceSize == 32 ? 0x20 : 0x40);
  const uint8_t P2 =
      static_cast<uint8_t>(0x08 | Length | (HasMask ? 0x02 : 0x00));
  // vpmov* [rax]{k2}, vec3. The destination tuple width follows the source
  // lane count and the narrowed element width.
  return {0x62, 0xf2, 0x7e, P2, Family.Opcode, 0x18};
}

std::vector<LowOp> liftX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "x86-64 decoder initialization failed";
    return {};
  }

  DecodedInsn Insn{};
  const int Size =
      Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn);
  if (Size != static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "x86-64 instruction decode failed";
    return {};
  }

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

void expectDecodeOrLiftRejected(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));

  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn) !=
      static_cast<int>(Bytes.size()))
    return;

  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

BinaryImage writableImage(uint64_t Address, const std::vector<uint8_t> &Bytes) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Memory;
  Memory.VA = Address;
  Memory.Size = Bytes.size();
  Memory.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Memory.Data = Bytes;
  Image.Segments.push_back(std::move(Memory));
  return Image;
}

std::optional<std::vector<uint8_t>>
readMemoryBytes(NdOpEmulator &Emulator, uint64_t Address, size_t Size) {
  std::vector<uint8_t> Result(Size);
  uint64_t Temp = UINT64_C(0x7d000000);
  for (size_t Offset = 0; Offset < Size;) {
    const uint16_t Chunk =
        static_cast<uint16_t>(std::min<size_t>(8, Size - Offset));
    LowOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = NdVar::tmp(Temp++, Chunk);
    Load.addInput(NdVar::cst(0, 8));
    Load.addInput(NdVar::cst(Address + Offset, 8));
    if (!Emulator.step(Load))
      return std::nullopt;
    const auto Value = Emulator.getRegister(Temp - 1);
    if (!Value)
      return std::nullopt;
    std::memcpy(Result.data() + Offset, &*Value, Chunk);
    Offset += Chunk;
  }
  return Result;
}

TEST(X86EVEXNarrow, AllFamiliesWidthsAndMaskModesMatchReference) {
  constexpr std::array<size_t, 3> SourceSizes = {16, 32, 64};
  constexpr std::array<MaskMode, 3> MaskModes = {
      MaskMode::None, MaskMode::Merge, MaskMode::Zero};

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM1);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K2);
  ASSERT_EQ(Source.Size, 64u);
  ASSERT_EQ(Destination.Size, 64u);
  ASSERT_EQ(Mask.Size, 8u);

  for (const NarrowFamily &Family : kFamilies) {
    for (size_t SourceSize : SourceSizes) {
      const size_t LaneCount = SourceSize / Family.SourceElementSize;
      const size_t ActiveBytes = LaneCount * Family.DestinationElementSize;
      ASSERT_LE(ActiveBytes, 32u);

      std::vector<uint8_t> SourceValue(64, 0xcc);
      for (size_t Lane = 0; Lane < LaneCount; ++Lane)
        setLane(SourceValue, Lane, Family.SourceElementSize,
                sourceLaneValue(Family, Lane));

      for (MaskMode CurrentMaskMode : MaskModes) {
        SCOPED_TRACE(testing::Message()
                     << Family.Name << ", source bytes=" << SourceSize
                     << ", mask mode="
                     << static_cast<unsigned>(CurrentMaskMode));

        const std::vector<LowOp> Ops =
            liftX64(narrowEncoding(Family, SourceSize, CurrentMaskMode));
        ASSERT_FALSE(Ops.empty());

        std::vector<uint8_t> InitialDestination(64);
        for (size_t Byte = 0; Byte < InitialDestination.size(); ++Byte)
          InitialDestination[Byte] = static_cast<uint8_t>(0x31u + Byte * 13u);
        std::vector<uint8_t> Expected(64, 0);
        uint64_t MaskValue = 0;
        for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
          const bool Active =
              CurrentMaskMode == MaskMode::None || ((Lane * 5 + 1) % 3 != 0);
          if (CurrentMaskMode != MaskMode::None && Active)
            MaskValue |= UINT64_C(1) << Lane;

          uint64_t ExpectedLane = 0;
          if (Active) {
            ExpectedLane = convertedLaneValue(
                Family, getLane(SourceValue, Lane, Family.SourceElementSize));
          } else if (CurrentMaskMode == MaskMode::Merge) {
            ExpectedLane = getLane(InitialDestination, Lane,
                                   Family.DestinationElementSize);
          }
          setLane(Expected, Lane, Family.DestinationElementSize, ExpectedLane);
        }

        BinaryImage Image;
        Image.Arch = Arch::X64;
        Image.Bits = Bitness::Bits64;
        NdOpEmulator Emulator(Image);
        Emulator.setRegisterBytes(Source.Offset, SourceValue);
        Emulator.setRegisterBytes(Destination.Offset, InitialDestination);
        Emulator.setRegister(Mask.Offset, MaskValue);
        EXPECT_EQ(Emulator.run(Ops), Ops.size());

        const auto Result = Emulator.getRegisterBytes(Destination.Offset);
        ASSERT_TRUE(Result);
        EXPECT_EQ(*Result, Expected);
        const auto SourceAfter = Emulator.getRegisterBytes(Source.Offset);
        ASSERT_TRUE(SourceAfter);
        EXPECT_EQ(*SourceAfter, SourceValue);
        ASSERT_TRUE(Emulator.getRegister(Mask.Offset));
        EXPECT_EQ(*Emulator.getRegister(Mask.Offset), MaskValue);
        EXPECT_LE(ActiveBytes, Result->size());
      }
    }
  }
}

TEST(X86EVEXNarrow, HighestVectorAndMaskRegistersRemainAddressable) {
  // vpmovuswb ymm31{k7}{z}, zmm20
  const std::vector<LowOp> Ops = liftX64({0x62, 0x82, 0x7e, 0xcf, 0x10, 0xe7});
  ASSERT_FALSE(Ops.empty());

  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM20);
  const RegInfo Destination = mapCapstoneReg(X86_REG_ZMM31);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K7);
  std::vector<uint8_t> SourceValue(64, 0);
  for (size_t Lane = 0; Lane < 32; ++Lane) {
    const int16_t Value = static_cast<int16_t>(Lane % 4 == 0   ? -1
                                               : Lane % 4 == 1 ? Lane
                                               : Lane % 4 == 2 ? 255
                                                               : 300);
    setLane(SourceValue, Lane, 2, static_cast<uint16_t>(Value));
  }

  std::vector<uint8_t> Expected(64, 0);
  const uint64_t MaskValue = UINT64_C(0x55555555);
  for (size_t Lane = 0; Lane < 32; ++Lane) {
    if (((MaskValue >> Lane) & 1) == 0)
      continue;
    const int16_t Value = static_cast<int16_t>(getLane(SourceValue, Lane, 2));
    Expected[Lane] = static_cast<uint8_t>(
        Value < 0 ? 0 : (Value > 255 ? 255 : static_cast<unsigned>(Value)));
  }

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setRegisterBytes(Source.Offset, SourceValue);
  Emulator.setRegisterBytes(Destination.Offset, std::vector<uint8_t>(64, 0xa5));
  Emulator.setRegister(Mask.Offset, MaskValue);
  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = Emulator.getRegisterBytes(Destination.Offset);
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, Expected);
}

TEST(X86EVEXNarrow, AllMemoryFamiliesWidthsAndMasksStoreExactActiveLanes) {
  constexpr std::array<size_t, 3> SourceSizes = {16, 32, 64};
  constexpr uint64_t Address = UINT64_C(0x8000);
  const RegInfo Source = mapCapstoneReg(X86_REG_ZMM3);
  const RegInfo Mask = mapCapstoneReg(X86_REG_K2);

  for (const NarrowFamily &Family : kFamilies) {
    for (size_t SourceSize : SourceSizes) {
      const size_t LaneCount = SourceSize / Family.SourceElementSize;
      const size_t DestinationSize = LaneCount * Family.DestinationElementSize;
      std::vector<uint8_t> SourceValue(64, 0xcc);
      for (size_t Lane = 0; Lane < LaneCount; ++Lane)
        setLane(SourceValue, Lane, Family.SourceElementSize,
                sourceLaneValue(Family, Lane));

      for (bool HasMask : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << Family.Name << ", source bytes=" << SourceSize
                     << ", masked=" << HasMask);
        const std::vector<LowOp> Ops =
            liftX64(narrowMemoryEncoding(Family, SourceSize, HasMask));
        ASSERT_FALSE(Ops.empty());

        std::vector<uint8_t> Initial(DestinationSize);
        for (size_t Byte = 0; Byte < Initial.size(); ++Byte)
          Initial[Byte] = static_cast<uint8_t>(0xa0 + Byte);
        std::vector<uint8_t> Expected = Initial;
        uint64_t MaskValue = 0;
        for (size_t Lane = 0; Lane < LaneCount; ++Lane) {
          const bool Active = !HasMask || Lane % 3 != 1;
          if (!Active)
            continue;
          MaskValue |= UINT64_C(1) << Lane;
          setLane(
              Expected, Lane, Family.DestinationElementSize,
              convertedLaneValue(Family, getLane(SourceValue, Lane,
                                                 Family.SourceElementSize)));
        }
        if (HasMask && LaneCount < 64)
          MaskValue |= UINT64_MAX << LaneCount;

        BinaryImage Image = writableImage(Address, Initial);
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        Emulator.setRegister(x86reg::RAX, Address);
        Emulator.setRegisterBytes(Source.Offset, SourceValue);
        Emulator.setRegister(Mask.Offset, MaskValue);
        ASSERT_EQ(Emulator.run(Ops), Ops.size());
        const auto Result = readMemoryBytes(Emulator, Address, Expected.size());
        ASSERT_TRUE(Result);
        EXPECT_EQ(*Result, Expected);
        EXPECT_EQ(Emulator.getRegisterBytes(Source.Offset), SourceValue);
        EXPECT_EQ(Emulator.getRegister(Mask.Offset), MaskValue);
        EXPECT_FALSE(Emulator.skips().any());
      }
    }
  }
}

TEST(X86EVEXNarrow,
     ZeroMaskSuppressesUnmappedStoreAndFaultingStoreCommitsNothing) {
  const NarrowFamily &Family = kFamilies[1]; // vpmovdb m128, zmm
  const std::vector<LowOp> Ops =
      liftX64(narrowMemoryEncoding(Family, 64, true));
  ASSERT_FALSE(Ops.empty());
  std::vector<uint8_t> SourceValue(64, 0);
  for (size_t Lane = 0; Lane < 16; ++Lane)
    setLane(SourceValue, Lane, 4, UINT32_C(0x12340000) + Lane);

  BinaryImage Unmapped;
  Unmapped.Arch = Arch::X64;
  Unmapped.Bits = Bitness::Bits64;
  NdOpEmulator Suppressed(Unmapped);
  Suppressed.setStrictMode(true);
  Suppressed.setRegister(x86reg::RAX, UINT64_C(0x9000));
  Suppressed.setRegister(x86reg::K2, 0);
  Suppressed.setRegisterBytes(x86reg::vectorReg(3), SourceValue);
  ASSERT_EQ(Suppressed.run(Ops), Ops.size());
  EXPECT_FALSE(Suppressed.skips().any());

  constexpr uint64_t Address = UINT64_C(0xa000);
  std::vector<uint8_t> Initial(8, 0x5a);
  BinaryImage Partial = writableImage(Address, Initial);
  NdOpEmulator Faulting(Partial);
  Faulting.setStrictMode(true);
  Faulting.setRegister(x86reg::RAX, Address);
  Faulting.setRegister(x86reg::K2, (UINT64_C(1) << 0) | (UINT64_C(1) << 12));
  Faulting.setRegisterBytes(x86reg::vectorReg(3), SourceValue);
  EXPECT_LT(Faulting.run(Ops), Ops.size());
  const auto Unchanged = readMemoryBytes(Faulting, Address, Initial.size());
  ASSERT_TRUE(Unchanged);
  EXPECT_EQ(*Unchanged, Initial);
}

TEST(X86EVEXNarrow, FsAddr32Disp8ScalesByNarrowDestinationTuple) {
  const NarrowFamily &Family = kFamilies[1]; // vpmovdb m128, zmm
  // vpmovdb xmmword ptr fs:[eax + 0x20] {k2}, zmm3. The encoded disp8=2
  // scales by the 16-byte destination tuple.
  const std::vector<LowOp> Ops =
      liftX64({0x64, 0x67, 0x62, 0xf2, 0x7e, 0x4a, 0x31, 0x58, 0x02});
  ASSERT_FALSE(Ops.empty());
  constexpr uint64_t FsBase = UINT64_C(0x100000000);
  constexpr uint32_t Eax = UINT32_C(0xffffff00);
  constexpr uint64_t Address = FsBase + UINT64_C(0xffffff20);
  std::vector<uint8_t> Initial(16, 0xa5);
  std::vector<uint8_t> Expected = Initial;
  std::vector<uint8_t> SourceValue(64, 0);
  const uint64_t MaskValue = (UINT64_C(1) << 0) | (UINT64_C(1) << 15);
  for (size_t Lane = 0; Lane < 16; ++Lane) {
    setLane(SourceValue, Lane, 4, UINT32_C(0x76543200) + Lane);
    if (MaskValue & (UINT64_C(1) << Lane))
      Expected[Lane] = static_cast<uint8_t>(Lane);
  }

  BinaryImage Image = writableImage(Address, Initial);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  ASSERT_TRUE(
      Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, FsBase));
  Emulator.setRegister(x86reg::RAX, UINT64_C(0xaaaaaaaa00000000) | Eax);
  Emulator.setRegister(x86reg::K2, MaskValue);
  Emulator.setRegisterBytes(x86reg::vectorReg(3), SourceValue);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  const auto Result = readMemoryBytes(Emulator, Address, Expected.size());
  ASSERT_TRUE(Result);
  EXPECT_EQ(*Result, Expected);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86EVEXNarrow, ReservedModifiersFailClosed) {
  const std::array<std::vector<uint8_t>, 5> Invalid = {{
      // EVEX.b, reserved LL=3, reserved vvvv, z without a real writemask,
      // and {z} on a memory destination.
      {0x62, 0xf2, 0x7e, 0x5a, 0x31, 0xd9},
      {0x62, 0xf2, 0x7e, 0x6a, 0x25, 0xd9},
      {0x62, 0xf2, 0x76, 0x4a, 0x10, 0xd9},
      {0x62, 0xf2, 0x7e, 0xc8, 0x31, 0xd9},
      {0x62, 0xf2, 0x7e, 0xca, 0x31, 0x18},
  }};

  for (const std::vector<uint8_t> &Bytes : Invalid) {
    SCOPED_TRACE(testing::Message()
                 << "size=" << Bytes.size() << ", opcode=" << std::hex
                 << static_cast<unsigned>(Bytes[Bytes.size() - 2]));
    expectDecodeOrLiftRejected(Bytes);
  }
}

} // namespace
