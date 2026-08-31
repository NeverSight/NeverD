//===- X86_64_EVEXLaneMoveTests.cpp - EVEX lane move semantics ----------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kAddress = 0x1000;

enum class MaskMode : uint8_t {
  None,
  Merge,
  Zero,
};

struct LaneFamily {
  const char *Name;
  unsigned Id;
  uint8_t Opcode;
  uint8_t ElementSize;
  uint8_t LaneSize;
  bool W;
};

constexpr std::array<LaneFamily, 2> kRegisterBroadcasts = {{
    {"vbroadcastf32x2", X86_INS_VBROADCASTF32X2, 0x19, 4, 8, false},
    {"vbroadcasti32x2", X86_INS_VBROADCASTI32X2, 0x59, 4, 8, false},
}};

constexpr std::array<LaneFamily, 10> kAllBroadcasts = {{
    {"vbroadcastf32x2", X86_INS_VBROADCASTF32X2, 0x19, 4, 8, false},
    {"vbroadcastf32x4", X86_INS_VBROADCASTF32X4, 0x1a, 4, 16, false},
    {"vbroadcastf32x8", X86_INS_VBROADCASTF32X8, 0x1b, 4, 32, false},
    {"vbroadcastf64x2", X86_INS_VBROADCASTF64X2, 0x1a, 8, 16, true},
    {"vbroadcastf64x4", X86_INS_VBROADCASTF64X4, 0x1b, 8, 32, true},
    {"vbroadcasti32x2", X86_INS_VBROADCASTI32X2, 0x59, 4, 8, false},
    {"vbroadcasti32x4", X86_INS_VBROADCASTI32X4, 0x5a, 4, 16, false},
    {"vbroadcasti32x8", X86_INS_VBROADCASTI32X8, 0x5b, 4, 32, false},
    {"vbroadcasti64x2", X86_INS_VBROADCASTI64X2, 0x5a, 8, 16, true},
    {"vbroadcasti64x4", X86_INS_VBROADCASTI64X4, 0x5b, 8, 32, true},
}};

constexpr std::array<LaneFamily, 8> kInserts = {{
    {"vinsertf32x4", X86_INS_VINSERTF32X4, 0x18, 4, 16, false},
    {"vinsertf32x8", X86_INS_VINSERTF32X8, 0x1a, 4, 32, false},
    {"vinsertf64x2", X86_INS_VINSERTF64X2, 0x18, 8, 16, true},
    {"vinsertf64x4", X86_INS_VINSERTF64X4, 0x1a, 8, 32, true},
    {"vinserti32x4", X86_INS_VINSERTI32X4, 0x38, 4, 16, false},
    {"vinserti32x8", X86_INS_VINSERTI32X8, 0x3a, 4, 32, false},
    {"vinserti64x2", X86_INS_VINSERTI64X2, 0x38, 8, 16, true},
    {"vinserti64x4", X86_INS_VINSERTI64X4, 0x3a, 8, 32, true},
}};

constexpr std::array<LaneFamily, 8> kExtracts = {{
    {"vextractf32x4", X86_INS_VEXTRACTF32X4, 0x19, 4, 16, false},
    {"vextractf32x8", X86_INS_VEXTRACTF32X8, 0x1b, 4, 32, false},
    {"vextractf64x2", X86_INS_VEXTRACTF64X2, 0x19, 8, 16, true},
    {"vextractf64x4", X86_INS_VEXTRACTF64X4, 0x1b, 8, 32, true},
    {"vextracti32x4", X86_INS_VEXTRACTI32X4, 0x39, 4, 16, false},
    {"vextracti32x8", X86_INS_VEXTRACTI32X8, 0x3b, 4, 32, false},
    {"vextracti64x2", X86_INS_VEXTRACTI64X2, 0x39, 8, 16, true},
    {"vextracti64x4", X86_INS_VEXTRACTI64X4, 0x3b, 8, 32, true},
}};

uint8_t lengthBits(unsigned VectorSize) {
  return VectorSize == 16 ? 0x00 : (VectorSize == 32 ? 0x20 : 0x40);
}

uint8_t maskBits(MaskMode Mode) {
  return Mode == MaskMode::None
             ? 0
             : static_cast<uint8_t>(0x03 |
                                    (Mode == MaskMode::Zero ? 0x80 : 0));
}

std::vector<uint8_t> broadcastEncoding(const LaneFamily &Family,
                                       unsigned VectorSize, MaskMode Mask,
                                       bool Memory) {
  return {0x62,
          0xf2,
          static_cast<uint8_t>(Family.W ? 0xfd : 0x7d),
          static_cast<uint8_t>(0x08 | lengthBits(VectorSize) |
                               maskBits(Mask)),
          Family.Opcode,
          static_cast<uint8_t>(Memory ? 0x08 : 0xca)};
}

std::vector<uint8_t> insertEncoding(const LaneFamily &Family,
                                    unsigned VectorSize, MaskMode Mask,
                                    uint8_t Immediate, bool Memory) {
  return {0x62,
          0xf3,
          static_cast<uint8_t>(Family.W ? 0xed : 0x6d),
          static_cast<uint8_t>(0x08 | lengthBits(VectorSize) |
                               maskBits(Mask)),
          Family.Opcode,
          static_cast<uint8_t>(Memory ? 0x08 : 0xcb),
          Immediate};
}

std::vector<uint8_t> extractEncoding(const LaneFamily &Family,
                                     unsigned SourceSize, MaskMode Mask,
                                     uint8_t Immediate, bool Memory) {
  return {0x62,
          0xf3,
          static_cast<uint8_t>(Family.W ? 0xfd : 0x7d),
          static_cast<uint8_t>(0x08 | lengthBits(SourceSize) |
                               maskBits(Mask)),
          Family.Opcode,
          static_cast<uint8_t>(Memory ? 0x10 : 0xd1),
          Immediate};
}

std::vector<uint8_t> pattern(size_t Size, uint8_t Seed) {
  std::vector<uint8_t> Value(Size);
  for (size_t Index = 0; Index < Size; ++Index)
    Value[Index] = static_cast<uint8_t>(Seed + Index * 29u);
  return Value;
}

uint64_t laneMask(unsigned LaneCount) {
  uint64_t Result = 0;
  for (unsigned Lane = 0; Lane < LaneCount; ++Lane)
    if (Lane % 3 != 1)
      Result |= UINT64_C(1) << Lane;
  return Result;
}

void applyMask(std::vector<uint8_t> &Expected,
               const std::vector<uint8_t> &Raw,
               const std::vector<uint8_t> &Old, unsigned ActiveSize,
               unsigned ElementSize, MaskMode Mode, uint64_t Mask) {
  Expected.assign(64, 0);
  for (unsigned Offset = 0; Offset < ActiveSize; Offset += ElementSize) {
    const unsigned Lane = Offset / ElementSize;
    const bool Active = Mode == MaskMode::None ||
                        (Mask & (UINT64_C(1) << Lane)) != 0;
    const std::vector<uint8_t> *Source =
        Active ? &Raw : (Mode == MaskMode::Merge ? &Old : nullptr);
    if (Source)
      std::copy_n(Source->begin() + Offset, ElementSize,
                  Expected.begin() + Offset);
  }
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

std::optional<std::vector<uint8_t>>
runRegisterCase(const std::vector<uint8_t> &Bytes, unsigned ExpectedId,
                const std::vector<uint8_t> &Destination,
                const std::vector<uint8_t> &Source1,
                const std::vector<uint8_t> &Source2, uint64_t Mask) {
  const auto Ops = liftX64(Bytes, ExpectedId);
  if (!Ops || Ops->empty()) {
    ADD_FAILURE() << "no LowIR was produced";
    return std::nullopt;
  }

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(x86reg::vectorReg(1), Destination);
  Emulator.setRegisterBytes(x86reg::vectorReg(2), Source1);
  Emulator.setRegisterBytes(x86reg::vectorReg(3), Source2);
  Emulator.setRegister(x86reg::K3, Mask);
  if (Emulator.run(*Ops) != Ops->size()) {
    ADD_FAILURE() << "strict LowIR execution stopped early";
    return std::nullopt;
  }
  return Emulator.getRegisterBytes(x86reg::vectorReg(1));
}

BinaryImage makeMemoryImage(uint64_t Address,
                            const std::vector<uint8_t> &Initial) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Data;
  Data.VA = Address;
  Data.Size = Initial.size();
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data = Initial;
  Image.Segments.push_back(std::move(Data));
  return Image;
}

std::optional<std::vector<uint8_t>>
runMemorySourceCase(const std::vector<uint8_t> &Bytes, unsigned ExpectedId,
                    BinaryImage &Image, uint64_t Address,
                    const std::vector<uint8_t> &Destination,
                    const std::vector<uint8_t> &Base) {
  const auto Ops = liftX64(Bytes, ExpectedId);
  if (!Ops || Ops->empty()) {
    ADD_FAILURE() << "no LowIR was produced";
    return std::nullopt;
  }
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RAX, Address);
  Emulator.setRegisterBytes(x86reg::vectorReg(1), Destination);
  Emulator.setRegisterBytes(x86reg::vectorReg(2), Base);
  if (Emulator.run(*Ops) != Ops->size()) {
    ADD_FAILURE() << "strict memory-source execution stopped early";
    return std::nullopt;
  }
  return Emulator.getRegisterBytes(x86reg::vectorReg(1));
}

std::optional<std::vector<uint8_t>> readMemoryBytes(NdOpEmulator &Emulator,
                                                    uint64_t Address,
                                                    size_t Size) {
  std::vector<uint8_t> Result(Size);
  size_t Offset = 0;
  uint64_t Temp = UINT64_C(0x7e000000);
  while (Offset < Size) {
    const uint16_t Chunk =
        static_cast<uint16_t>(std::min<size_t>(8, Size - Offset));
    LowOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = NdVar::tmp(Temp++, Chunk);
    Load.addInput(NdVar::cst(0, 8));
    Load.addInput(NdVar::cst(Address + Offset, 8));
    if (!Emulator.step(Load)) {
      ADD_FAILURE() << "memory probe failed";
      return std::nullopt;
    }
    const auto Value = Emulator.getRegister(Temp - 1);
    if (!Value) {
      ADD_FAILURE() << "memory probe produced no value";
      return std::nullopt;
    }
    std::memcpy(Result.data() + Offset, &*Value, Chunk);
    Offset += Chunk;
  }
  return Result;
}

TEST(X86EVEXLaneMove, RegisterBroadcastsRepeatTuplesAndApplyWritemask) {
  const std::vector<uint8_t> Old = pattern(64, 0x91);
  const std::vector<uint8_t> Source = pattern(64, 0x23);
  const std::vector<uint8_t> Unused = pattern(64, 0xd1);

  for (const LaneFamily &Family : kRegisterBroadcasts) {
    const std::array<unsigned, 3> Sizes =
        Family.Id == X86_INS_VBROADCASTI32X2
            ? std::array<unsigned, 3>{16, 32, 64}
            : std::array<unsigned, 3>{32, 64, 0};
    for (unsigned Size : Sizes) {
      if (Size == 0)
        continue;
      std::vector<uint8_t> Raw(64, 0);
      for (unsigned Byte = 0; Byte < Size; ++Byte)
        Raw[Byte] = Source[Byte % Family.LaneSize];
      const uint64_t Mask = laneMask(Size / Family.ElementSize);
      for (MaskMode Mode : {MaskMode::None, MaskMode::Merge, MaskMode::Zero}) {
        SCOPED_TRACE(std::string(Family.Name) + ", size=" +
                     std::to_string(Size) + ", mask=" +
                     std::to_string(static_cast<unsigned>(Mode)));
        std::vector<uint8_t> Expected;
        applyMask(Expected, Raw, Old, Size, Family.ElementSize, Mode, Mask);
        const auto Result = runRegisterCase(
            broadcastEncoding(Family, Size, Mode, false), Family.Id, Old,
            Source, Unused, Mask);
        ASSERT_TRUE(Result);
        EXPECT_EQ(*Result, Expected);
      }
    }
  }
}

TEST(X86EVEXLaneMove, RegisterInsertsHonorLaneImmediateAndWritemask) {
  const std::vector<uint8_t> Old = pattern(64, 0x81);
  const std::vector<uint8_t> Base = pattern(64, 0x17);
  const std::vector<uint8_t> Lane = pattern(64, 0xc3);
  constexpr uint8_t Immediate = 0xfe;

  for (const LaneFamily &Family : kInserts) {
    const std::array<unsigned, 2> Sizes =
        Family.LaneSize == 16 ? std::array<unsigned, 2>{32, 64}
                              : std::array<unsigned, 2>{64, 0};
    for (unsigned Size : Sizes) {
      if (Size == 0)
        continue;
      std::vector<uint8_t> Raw = Base;
      const unsigned LaneIndex = Immediate & (Size / Family.LaneSize - 1);
      std::copy_n(Lane.begin(), Family.LaneSize,
                  Raw.begin() + LaneIndex * Family.LaneSize);
      const uint64_t Mask = laneMask(Size / Family.ElementSize);
      for (MaskMode Mode : {MaskMode::None, MaskMode::Merge, MaskMode::Zero}) {
        SCOPED_TRACE(std::string(Family.Name) + ", size=" +
                     std::to_string(Size) + ", mask=" +
                     std::to_string(static_cast<unsigned>(Mode)));
        std::vector<uint8_t> Expected;
        applyMask(Expected, Raw, Old, Size, Family.ElementSize, Mode, Mask);
        const auto Result = runRegisterCase(
            insertEncoding(Family, Size, Mode, Immediate, false), Family.Id,
            Old, Base, Lane, Mask);
        ASSERT_TRUE(Result);
        EXPECT_EQ(*Result, Expected);
      }
    }
  }
}

TEST(X86EVEXLaneMove, RegisterExtractsHonorLaneImmediateAndWritemask) {
  const std::vector<uint8_t> Old = pattern(64, 0x71);
  const std::vector<uint8_t> Source = pattern(64, 0x2b);
  const std::vector<uint8_t> Unused = pattern(64, 0xa7);
  constexpr uint8_t Immediate = 0xfd;

  for (const LaneFamily &Family : kExtracts) {
    const std::array<unsigned, 2> Sizes =
        Family.LaneSize == 16 ? std::array<unsigned, 2>{32, 64}
                              : std::array<unsigned, 2>{64, 0};
    for (unsigned Size : Sizes) {
      if (Size == 0)
        continue;
      std::vector<uint8_t> Raw(64, 0);
      const unsigned LaneIndex = Immediate & (Size / Family.LaneSize - 1);
      std::copy_n(Source.begin() + LaneIndex * Family.LaneSize,
                  Family.LaneSize, Raw.begin());
      const uint64_t Mask = laneMask(Family.LaneSize / Family.ElementSize);
      for (MaskMode Mode : {MaskMode::None, MaskMode::Merge, MaskMode::Zero}) {
        SCOPED_TRACE(std::string(Family.Name) + ", source size=" +
                     std::to_string(Size) + ", mask=" +
                     std::to_string(static_cast<unsigned>(Mode)));
        std::vector<uint8_t> Expected;
        applyMask(Expected, Raw, Old, Family.LaneSize, Family.ElementSize,
                  Mode, Mask);
        const auto Result = runRegisterCase(
            extractEncoding(Family, Size, Mode, Immediate, false), Family.Id,
            Old, Source, Unused, Mask);
        ASSERT_TRUE(Result);
        EXPECT_EQ(*Result, Expected);
      }
    }
  }
}

TEST(X86EVEXLaneMove, HighRegisterExtensionBitsAreHonored) {
  const std::vector<uint8_t> Old = pattern(64, 0x53);
  const std::vector<uint8_t> Source29 = pattern(64, 0x21);
  const std::vector<uint8_t> Source30 = pattern(64, 0x97);
  const uint64_t Mask = laneMask(16);

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(x86reg::vectorReg(29), Source29);
  Emulator.setRegisterBytes(x86reg::vectorReg(30), Source30);
  Emulator.setRegisterBytes(x86reg::vectorReg(31), Old);
  Emulator.setRegister(x86reg::K7, Mask);

  const auto Broadcast =
      liftX64({0x62, 0x02, 0x7d, 0xcf, 0x19, 0xfd},
              X86_INS_VBROADCASTF32X2);
  ASSERT_TRUE(Broadcast);
  ASSERT_EQ(Emulator.run(*Broadcast), Broadcast->size());
  std::vector<uint8_t> BroadcastRaw(64);
  for (size_t Byte = 0; Byte < BroadcastRaw.size(); ++Byte)
    BroadcastRaw[Byte] = Source29[Byte % 8];
  std::vector<uint8_t> Expected;
  applyMask(Expected, BroadcastRaw, Old, 64, 4, MaskMode::Zero, Mask);
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(31)), Expected);

  Emulator.setRegisterBytes(x86reg::vectorReg(31), Old);
  const auto Insert =
      liftX64({0x62, 0x03, 0x0d, 0xc7, 0x18, 0xfd, 0x03},
              X86_INS_VINSERTF32X4);
  ASSERT_TRUE(Insert);
  ASSERT_EQ(Emulator.run(*Insert), Insert->size());
  std::vector<uint8_t> InsertRaw = Source30;
  std::copy_n(Source29.begin(), 16, InsertRaw.begin() + 48);
  applyMask(Expected, InsertRaw, Old, 64, 4, MaskMode::Zero, Mask);
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(31)), Expected);

  Emulator.setRegisterBytes(x86reg::vectorReg(29), Old);
  const auto Extract =
      liftX64({0x62, 0x03, 0x7d, 0xcf, 0x19, 0xf5, 0x03},
              X86_INS_VEXTRACTF32X4);
  ASSERT_TRUE(Extract);
  ASSERT_EQ(Emulator.run(*Extract), Extract->size());
  std::vector<uint8_t> ExtractRaw(64, 0);
  std::copy_n(Source30.begin() + 48, 16, ExtractRaw.begin());
  applyMask(Expected, ExtractRaw, Old, 16, 4, MaskMode::Zero, Mask);
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(29)), Expected);
}

TEST(X86EVEXLaneMove, UnmaskedMemoryFormsUseExactTupleWidth) {
  constexpr uint64_t MemoryAddress = 0x4000;
  const std::vector<uint8_t> Old = pattern(64, 0x63);
  const std::vector<uint8_t> Base = pattern(64, 0x19);
  const std::vector<uint8_t> Memory = pattern(64, 0xb3);

  for (const LaneFamily &Family : kAllBroadcasts) {
    SCOPED_TRACE(Family.Name);
    BinaryImage Image = makeMemoryImage(MemoryAddress, Memory);
    std::vector<uint8_t> Expected(64);
    for (size_t Byte = 0; Byte < Expected.size(); ++Byte)
      Expected[Byte] = Memory[Byte % Family.LaneSize];
    const auto Result = runMemorySourceCase(
        broadcastEncoding(Family, 64, MaskMode::None, true), Family.Id, Image,
        MemoryAddress, Old, Base);
    ASSERT_TRUE(Result);
    EXPECT_EQ(*Result, Expected);
  }

  constexpr uint8_t InsertImmediate = 0xff;
  for (const LaneFamily &Family : kInserts) {
    SCOPED_TRACE(Family.Name);
    BinaryImage Image = makeMemoryImage(MemoryAddress, Memory);
    std::vector<uint8_t> Expected = Base;
    const unsigned Selected = InsertImmediate & (64 / Family.LaneSize - 1);
    std::copy_n(Memory.begin(), Family.LaneSize,
                Expected.begin() + Selected * Family.LaneSize);
    const auto Result = runMemorySourceCase(
        insertEncoding(Family, 64, MaskMode::None, InsertImmediate, true),
        Family.Id, Image, MemoryAddress, Old, Base);
    ASSERT_TRUE(Result);
    EXPECT_EQ(*Result, Expected);
  }

  constexpr uint8_t ExtractImmediate = 0xff;
  for (const LaneFamily &Family : kExtracts) {
    SCOPED_TRACE(Family.Name);
    BinaryImage Image = makeMemoryImage(MemoryAddress, Old);
    const auto Ops = liftX64(
        extractEncoding(Family, 64, MaskMode::None, ExtractImmediate, true),
        Family.Id);
    ASSERT_TRUE(Ops);
    ASSERT_FALSE(Ops->empty());
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::RAX, MemoryAddress);
    Emulator.setRegisterBytes(x86reg::vectorReg(2), Base);
    ASSERT_EQ(Emulator.run(*Ops), Ops->size());

    std::vector<uint8_t> Expected = Old;
    const unsigned Selected = ExtractImmediate & (64 / Family.LaneSize - 1);
    std::copy_n(Base.begin() + Selected * Family.LaneSize, Family.LaneSize,
                Expected.begin());
    const auto Result = readMemoryBytes(Emulator, MemoryAddress, Old.size());
    ASSERT_TRUE(Result);
    EXPECT_EQ(*Result, Expected);
  }

  // A normal tuple load still faults before any destination update.
  BinaryImage Partial =
      makeMemoryImage(UINT64_C(0x5000), pattern(8, 0xd7));
  const auto FaultOps =
      liftX64(broadcastEncoding(kAllBroadcasts[1], 64, MaskMode::None, true),
              kAllBroadcasts[1].Id);
  ASSERT_TRUE(FaultOps);
  ASSERT_FALSE(FaultOps->empty());
  NdOpEmulator Fault(Partial);
  Fault.setStrictMode(true);
  Fault.setRegister(x86reg::RAX, UINT64_C(0x5000));
  Fault.setRegisterBytes(x86reg::vectorReg(1), Old);
  EXPECT_LT(Fault.run(*FaultOps), FaultOps->size());
  EXPECT_EQ(Fault.getRegisterBytes(x86reg::vectorReg(1)), Old);
}

TEST(X86EVEXLaneMove, EveryMaskedMemoryFamilyHasFaultSuppressingLowering) {
  for (const LaneFamily &Family : kAllBroadcasts) {
    SCOPED_TRACE(Family.Name);
    const unsigned Size = Family.LaneSize == 32 ? 64 : 32;
    const auto Ops = liftX64(
        broadcastEncoding(Family, Size, MaskMode::Merge, true), Family.Id);
    ASSERT_TRUE(Ops);
    EXPECT_FALSE(Ops->empty());
  }
  for (const LaneFamily &Family : kInserts) {
    SCOPED_TRACE(Family.Name);
    const auto Ops = liftX64(
        insertEncoding(Family, 64, MaskMode::Merge, 1, true), Family.Id);
    ASSERT_TRUE(Ops);
    EXPECT_FALSE(Ops->empty());
  }
  for (const LaneFamily &Family : kExtracts) {
    SCOPED_TRACE(Family.Name);
    const auto Ops = liftX64(
        extractEncoding(Family, 64, MaskMode::Merge, 1, true), Family.Id);
    ASSERT_TRUE(Ops);
    EXPECT_FALSE(Ops->empty());
  }
}

} // namespace
