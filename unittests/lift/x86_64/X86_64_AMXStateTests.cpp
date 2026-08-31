//===- X86_64_AMXStateTests.cpp - AMX state semantics -------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;

struct LiftedInstruction {
  unsigned Id = X86_INS_INVALID;
  std::vector<LowOp> Ops;
};

LiftedInstruction liftX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "failed to initialize x86-64 decoder";
    return {};
  }
  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kInstructionAddress,
                           Insn) != static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "failed to decode complete instruction";
    return {};
  }
  LiftedInstruction Result;
  Result.Id = Insn.Id;
  try {
    Dec.liftToLow(Insn, Result.Ops);
  } catch (const UnliftedInstruction &) {
    ADD_FAILURE() << "decoded AMX instruction was not lifted";
  }
  return Result;
}

Segment makeSegment(uint64_t Address, size_t Size, SegmentFlags Flags) {
  Segment Result;
  Result.VA = Address;
  Result.Size = Size;
  Result.Flags = Flags;
  Result.Data.resize(Size);
  return Result;
}

BinaryImage makeImage(std::vector<Segment> Segments) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Image.Segments = std::move(Segments);
  return Image;
}

std::array<uint8_t, x86reg::TileConfigSize>
makeConfig(uint8_t StartRow = 0) {
  std::array<uint8_t, x86reg::TileConfigSize> Config{};
  Config[0] = 1;
  Config[1] = StartRow;
  return Config;
}

void configureTile(std::array<uint8_t, x86reg::TileConfigSize> &Config,
                   unsigned Tile, uint16_t ColumnBytes, uint8_t Rows) {
  ASSERT_LT(Tile, x86reg::TileRegCount);
  Config[16 + Tile * 2] = static_cast<uint8_t>(ColumnBytes);
  Config[17 + Tile * 2] = static_cast<uint8_t>(ColumnBytes >> 8);
  Config[48 + Tile] = Rows;
}

void writeImageBytes(BinaryImage &Image, uint64_t Address,
                     const uint8_t *Bytes, size_t Size) {
  for (Segment &Seg : Image.Segments) {
    if (Address >= Seg.VA && Address - Seg.VA <= Seg.Data.size() &&
        Size <= Seg.Data.size() - (Address - Seg.VA)) {
      std::copy_n(Bytes, Size, Seg.Data.begin() + (Address - Seg.VA));
      return;
    }
  }
  ADD_FAILURE() << "test memory write is outside the image";
}

uint8_t probeByte(NdOpEmulator &Emulator, uint64_t Address,
                  uint64_t TempOffset) {
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::tmp(TempOffset, 1);
  Load.addInput(NdVar::cst(0, 8));
  Load.addInput(NdVar::cst(Address, 8));
  EXPECT_TRUE(Emulator.step(Load));
  return static_cast<uint8_t>(Emulator.getRegister(TempOffset).value_or(0));
}

void writeDword(std::vector<uint8_t> &Tile, unsigned Row, unsigned Column,
                uint32_t Value) {
  const size_t Offset = static_cast<size_t>(Row) * 64 + Column * 4;
  ASSERT_LE(Offset + 4, Tile.size());
  for (unsigned Byte = 0; Byte < 4; ++Byte)
    Tile[Offset + Byte] = static_cast<uint8_t>(Value >> (Byte * 8));
}

uint32_t readDword(const std::vector<uint8_t> &Tile, unsigned Row,
                   unsigned Column) {
  const size_t Offset = static_cast<size_t>(Row) * 64 + Column * 4;
  EXPECT_LE(Offset + 4, Tile.size());
  uint32_t Value = 0;
  for (unsigned Byte = 0; Byte < 4 && Offset + Byte < Tile.size(); ++Byte)
    Value |= static_cast<uint32_t>(Tile[Offset + Byte]) << (Byte * 8);
  return Value;
}

TEST(X86AMXState, ConfigLoadStoreAndReleasePreserveExactArchitecturalState) {
  constexpr uint64_t ConfigAddress = 0x4000;
  constexpr uint64_t StoredAddress = 0x5000;
  auto Config = makeConfig();
  configureTile(Config, 1, 8, 3);
  configureTile(Config, 7, 12, 2);

  BinaryImage Image = makeImage({makeSegment(
      ConfigAddress, 0x1100,
      SegmentFlags::Readable | SegmentFlags::Writable)});
  writeImageBytes(Image, ConfigAddress, Config.data(), Config.size());

  const LiftedInstruction Load =
      liftX64({0xc4, 0xe2, 0x78, 0x49, 0x00}); // ldtilecfg [rax]
  const LiftedInstruction Store =
      liftX64({0xc4, 0xc2, 0x79, 0x49, 0x07}); // sttilecfg [r15]
  const LiftedInstruction Release =
      liftX64({0xc4, 0xe2, 0x78, 0x49, 0xc0}); // tilerelease
  ASSERT_EQ(Load.Id, X86_INS_LDTILECFG);
  ASSERT_EQ(Store.Id, X86_INS_STTILECFG);
  ASSERT_EQ(Release.Id, X86_INS_TILERELEASE);
  ASSERT_FALSE(Load.Ops.empty());
  ASSERT_FALSE(Store.Ops.empty());
  ASSERT_FALSE(Release.Ops.empty());

  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RAX, ConfigAddress);
  for (unsigned Tile = 0; Tile < x86reg::TileRegCount; ++Tile)
    Emulator.setRegisterBytes(x86reg::tileReg(Tile),
                              std::vector<uint8_t>(1024, 0xa5 + Tile));
  ASSERT_EQ(Emulator.run(Load.Ops), Load.Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::TileConfig),
            std::vector<uint8_t>(Config.begin(), Config.end()));
  for (unsigned Tile = 0; Tile < x86reg::TileRegCount; ++Tile) {
    const auto Value = Emulator.getRegisterBytes(x86reg::tileReg(Tile));
    ASSERT_TRUE(Value);
    EXPECT_TRUE(std::all_of(Value->begin(), Value->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
  }

  Emulator.setRegister(x86reg::R15, StoredAddress);
  ASSERT_EQ(Emulator.run(Store.Ops), Store.Ops.size());
  for (size_t I = 0; I < Config.size(); ++I)
    EXPECT_EQ(probeByte(Emulator, StoredAddress + I, 0x70000000 + I),
              Config[I]);

  Emulator.setRegisterBytes(x86reg::TMM3, std::vector<uint8_t>(1024, 0x5a));
  ASSERT_EQ(Emulator.run(Release.Ops), Release.Ops.size());
  const auto ReleasedConfig = Emulator.getRegisterBytes(x86reg::TileConfig);
  ASSERT_TRUE(ReleasedConfig);
  EXPECT_TRUE(std::all_of(ReleasedConfig->begin(), ReleasedConfig->end(),
                          [](uint8_t Byte) { return Byte == 0; }));
  const auto ReleasedTile = Emulator.getRegisterBytes(x86reg::TMM3);
  ASSERT_TRUE(ReleasedTile);
  EXPECT_TRUE(std::all_of(ReleasedTile->begin(), ReleasedTile->end(),
                          [](uint8_t Byte) { return Byte == 0; }));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86AMXState, InvalidConfigFaultsBeforeChangingConfigOrTileData) {
  constexpr uint64_t ConfigAddress = 0x4000;
  const LiftedInstruction Load =
      liftX64({0xc4, 0xe2, 0x78, 0x49, 0x00}); // ldtilecfg [rax]
  ASSERT_FALSE(Load.Ops.empty());

  std::vector<std::array<uint8_t, x86reg::TileConfigSize>> Invalid;
  auto Reserved = makeConfig();
  Reserved[7] = 1;
  Invalid.push_back(Reserved);
  auto MismatchedShape = makeConfig();
  configureTile(MismatchedShape, 2, 8, 0);
  Invalid.push_back(MismatchedShape);
  auto TooWide = makeConfig();
  configureTile(TooWide, 3, 68, 1);
  Invalid.push_back(TooWide);
  auto UnknownPalette = makeConfig();
  UnknownPalette[0] = 2;
  Invalid.push_back(UnknownPalette);

  std::vector<uint8_t> OriginalConfig(x86reg::TileConfigSize, 0x3c);
  std::vector<uint8_t> OriginalTile(x86reg::TileRegStride, 0x96);
  for (const auto &Candidate : Invalid) {
    BinaryImage Image = makeImage({makeSegment(
        ConfigAddress, Candidate.size(), SegmentFlags::Readable)});
    writeImageBytes(Image, ConfigAddress, Candidate.data(), Candidate.size());
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::RAX, ConfigAddress);
    Emulator.setRegisterBytes(x86reg::TileConfig, OriginalConfig);
    Emulator.setRegisterBytes(x86reg::TMM2, OriginalTile);
    EXPECT_LT(Emulator.run(Load.Ops), Load.Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::TileConfig), OriginalConfig);
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::TMM2), OriginalTile);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86AMXState, IntegerDotProductsHonorSignednessAndDwordWrap) {
  struct ComputeCase {
    std::array<uint8_t, 5> Bytes;
    unsigned Id;
    int64_t DotProduct;
  };
  const std::array<ComputeCase, 4> Cases = {{
      {{{0xc4, 0xe2, 0x63, 0x5e, 0xca}}, X86_INS_TDPBSSD, -130},
      {{{0xc4, 0xe2, 0x62, 0x5e, 0xca}}, X86_INS_TDPBSUD, 126},
      {{{0xc4, 0xe2, 0x61, 0x5e, 0xca}}, X86_INS_TDPBUSD, 126},
      {{{0xc4, 0xe2, 0x60, 0x5e, 0xca}}, X86_INS_TDPBUUD, 65918},
  }};

  auto Config = makeConfig(1);
  configureTile(Config, 1, 4, 1);
  configureTile(Config, 2, 4, 1);
  configureTile(Config, 3, 4, 1);
  const std::array<uint8_t, 4> Src1Bytes = {0xff, 0x80, 0x02, 0x7f};
  const std::array<uint8_t, 4> Src2Bytes = {0xfe, 0x03, 0x80, 0x04};
  constexpr uint32_t Initial = 0xfffffff0U;

  for (const ComputeCase &Case : Cases) {
    const LiftedInstruction Compute = liftX64(
        std::vector<uint8_t>(Case.Bytes.begin(), Case.Bytes.end()));
    ASSERT_EQ(Compute.Id, Case.Id);
    ASSERT_FALSE(Compute.Ops.empty());

    std::vector<uint8_t> Destination(x86reg::TileRegStride, 0xa5);
    std::vector<uint8_t> Source1(x86reg::TileRegStride, 0x5a);
    std::vector<uint8_t> Source2(x86reg::TileRegStride, 0xc3);
    writeDword(Destination, 0, 0, Initial);
    std::copy(Src1Bytes.begin(), Src1Bytes.end(), Source1.begin());
    std::copy(Src2Bytes.begin(), Src2Bytes.end(), Source2.begin());

    BinaryImage Image = makeImage({});
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::TileConfig, Config);
    Emulator.setRegisterBytes(x86reg::TMM1, Destination);
    Emulator.setRegisterBytes(x86reg::TMM2, Source1);
    Emulator.setRegisterBytes(x86reg::TMM3, Source2);
    ASSERT_EQ(Emulator.run(Compute.Ops), Compute.Ops.size());

    const auto Result = Emulator.getRegisterBytes(x86reg::TMM1);
    const auto ResultConfig =
        Emulator.getRegisterBytes(x86reg::TileConfig);
    ASSERT_TRUE(Result);
    ASSERT_TRUE(ResultConfig);
    EXPECT_EQ(readDword(*Result, 0, 0),
              static_cast<uint32_t>(static_cast<int64_t>(Initial) +
                                    Case.DotProduct));
    EXPECT_TRUE(std::all_of(Result->begin() + 4, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::TMM2), Source1);
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::TMM3), Source2);
    EXPECT_EQ((*ResultConfig)[1], 0);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86AMXState, IntegerDotProductsRejectMismatchedTileShapesBeforeEffect) {
  const LiftedInstruction Compute =
      liftX64({0xc4, 0xe2, 0x63, 0x5e, 0xca});
  ASSERT_EQ(Compute.Id, X86_INS_TDPBSSD);
  ASSERT_FALSE(Compute.Ops.empty());

  auto Config = makeConfig(1);
  configureTile(Config, 1, 4, 1);
  configureTile(Config, 2, 8, 1);
  configureTile(Config, 3, 4, 1);
  const std::vector<uint8_t> OriginalDestination(x86reg::TileRegStride, 0x6d);
  BinaryImage Image = makeImage({});
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(x86reg::TileConfig, Config);
  Emulator.setRegisterBytes(x86reg::TMM1, OriginalDestination);
  Emulator.setRegisterBytes(x86reg::TMM2,
                            std::vector<uint8_t>(x86reg::TileRegStride, 1));
  Emulator.setRegisterBytes(x86reg::TMM3,
                            std::vector<uint8_t>(x86reg::TileRegStride, 1));

  EXPECT_LT(Emulator.run(Compute.Ops), Compute.Ops.size());
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::TMM1), OriginalDestination);
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::TileConfig),
            std::vector<uint8_t>(Config.begin(), Config.end()));
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86AMXState, FloatingTileComputesUseInstructionDefinedFormats) {
  struct ComputeCase {
    std::array<uint8_t, 5> Bytes;
    unsigned Id;
    uint32_t Source1;
    uint32_t Source2;
    uint32_t Expected;
  };
  const std::array<ComputeCase, 9> Cases = {{
      {{{0xc4, 0xe2, 0x62, 0x5c, 0xca}}, X86_INS_TDPBF16PS,
       0x40003f80U, 0x40804040U, 0x41800000U},
      {{{0xc4, 0xe2, 0x63, 0x5c, 0xca}}, X86_INS_TDPFP16PS,
       0x40003c00U, 0x44004200U, 0x41800000U},
      {{{0xc4, 0xe2, 0x61, 0x6c, 0xca}}, X86_INS_TCMMIMFP16PS,
       0x40003c00U, 0x44004200U, 0x41700000U},
      {{{0xc4, 0xe2, 0x60, 0x6c, 0xca}}, X86_INS_TCMMRLFP16PS,
       0x40003c00U, 0x44004200U, 0x00000000U},
      {{{0xc4, 0xe5, 0x60, 0xfd, 0xca}}, X86_INS_TDPBF8PS,
       0x403c403cU, 0x44424442U, 0x41d80000U},
      {{{0xc4, 0xe5, 0x63, 0xfd, 0xca}}, X86_INS_TDPBHF8PS,
       0x403c403cU, 0x48444844U, 0x41d80000U},
      {{{0xc4, 0xe5, 0x62, 0xfd, 0xca}}, X86_INS_TDPHBF8PS,
       0x40384038U, 0x44424442U, 0x41d80000U},
      {{{0xc4, 0xe5, 0x61, 0xfd, 0xca}}, X86_INS_TDPHF8PS,
       0x40384038U, 0x48444844U, 0x41d80000U},
      {{{0xc4, 0xe2, 0x61, 0x48, 0xca}}, X86_INS_TMMULTF32PS,
       0x3fc00000U, 0x40000000U, 0x41000000U},
  }};

  auto Config = makeConfig(1);
  configureTile(Config, 1, 4, 1);
  configureTile(Config, 2, 4, 1);
  configureTile(Config, 3, 4, 1);
  for (const ComputeCase &Case : Cases) {
    const LiftedInstruction Compute = liftX64(
        std::vector<uint8_t>(Case.Bytes.begin(), Case.Bytes.end()));
    ASSERT_EQ(Compute.Id, Case.Id);
    ASSERT_FALSE(Compute.Ops.empty());

    std::vector<uint8_t> Destination(x86reg::TileRegStride, 0xa5);
    std::vector<uint8_t> Source1(x86reg::TileRegStride, 0x5a);
    std::vector<uint8_t> Source2(x86reg::TileRegStride, 0xc3);
    writeDword(Destination, 0, 0, 0x40a00000U); // 5.0f
    writeDword(Source1, 0, 0, Case.Source1);
    writeDword(Source2, 0, 0, Case.Source2);

    BinaryImage Image = makeImage({});
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::TileConfig, Config);
    Emulator.setRegisterBytes(x86reg::TMM1, Destination);
    Emulator.setRegisterBytes(x86reg::TMM2, Source1);
    Emulator.setRegisterBytes(x86reg::TMM3, Source2);
    ASSERT_EQ(Emulator.run(Compute.Ops), Compute.Ops.size());
    const auto Result = Emulator.getRegisterBytes(x86reg::TMM1);
    const auto ResultConfig =
        Emulator.getRegisterBytes(x86reg::TileConfig);
    ASSERT_TRUE(Result);
    ASSERT_TRUE(ResultConfig);
    EXPECT_EQ(readDword(*Result, 0, 0), Case.Expected);
    EXPECT_TRUE(std::all_of(Result->begin() + 4, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
    EXPECT_EQ((*ResultConfig)[1], 0);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86AMXState, FloatingTileComputesHonorSpecialValueRules) {
  struct ComputeCase {
    std::array<uint8_t, 5> Bytes;
    uint32_t Accumulator;
    uint32_t Source1;
    uint32_t Source2;
    uint32_t Expected;
  };
  const std::array<ComputeCase, 8> Cases = {{
      {{{0xc4, 0xe2, 0x62, 0x5c, 0xca}}, 0, 0x00000001U,
       0x00003f80U, 0x00000000U},
      {{{0xc4, 0xe2, 0x63, 0x5c, 0xca}}, 0, 0x00000001U,
       0x00003c00U, 0x33800000U},
      {{{0xc4, 0xe5, 0x60, 0xfd, 0xca}}, 0, 0x0000007dU,
       0x3c3c3c3cU, 0xffc00000U},
      {{{0xc4, 0xe2, 0x61, 0x48, 0xca}}, 0, 0x3f801fffU,
       0x3f800000U, 0x3f800000U},
      {{{0xc4, 0xe2, 0x61, 0x48, 0xca}}, 0, 0x007fe000U,
       0x7e800000U, 0x3f7fc000U},
      {{{0xc4, 0xe2, 0x61, 0x48, 0xca}}, 0, 0x7f800001U,
       0x3f800000U, 0x7fc00000U},
      {{{0xc4, 0xe2, 0x61, 0x48, 0xca}}, 0, 0x7fc12345U,
       0x3f800000U, 0x7fc12000U},
      {{{0xc4, 0xe2, 0x61, 0x48, 0xca}}, 0, 0x7f800000U,
       0x00000000U, 0xffc00000U},
  }};

  auto Config = makeConfig(1);
  configureTile(Config, 1, 4, 1);
  configureTile(Config, 2, 4, 1);
  configureTile(Config, 3, 4, 1);
  BinaryImage Image = makeImage({});
  for (const ComputeCase &Case : Cases) {
    const LiftedInstruction Compute = liftX64(
        std::vector<uint8_t>(Case.Bytes.begin(), Case.Bytes.end()));
    ASSERT_FALSE(Compute.Ops.empty());
    std::vector<uint8_t> Destination(x86reg::TileRegStride, 0xa5);
    std::vector<uint8_t> Source1(x86reg::TileRegStride, 0x5a);
    std::vector<uint8_t> Source2(x86reg::TileRegStride, 0xc3);
    writeDword(Destination, 0, 0, Case.Accumulator);
    writeDword(Source1, 0, 0, Case.Source1);
    writeDword(Source2, 0, 0, Case.Source2);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::TileConfig, Config);
    Emulator.setRegisterBytes(x86reg::TMM1, Destination);
    Emulator.setRegisterBytes(x86reg::TMM2, Source1);
    Emulator.setRegisterBytes(x86reg::TMM3, Source2);
    ASSERT_EQ(Emulator.run(Compute.Ops), Compute.Ops.size());
    const auto Result = Emulator.getRegisterBytes(x86reg::TMM1);
    const auto ResultConfig =
        Emulator.getRegisterBytes(x86reg::TileConfig);
    ASSERT_TRUE(Result);
    ASSERT_TRUE(ResultConfig);
    EXPECT_EQ(readDword(*Result, 0, 0), Case.Expected);
    EXPECT_TRUE(std::all_of(Result->begin() + 4, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
    EXPECT_EQ((*ResultConfig)[1], 0);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86AMXState, TileZeroRequiresAValidTileAndClearsRestartState) {
  const LiftedInstruction Zero =
      liftX64({0xc4, 0xe2, 0x7b, 0x49, 0xf0}); // tilezero tmm6
  ASSERT_EQ(Zero.Id, X86_INS_TILEZERO);
  ASSERT_FALSE(Zero.Ops.empty());
  BinaryImage Image = makeImage({});
  const std::vector<uint8_t> Initial(x86reg::TileRegStride, 0xa7);

  NdOpEmulator Inactive(Image);
  Inactive.setStrictMode(true);
  Inactive.setRegisterBytes(x86reg::TileConfig,
                            std::vector<uint8_t>(x86reg::TileConfigSize));
  Inactive.setRegisterBytes(x86reg::TMM6, Initial);
  EXPECT_LT(Inactive.run(Zero.Ops), Zero.Ops.size());
  EXPECT_EQ(Inactive.getRegisterBytes(x86reg::TMM6), Initial);

  auto Config = makeConfig(1);
  configureTile(Config, 6, 8, 2);
  NdOpEmulator Configured(Image);
  Configured.setStrictMode(true);
  Configured.setRegisterBytes(x86reg::TileConfig, Config);
  Configured.setRegisterBytes(x86reg::TMM6, Initial);
  ASSERT_EQ(Configured.run(Zero.Ops), Zero.Ops.size());
  const auto Result = Configured.getRegisterBytes(x86reg::TMM6);
  ASSERT_TRUE(Result);
  EXPECT_TRUE(std::all_of(Result->begin(), Result->end(),
                          [](uint8_t Byte) { return Byte == 0; }));
  const auto ResultConfig = Configured.getRegisterBytes(x86reg::TileConfig);
  ASSERT_TRUE(ResultConfig);
  EXPECT_EQ((*ResultConfig)[1], 0);
  EXPECT_FALSE(Configured.skips().any());
}

TEST(X86AMXState, TileTransfersRejectInvalidRuntimeShapeBeforeAnyEffect) {
  const LiftedInstruction Load =
      liftX64({0xc4, 0xe2, 0x7b, 0x4b, 0x0c, 0x18});
  const LiftedInstruction Store =
      liftX64({0xc4, 0x82, 0x7a, 0x4b, 0x3c, 0xbe});
  ASSERT_FALSE(Load.Ops.empty());
  ASSERT_FALSE(Store.Ops.empty());

  std::vector<std::array<uint8_t, x86reg::TileConfigSize>> Invalid;
  Invalid.emplace_back(); // INIT state: tiles are not configured.
  auto NonDwordRow = makeConfig();
  configureTile(NonDwordRow, 1, 6, 2);
  configureTile(NonDwordRow, 7, 6, 2);
  Invalid.push_back(NonDwordRow);
  auto ExhaustedRestart = makeConfig(2);
  configureTile(ExhaustedRestart, 1, 8, 2);
  configureTile(ExhaustedRestart, 7, 8, 2);
  Invalid.push_back(ExhaustedRestart);

  const std::vector<uint8_t> OriginalTile(x86reg::TileRegStride, 0x69);
  for (const auto &Config : Invalid) {
    BinaryImage Image = makeImage({makeSegment(
        0x9000, 0x100,
        SegmentFlags::Readable | SegmentFlags::Writable)});
    std::fill(Image.Segments[0].Data.begin(), Image.Segments[0].Data.end(),
              0x42);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::TileConfig, Config);
    Emulator.setRegisterBytes(x86reg::TMM1, OriginalTile);
    Emulator.setRegisterBytes(x86reg::TMM7, OriginalTile);
    Emulator.setRegister(x86reg::RAX, 0x9000);
    Emulator.setRegister(x86reg::RBX, 8);
    Emulator.setRegister(x86reg::R14, 0x9000);
    Emulator.setRegister(x86reg::R15, 2);
    EXPECT_LT(Emulator.run(Load.Ops), Load.Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::TMM1), OriginalTile);
    EXPECT_LT(Emulator.run(Store.Ops), Store.Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::TMM7), OriginalTile);
    for (unsigned I = 0; I < 8; ++I)
      EXPECT_EQ(probeByte(Emulator, 0x9000 + I, 0x73000000 + I), 0x42);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86AMXState, TileLoadAndStoreHonorShapeStrideAndRestartProgress) {
  constexpr uint64_t LoadAddress = 0x5000;
  constexpr uint64_t StoreAddress = 0x6000;
  auto Config = makeConfig();
  configureTile(Config, 1, 8, 3);
  configureTile(Config, 7, 8, 3);
  BinaryImage Image = makeImage({
      makeSegment(LoadAddress, 0x100, SegmentFlags::Readable),
      makeSegment(StoreAddress, 0x100,
                  SegmentFlags::Readable | SegmentFlags::Writable),
  });
  for (unsigned Row = 0; Row < 3; ++Row) {
    std::array<uint8_t, 8> Data{};
    for (unsigned I = 0; I < Data.size(); ++I)
      Data[I] = static_cast<uint8_t>(0x10 * (Row + 1) + I);
    writeImageBytes(Image, LoadAddress + Row * 16, Data.data(), Data.size());
  }

  const LiftedInstruction Load =
      liftX64({0xc4, 0xe2, 0x7b, 0x4b, 0x0c, 0x18});
  const LiftedInstruction Store =
      liftX64({0xc4, 0x82, 0x7a, 0x4b, 0x3c, 0xbe});
  ASSERT_EQ(Load.Id, X86_INS_TILELOADD);
  ASSERT_EQ(Store.Id, X86_INS_TILESTORED);
  ASSERT_FALSE(Load.Ops.empty());
  ASSERT_FALSE(Store.Ops.empty());

  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegisterBytes(x86reg::TileConfig, Config);
  Emulator.setRegisterBytes(x86reg::TMM1,
                            std::vector<uint8_t>(x86reg::TileRegStride, 0xcc));
  Emulator.setRegister(x86reg::RAX, LoadAddress);
  Emulator.setRegister(x86reg::RBX, 16);
  ASSERT_EQ(Emulator.run(Load.Ops), Load.Ops.size());
  const auto Loaded = Emulator.getRegisterBytes(x86reg::TMM1);
  ASSERT_TRUE(Loaded);
  for (unsigned Row = 0; Row < 3; ++Row) {
    for (unsigned I = 0; I < 8; ++I)
      EXPECT_EQ((*Loaded)[Row * 64 + I], 0x10 * (Row + 1) + I);
    EXPECT_TRUE(std::all_of(Loaded->begin() + Row * 64 + 8,
                            Loaded->begin() + (Row + 1) * 64,
                            [](uint8_t Byte) { return Byte == 0; }));
  }
  EXPECT_TRUE(std::all_of(Loaded->begin() + 3 * 64, Loaded->end(),
                          [](uint8_t Byte) { return Byte == 0; }));

  std::vector<uint8_t> StoredTile(x86reg::TileRegStride, 0);
  for (unsigned Row = 0; Row < 3; ++Row)
    for (unsigned I = 0; I < 8; ++I)
      StoredTile[Row * 64 + I] = static_cast<uint8_t>(0x80 + Row * 8 + I);
  Emulator.setRegisterBytes(x86reg::TMM7, StoredTile);
  Emulator.setRegister(x86reg::R14, StoreAddress);
  Emulator.setRegister(x86reg::R15, 8); // encoded scale 4 => 32-byte stride
  ASSERT_EQ(Emulator.run(Store.Ops), Store.Ops.size());
  for (unsigned Row = 0; Row < 3; ++Row)
    for (unsigned I = 0; I < 8; ++I)
      EXPECT_EQ(probeByte(Emulator, StoreAddress + Row * 32 + I,
                          0x71000000 + Row * 16 + I),
                StoredTile[Row * 64 + I]);

  BinaryImage LoadFaultImage = makeImage(
      {makeSegment(0x7000, 8, SegmentFlags::Readable)});
  for (unsigned I = 0; I < 8; ++I)
    LoadFaultImage.Segments[0].Data[I] = static_cast<uint8_t>(0xe0 + I);
  NdOpEmulator LoadFault(LoadFaultImage);
  LoadFault.setStrictMode(true);
  LoadFault.setRegisterBytes(x86reg::TileConfig, Config);
  LoadFault.setRegisterBytes(x86reg::TMM1,
                             std::vector<uint8_t>(1024, 0x77));
  LoadFault.setRegister(x86reg::RAX, 0x7000);
  LoadFault.setRegister(x86reg::RBX, 8);
  EXPECT_LT(LoadFault.run(Load.Ops), Load.Ops.size());
  const auto PartialLoad = LoadFault.getRegisterBytes(x86reg::TMM1);
  const auto LoadFaultConfig =
      LoadFault.getRegisterBytes(x86reg::TileConfig);
  ASSERT_TRUE(PartialLoad);
  ASSERT_TRUE(LoadFaultConfig);
  EXPECT_EQ((*LoadFaultConfig)[1], 1);
  for (unsigned I = 0; I < 8; ++I)
    EXPECT_EQ((*PartialLoad)[I], 0xe0 + I);
  EXPECT_TRUE(std::all_of(PartialLoad->begin() + 8, PartialLoad->end(),
                          [](uint8_t Byte) { return Byte == 0; }));

  BinaryImage StoreFaultImage = makeImage({makeSegment(
      0x8000, 8, SegmentFlags::Readable | SegmentFlags::Writable)});
  NdOpEmulator StoreFault(StoreFaultImage);
  StoreFault.setStrictMode(true);
  StoreFault.setRegisterBytes(x86reg::TileConfig, Config);
  StoreFault.setRegisterBytes(x86reg::TMM7, StoredTile);
  StoreFault.setRegister(x86reg::R14, 0x8000);
  StoreFault.setRegister(x86reg::R15, 2); // encoded scale 4 => 8-byte stride
  EXPECT_LT(StoreFault.run(Store.Ops), Store.Ops.size());
  const auto StoreFaultConfig =
      StoreFault.getRegisterBytes(x86reg::TileConfig);
  ASSERT_TRUE(StoreFaultConfig);
  EXPECT_EQ((*StoreFaultConfig)[1], 1);
  for (unsigned I = 0; I < 8; ++I)
    EXPECT_EQ(probeByte(StoreFault, 0x8000 + I, 0x72000000 + I),
              StoredTile[I]);
  EXPECT_FALSE(Emulator.skips().any());
  EXPECT_FALSE(LoadFault.skips().any());
  EXPECT_FALSE(StoreFault.skips().any());
}

TEST(X86AMXState, ReadSharedTileLoadsPreserveExactTileLoadValueSemantics) {
  const std::array<std::pair<std::vector<uint8_t>, unsigned>, 2> Cases = {{
      {{0xc4, 0xc2, 0x7b, 0x4a, 0x24, 0xe4}, X86_INS_TILELOADDRS},
      {{0xc4, 0xc2, 0x79, 0x4a, 0x24, 0xe4}, X86_INS_TILELOADDRST1},
  }};
  auto Config = makeConfig();
  configureTile(Config, 4, 8, 2);
  BinaryImage Image =
      makeImage({makeSegment(0x9000, 8, SegmentFlags::Readable)});
  for (unsigned I = 0; I < 8; ++I)
    Image.Segments[0].Data[I] = static_cast<uint8_t>(0x40 + I);

  for (const auto &[Bytes, Id] : Cases) {
    const LiftedInstruction Load = liftX64(Bytes);
    ASSERT_EQ(Load.Id, Id);
    ASSERT_FALSE(Load.Ops.empty());
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::TileConfig, Config);
    Emulator.setRegisterBytes(x86reg::TMM4,
                              std::vector<uint8_t>(x86reg::TileRegStride,
                                                   0xa5));
    Emulator.setRegister(x86reg::R12, 0x9000);
    ASSERT_EQ(Emulator.run(Load.Ops), Load.Ops.size());
    const auto Result = Emulator.getRegisterBytes(x86reg::TMM4);
    const auto ResultConfig =
        Emulator.getRegisterBytes(x86reg::TileConfig);
    ASSERT_TRUE(Result);
    ASSERT_TRUE(ResultConfig);
    for (unsigned Row = 0; Row < 2; ++Row) {
      for (unsigned I = 0; I < 8; ++I)
        EXPECT_EQ((*Result)[Row * 64 + I], 0x40 + I);
      EXPECT_TRUE(std::all_of(Result->begin() + Row * 64 + 8,
                              Result->begin() + (Row + 1) * 64,
                              [](uint8_t Byte) { return Byte == 0; }));
    }
    EXPECT_TRUE(std::all_of(Result->begin() + 2 * 64, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
    EXPECT_EQ((*ResultConfig)[1], 0);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86AMXState, RowOperationsConvertRoundAndZeroExactly) {
  struct RowCase {
    std::vector<uint8_t> Bytes;
    unsigned Id;
    unsigned Tile;
    unsigned Destination;
    std::array<uint32_t, 2> Source;
    std::array<uint32_t, 2> Expected;
  };
  const std::array<RowCase, 6> Cases = {{
      {{{0x62, 0xe2, 0x46, 0x40, 0x4a, 0xce}},
       X86_INS_TCVTROWD2PS,
       6,
       17,
       {0x01000001U, 0xfeffffffU},
       {0x4b800000U, 0xcb800000U}},
      {{{0x62, 0x63, 0x7f, 0x48, 0x07, 0xf8, 0x01}},
       X86_INS_TCVTROWPS2BF16H,
       0,
       31,
       {0x3f818000U, 0x3f808000U},
       {0x3f820000U, 0x3f800000U}},
      {{{0x62, 0xe2, 0x46, 0x40, 0x6d, 0xce}},
       X86_INS_TCVTROWPS2BF16L,
       6,
       17,
       {0x3f818000U, 0x3f808000U},
       {0x00003f82U, 0x00003f80U}},
      {{{0x62, 0x63, 0x7c, 0x48, 0x07, 0xf8, 0x01}},
       X86_INS_TCVTROWPS2PHH,
       0,
       31,
       {0x3f803000U, 0x00000001U},
       {0x3c020000U, 0x00000000U}},
      {{{0x62, 0xe2, 0x45, 0x40, 0x6d, 0xce}},
       X86_INS_TCVTROWPS2PHL,
       6,
       17,
       {0x3f801000U, 0x3f803000U},
       {0x00003c00U, 0x00003c02U}},
      {{{0x62, 0x63, 0x7d, 0x48, 0x07, 0xf8, 0x01}},
       X86_INS_TILEMOVROW,
       0,
       31,
       {0xdeadbeefU, 0x01234567U},
       {0xdeadbeefU, 0x01234567U}},
  }};

  auto Config = makeConfig(1);
  configureTile(Config, 0, 8, 2);
  configureTile(Config, 6, 8, 2);
  BinaryImage Image = makeImage({});
  for (const RowCase &Case : Cases) {
    const LiftedInstruction Row = liftX64(Case.Bytes);
    ASSERT_EQ(Row.Id, Case.Id);
    ASSERT_FALSE(Row.Ops.empty());
    std::vector<uint8_t> Tile(x86reg::TileRegStride, 0xa5);
    writeDword(Tile, 1, 0, Case.Source[0]);
    writeDword(Tile, 1, 1, Case.Source[1]);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::TileConfig, Config);
    Emulator.setRegisterBytes(x86reg::tileReg(Case.Tile), Tile);
    Emulator.setRegisterBytes(x86reg::vectorReg(Case.Destination),
                              std::vector<uint8_t>(64, 0x5a));
    Emulator.setRegister(x86reg::R23, 1);
    ASSERT_EQ(Emulator.run(Row.Ops), Row.Ops.size());
    const auto Result =
        Emulator.getRegisterBytes(x86reg::vectorReg(Case.Destination));
    const auto ResultConfig =
        Emulator.getRegisterBytes(x86reg::TileConfig);
    ASSERT_TRUE(Result);
    ASSERT_TRUE(ResultConfig);
    EXPECT_EQ(readDword(*Result, 0, 0), Case.Expected[0]);
    EXPECT_EQ(readDword(*Result, 0, 1), Case.Expected[1]);
    EXPECT_TRUE(std::all_of(Result->begin() + 8, Result->end(),
                            [](uint8_t Byte) { return Byte == 0; }));
    EXPECT_EQ((*ResultConfig)[1], 0);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86AMXState, RowOperationsRejectInvalidStateBeforeAnyEffect) {
  const std::array<std::vector<uint8_t>, 2> Cases = {{
      {0x62, 0x63, 0x7d, 0x48, 0x07, 0xf8, 0xa5},
      {0x62, 0xe2, 0x46, 0x40, 0x4a, 0xce},
  }};
  auto Config = makeConfig(1);
  configureTile(Config, 0, 8, 2);
  configureTile(Config, 6, 8, 2);
  const std::vector<uint8_t> OriginalDestination(64, 0x6d);
  BinaryImage Image = makeImage({});
  for (const std::vector<uint8_t> &Bytes : Cases) {
    const LiftedInstruction Row = liftX64(Bytes);
    ASSERT_FALSE(Row.Ops.empty());
    const unsigned Destination = Row.Id == X86_INS_TILEMOVROW ? 31 : 17;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::TileConfig, Config);
    Emulator.setRegisterBytes(x86reg::TMM0,
                              std::vector<uint8_t>(x86reg::TileRegStride));
    Emulator.setRegisterBytes(x86reg::TMM6,
                              std::vector<uint8_t>(x86reg::TileRegStride));
    Emulator.setRegisterBytes(x86reg::vectorReg(Destination),
                              OriginalDestination);
    Emulator.setRegister(x86reg::R23, UINT64_C(1) << 16);
    EXPECT_LT(Emulator.run(Row.Ops), Row.Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(Destination)),
              OriginalDestination);
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::TileConfig),
              std::vector<uint8_t>(Config.begin(), Config.end()));
    EXPECT_FALSE(Emulator.skips().any());
  }

  auto InvalidShape = makeConfig(1);
  configureTile(InvalidShape, 0, 6, 2);
  const LiftedInstruction Move =
      liftX64({0x62, 0x63, 0x7d, 0x48, 0x07, 0xf8, 0x01});
  ASSERT_FALSE(Move.Ops.empty());
  NdOpEmulator InvalidShapeEmulator(Image);
  InvalidShapeEmulator.setStrictMode(true);
  InvalidShapeEmulator.setRegisterBytes(x86reg::TileConfig, InvalidShape);
  InvalidShapeEmulator.setRegisterBytes(
      x86reg::TMM0, std::vector<uint8_t>(x86reg::TileRegStride));
  InvalidShapeEmulator.setRegisterBytes(x86reg::XMM31,
                                        OriginalDestination);
  EXPECT_LT(InvalidShapeEmulator.run(Move.Ops), Move.Ops.size());
  EXPECT_EQ(InvalidShapeEmulator.getRegisterBytes(x86reg::XMM31),
            OriginalDestination);
  EXPECT_EQ(InvalidShapeEmulator.getRegisterBytes(x86reg::TileConfig),
            std::vector<uint8_t>(InvalidShape.begin(), InvalidShape.end()));
  EXPECT_FALSE(InvalidShapeEmulator.skips().any());
}

} // namespace
