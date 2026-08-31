//===- X86_64_EVEXVSIBTests.cpp - EVEX VSIB memory semantics -----------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;

enum class VSIBFamily { DD, DQ, QD, QQ };

struct VSIBCase {
  const char *Name;
  std::vector<uint8_t> Bytes;
  VSIBFamily Family;
  bool Scatter;
  uint16_t DataBytes;
  uint16_t IndexBytes;
  x86_reg ValueReg;
  x86_reg IndexReg;
  x86_reg MaskReg;
  x86_reg BaseReg;
  uint64_t BaseValue;
  int64_t Displacement;
  unsigned Scale;
};

std::vector<LowOp> liftX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  EXPECT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Bytes.size()));
  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

uint16_t indexElementBytes(VSIBFamily Family) {
  return Family == VSIBFamily::DD || Family == VSIBFamily::DQ ? 4 : 8;
}

uint16_t valueElementBytes(VSIBFamily Family) {
  return Family == VSIBFamily::DD || Family == VSIBFamily::QD ? 4 : 8;
}

int64_t laneIndex(size_t Lane) {
  return Lane == 0 ? -4 : static_cast<int64_t>(Lane * 4);
}

uint64_t laneAddress(const VSIBCase &Test, size_t Lane) {
  const int64_t Relative =
      Test.Displacement + laneIndex(Lane) * static_cast<int64_t>(Test.Scale);
  return Test.BaseValue + static_cast<uint64_t>(Relative);
}

void setLane(std::vector<uint8_t> &Bytes, size_t Lane, size_t LaneBytes,
             uint64_t Value) {
  std::memcpy(Bytes.data() + Lane * LaneBytes, &Value, LaneBytes);
}

uint64_t getLane(const std::vector<uint8_t> &Bytes, size_t Lane,
                 size_t LaneBytes) {
  uint64_t Value = 0;
  std::memcpy(&Value, Bytes.data() + Lane * LaneBytes, LaneBytes);
  return Value;
}

BinaryImage makeMemoryImage(uint64_t VA, size_t Size) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Data;
  Data.VA = VA;
  Data.Size = Size;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Size);
  Image.Segments.push_back(std::move(Data));
  return Image;
}

void writeImageValue(BinaryImage &Image, uint64_t Address, uint16_t Size,
                     uint64_t Value) {
  ASSERT_EQ(Image.Segments.size(), 1u);
  Segment &Data = Image.Segments.front();
  ASSERT_GE(Address, Data.VA);
  const uint64_t Offset = Address - Data.VA;
  ASSERT_LE(Offset + Size, Data.Data.size());
  std::memcpy(Data.Data.data() + Offset, &Value, Size);
}

uint64_t probeMemory(NdOpEmulator &Emulator, uint64_t Address, uint16_t Size,
                     uint64_t Temp) {
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::tmp(Temp, Size);
  Load.addInput(NdVar::cst(0, 8));
  Load.addInput(NdVar::cst(Address, 8));
  EXPECT_TRUE(Emulator.step(Load));
  return Emulator.getRegister(Temp).value_or(UINT64_MAX);
}

void expectMalformedShapeRejected(const std::vector<uint8_t> &Bytes,
                                  const std::function<void(cs_x86 &)> &Mutate) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  Mutate(Insn.Raw->detail->x86);
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

std::vector<VSIBCase> allLegalWidthCases() {
  constexpr uint64_t Base = 0x4000;
  return {
      {"gatherdd-x",
       {0x62, 0xf2, 0x7d, 0x09, 0x90, 0x4c, 0x90, 0x08},
       VSIBFamily::DD,
       false,
       16,
       16,
       X86_REG_XMM1,
       X86_REG_XMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"gatherdd-y",
       {0x62, 0xf2, 0x7d, 0x29, 0x90, 0x4c, 0x90, 0x08},
       VSIBFamily::DD,
       false,
       32,
       32,
       X86_REG_YMM1,
       X86_REG_YMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"gatherdd-z",
       {0x62, 0xf2, 0x7d, 0x49, 0x90, 0x4c, 0x90, 0x08},
       VSIBFamily::DD,
       false,
       64,
       64,
       X86_REG_ZMM1,
       X86_REG_ZMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"gatherdq-x",
       {0x62, 0xf2, 0xfd, 0x09, 0x90, 0x4c, 0x90, 0x04},
       VSIBFamily::DQ,
       false,
       16,
       16,
       X86_REG_XMM1,
       X86_REG_XMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"gatherdq-y",
       {0x62, 0xf2, 0xfd, 0x29, 0x90, 0x4c, 0x90, 0x04},
       VSIBFamily::DQ,
       false,
       32,
       16,
       X86_REG_YMM1,
       X86_REG_XMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"gatherdq-z",
       {0x62, 0xf2, 0xfd, 0x49, 0x90, 0x4c, 0x90, 0x04},
       VSIBFamily::DQ,
       false,
       64,
       32,
       X86_REG_ZMM1,
       X86_REG_YMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"gatherqd-x",
       {0x62, 0xf2, 0x7d, 0x09, 0x91, 0x4c, 0x90, 0x08},
       VSIBFamily::QD,
       false,
       8,
       16,
       X86_REG_XMM1,
       X86_REG_XMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"gatherqd-y",
       {0x62, 0xf2, 0x7d, 0x29, 0x91, 0x4c, 0x90, 0x08},
       VSIBFamily::QD,
       false,
       16,
       32,
       X86_REG_XMM1,
       X86_REG_YMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"gatherqd-z",
       {0x62, 0xf2, 0x7d, 0x49, 0x91, 0x4c, 0x90, 0x08},
       VSIBFamily::QD,
       false,
       32,
       64,
       X86_REG_YMM1,
       X86_REG_ZMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"gatherqq-x",
       {0x62, 0xf2, 0xfd, 0x09, 0x91, 0x4c, 0x90, 0x04},
       VSIBFamily::QQ,
       false,
       16,
       16,
       X86_REG_XMM1,
       X86_REG_XMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"gatherqq-y",
       {0x62, 0xf2, 0xfd, 0x29, 0x91, 0x4c, 0x90, 0x04},
       VSIBFamily::QQ,
       false,
       32,
       32,
       X86_REG_YMM1,
       X86_REG_YMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"gatherqq-z",
       {0x62, 0xf2, 0xfd, 0x49, 0x91, 0x4c, 0x90, 0x04},
       VSIBFamily::QQ,
       false,
       64,
       64,
       X86_REG_ZMM1,
       X86_REG_ZMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"scatterdd-x",
       {0x62, 0xf2, 0x7d, 0x09, 0xa0, 0x4c, 0x90, 0x08},
       VSIBFamily::DD,
       true,
       16,
       16,
       X86_REG_XMM1,
       X86_REG_XMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"scatterdd-y",
       {0x62, 0xf2, 0x7d, 0x29, 0xa0, 0x4c, 0x90, 0x08},
       VSIBFamily::DD,
       true,
       32,
       32,
       X86_REG_YMM1,
       X86_REG_YMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"scatterdd-z",
       {0x62, 0xf2, 0x7d, 0x49, 0xa0, 0x4c, 0x90, 0x08},
       VSIBFamily::DD,
       true,
       64,
       64,
       X86_REG_ZMM1,
       X86_REG_ZMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"scatterdq-x",
       {0x62, 0xf2, 0xfd, 0x09, 0xa0, 0x4c, 0x90, 0x04},
       VSIBFamily::DQ,
       true,
       16,
       16,
       X86_REG_XMM1,
       X86_REG_XMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"scatterdq-y",
       {0x62, 0xf2, 0xfd, 0x29, 0xa0, 0x4c, 0x90, 0x04},
       VSIBFamily::DQ,
       true,
       32,
       16,
       X86_REG_YMM1,
       X86_REG_XMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"scatterdq-z",
       {0x62, 0xf2, 0xfd, 0x49, 0xa0, 0x4c, 0x90, 0x04},
       VSIBFamily::DQ,
       true,
       64,
       32,
       X86_REG_ZMM1,
       X86_REG_YMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"scatterqd-x",
       {0x62, 0xf2, 0x7d, 0x09, 0xa1, 0x4c, 0x90, 0x08},
       VSIBFamily::QD,
       true,
       8,
       16,
       X86_REG_XMM1,
       X86_REG_XMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"scatterqd-y",
       {0x62, 0xf2, 0x7d, 0x29, 0xa1, 0x4c, 0x90, 0x08},
       VSIBFamily::QD,
       true,
       16,
       32,
       X86_REG_XMM1,
       X86_REG_YMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"scatterqd-z",
       {0x62, 0xf2, 0x7d, 0x49, 0xa1, 0x4c, 0x90, 0x08},
       VSIBFamily::QD,
       true,
       32,
       64,
       X86_REG_YMM1,
       X86_REG_ZMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"scatterqq-x",
       {0x62, 0xf2, 0xfd, 0x09, 0xa1, 0x4c, 0x90, 0x04},
       VSIBFamily::QQ,
       true,
       16,
       16,
       X86_REG_XMM1,
       X86_REG_XMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"scatterqq-y",
       {0x62, 0xf2, 0xfd, 0x29, 0xa1, 0x4c, 0x90, 0x04},
       VSIBFamily::QQ,
       true,
       32,
       32,
       X86_REG_YMM1,
       X86_REG_YMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"scatterqq-z",
       {0x62, 0xf2, 0xfd, 0x49, 0xa1, 0x4c, 0x90, 0x04},
       VSIBFamily::QQ,
       true,
       64,
       64,
       X86_REG_ZMM1,
       X86_REG_ZMM2,
       X86_REG_K1,
       X86_REG_RAX,
       Base,
       32,
       4},
      {"gatherdd-high",
       {0x62, 0x42, 0x7d, 0x47, 0x90, 0x7c, 0xe5, 0xf0},
       VSIBFamily::DD,
       false,
       64,
       64,
       X86_REG_ZMM31,
       X86_REG_ZMM20,
       X86_REG_K7,
       X86_REG_R13,
       Base,
       -64,
       8},
      {"scatterqq-high",
       {0x62, 0x42, 0xfd, 0x47, 0xa1, 0x7c, 0xe5, 0xf8},
       VSIBFamily::QQ,
       true,
       64,
       64,
       X86_REG_ZMM31,
       X86_REG_ZMM20,
       X86_REG_K7,
       X86_REG_R13,
       Base,
       -64,
       8},
  };
}

TEST(X86EVEXVSIB, ExactKMaskAddressingSuppressionAndFaultProgress) {
  uint64_t ProbeTemp = UINT64_C(0x70000000);
  for (const VSIBCase &Test : allLegalWidthCases()) {
    SCOPED_TRACE(Test.Name);
    const uint16_t IndexElement = indexElementBytes(Test.Family);
    const uint16_t ValueElement = valueElementBytes(Test.Family);
    const size_t Lanes = Test.DataBytes / ValueElement;
    ASSERT_GE(Test.IndexBytes, Lanes * IndexElement);

    BinaryImage Image = makeMemoryImage(0x2000, 0x4000);
    std::vector<uint8_t> Indices(Test.IndexBytes);
    std::vector<uint8_t> InitialDestination(64, 0xa5);
    std::vector<uint8_t> Source(64, 0x5a);
    uint64_t ActiveBits = 0;
    std::vector<uint64_t> InitialMemory(Lanes);
    std::vector<uint64_t> SourceValues(Lanes);
    for (size_t Lane = 0; Lane < Lanes; ++Lane) {
      setLane(Indices, Lane, IndexElement,
              static_cast<uint64_t>(laneIndex(Lane)));
      const uint64_t Address = laneAddress(Test, Lane);
      InitialMemory[Lane] = UINT64_C(0x3132333400000000) + Lane;
      SourceValues[Lane] = UINT64_C(0x7172737400000000) + Lane;
      writeImageValue(Image, Address, ValueElement, InitialMemory[Lane]);
      setLane(InitialDestination, Lane, ValueElement,
              UINT64_C(0xb1b2b3b400000000) + Lane);
      setLane(Source, Lane, ValueElement, SourceValues[Lane]);
      if ((Lane & 1) == 0)
        ActiveBits |= UINT64_C(1) << Lane;
    }

    const std::vector<LowOp> Ops = liftX64(Test.Bytes);
    ASSERT_FALSE(Ops.empty());
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(mapCapstoneReg(Test.BaseReg).Offset, Test.BaseValue);
    Emulator.setRegisterBytes(mapCapstoneReg(Test.IndexReg).Offset, Indices);
    Emulator.setRegister(mapCapstoneReg(Test.MaskReg).Offset,
                         UINT64_C(0xfedcba9876540000) | ActiveBits);
    Emulator.setRegisterBytes(mapCapstoneReg(Test.ValueReg).Offset,
                              Test.Scatter ? Source : InitialDestination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(mapCapstoneReg(Test.MaskReg).Offset), 0u);

    if (!Test.Scatter) {
      const auto Result =
          Emulator.getRegisterBytes(mapCapstoneReg(Test.ValueReg).Offset);
      ASSERT_TRUE(Result);
      ASSERT_EQ(Result->size(), 64u);
      for (size_t Lane = 0; Lane < Lanes; ++Lane) {
        const uint64_t Expected =
            (ActiveBits & (UINT64_C(1) << Lane))
                ? InitialMemory[Lane]
                : getLane(InitialDestination, Lane, ValueElement);
        EXPECT_EQ(getLane(*Result, Lane, ValueElement),
                  Expected & (ValueElement == 4 ? UINT32_MAX : UINT64_MAX));
      }
      EXPECT_TRUE(std::all_of(Result->begin() + Test.DataBytes, Result->end(),
                              [](uint8_t Byte) { return Byte == 0; }));
      EXPECT_EQ(Emulator.getLoadRecords().size(),
                static_cast<size_t>((Lanes + 1) / 2));
    } else {
      for (size_t Lane = 0; Lane < Lanes; ++Lane) {
        const uint64_t Address = laneAddress(Test, Lane);
        const uint64_t Expected = (ActiveBits & (UINT64_C(1) << Lane))
                                      ? SourceValues[Lane]
                                      : InitialMemory[Lane];
        EXPECT_EQ(probeMemory(Emulator, Address, ValueElement, ProbeTemp++),
                  Expected & (ValueElement == 4 ? UINT32_MAX : UINT64_MAX));
      }
    }
  }

  // GS + addr32: EAX + disp wraps to zero. Lane zero succeeds, lane one faults;
  // its destination and mask bit, every later lane, and the high K state stay
  // uncommitted. This also proves that masked-off lanes do not generate loads.
  const std::vector<uint8_t> GatherBytes{0x65, 0x67, 0x62, 0xf2, 0x7d,
                                         0x49, 0x90, 0x4c, 0x90, 0x08};
  const std::vector<LowOp> GatherOps = liftX64(GatherBytes);
  BinaryImage GatherImage = makeMemoryImage(0x8000, 4);
  writeImageValue(GatherImage, 0x8000, 4, UINT32_C(0xdec0ad01));
  std::vector<uint8_t> GatherIndices(64);
  setLane(GatherIndices, 0, 4, 0);
  setLane(GatherIndices, 1, 4, 4);
  std::vector<uint8_t> GatherOld(64);
  for (size_t Lane = 0; Lane < 16; ++Lane)
    setLane(GatherOld, Lane, 4, UINT32_C(0xa0000000) + Lane);
  const uint64_t PartialMask = UINT64_C(0x1234000000000003);
  NdOpEmulator PartialGather(GatherImage);
  PartialGather.setStrictMode(true);
  PartialGather.setLoadCollect(true);
  ASSERT_TRUE(PartialGather.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86GS, 0x8000));
  PartialGather.setRegister(x86reg::RAX, UINT64_C(0xffffffe0));
  PartialGather.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM2).Offset,
                                 GatherIndices);
  PartialGather.setRegister(mapCapstoneReg(X86_REG_K1).Offset, PartialMask);
  PartialGather.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset,
                                 GatherOld);
  EXPECT_LT(PartialGather.run(GatherOps), GatherOps.size());
  const auto PartialGatherResult =
      PartialGather.getRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset);
  ASSERT_TRUE(PartialGatherResult);
  ASSERT_EQ(PartialGatherResult->size(), 64u);
  EXPECT_EQ(getLane(*PartialGatherResult, 0, 4), UINT32_C(0xdec0ad01));
  for (size_t Lane = 1; Lane < 16; ++Lane)
    EXPECT_EQ(getLane(*PartialGatherResult, Lane, 4),
              UINT32_C(0xa0000000) + Lane);
  EXPECT_EQ(PartialGather.getRegister(mapCapstoneReg(X86_REG_K1).Offset),
            PartialMask & ~UINT64_C(1));
  ASSERT_EQ(PartialGather.getLoadRecords().size(), 1u);
  EXPECT_EQ(PartialGather.getLoadRecords().front().Addr, 0x8000u);

  // FS + addr32 uses the same wrapped offset for stores. The first lane is
  // committed before the second faults; its K bit clears while all later bits
  // and high K state remain restartable.
  const std::vector<uint8_t> ScatterBytes{0x64, 0x67, 0x62, 0xf2, 0x7d,
                                          0x49, 0xa0, 0x4c, 0x90, 0x08};
  const std::vector<LowOp> ScatterOps = liftX64(ScatterBytes);
  BinaryImage ScatterImage = makeMemoryImage(0x9000, 4);
  std::vector<uint8_t> ScatterSource(64);
  setLane(ScatterSource, 0, 4, UINT32_C(0xc001d00d));
  setLane(ScatterSource, 1, 4, UINT32_C(0xbad0c0de));
  NdOpEmulator PartialScatter(ScatterImage);
  PartialScatter.setStrictMode(true);
  ASSERT_TRUE(PartialScatter.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86FS, 0x9000));
  PartialScatter.setRegister(x86reg::RAX, UINT64_C(0xffffffe0));
  PartialScatter.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM2).Offset,
                                  GatherIndices);
  PartialScatter.setRegister(mapCapstoneReg(X86_REG_K1).Offset, PartialMask);
  PartialScatter.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset,
                                  ScatterSource);
  EXPECT_LT(PartialScatter.run(ScatterOps), ScatterOps.size());
  EXPECT_EQ(PartialScatter.getRegister(mapCapstoneReg(X86_REG_K1).Offset),
            PartialMask & ~UINT64_C(1));
  LowOp FSProbe;
  FSProbe.Opcode = NdOp::LOAD;
  FSProbe.Output = NdVar::tmp(ProbeTemp++, 4);
  FSProbe.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  FSProbe.addInput(NdVar::cst(0, 8));
  ASSERT_TRUE(PartialScatter.step(FSProbe));
  EXPECT_EQ(PartialScatter.getRegister(FSProbe.Output.Offset),
            UINT32_C(0xc001d00d));

  // With every K bit clear, even wholly unmapped VSIB addresses are suppressed.
  BinaryImage EmptyImage;
  EmptyImage.Arch = Arch::X64;
  EmptyImage.Bits = Bitness::Bits64;
  NdOpEmulator Suppressed(EmptyImage);
  Suppressed.setStrictMode(true);
  Suppressed.setLoadCollect(true);
  ASSERT_TRUE(Suppressed.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS,
                                                   UINT64_C(0x100000000)));
  Suppressed.setRegister(x86reg::RAX, UINT64_C(0xffffffe0));
  Suppressed.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM2).Offset,
                              GatherIndices);
  Suppressed.setRegister(mapCapstoneReg(X86_REG_K1).Offset, 0);
  Suppressed.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset, GatherOld);
  EXPECT_EQ(Suppressed.run(GatherOps), GatherOps.size());
  EXPECT_TRUE(Suppressed.getLoadRecords().empty());

  NdOpEmulator SuppressedScatter(EmptyImage);
  SuppressedScatter.setStrictMode(true);
  ASSERT_TRUE(SuppressedScatter.setMemoryAddressSpaceBase(
      NdMemoryAddressSpace::X86FS, UINT64_C(0x200000000)));
  SuppressedScatter.setRegister(x86reg::RAX, UINT64_C(0xffffffe0));
  SuppressedScatter.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM2).Offset,
                                     GatherIndices);
  SuppressedScatter.setRegister(mapCapstoneReg(X86_REG_K1).Offset, 0);
  SuppressedScatter.setRegisterBytes(mapCapstoneReg(X86_REG_ZMM1).Offset,
                                     ScatterSource);
  EXPECT_EQ(SuppressedScatter.run(ScatterOps), ScatterOps.size());

  // A qword whose first byte is mapped but whose tail crosses the segment
  // boundary faults as one lane: no byte is stored and the current K bit is
  // left set for restart.
  const std::vector<uint8_t> ScatterQQBytes{0x62, 0xf2, 0xfd, 0x09,
                                            0xa1, 0x4c, 0x90, 0x04};
  const std::vector<LowOp> ScatterQQOps = liftX64(ScatterQQBytes);
  BinaryImage SplitStoreImage = makeMemoryImage(0xa020, 4);
  writeImageValue(SplitStoreImage, 0xa020, 4, UINT32_C(0x11223344));
  std::vector<uint8_t> SplitIndices(16);
  setLane(SplitIndices, 0, 8, 0);
  std::vector<uint8_t> SplitSource(64);
  setLane(SplitSource, 0, 8, UINT64_C(0x8877665544332211));
  const uint64_t SplitMask = UINT64_C(0x55aa000000000001);
  NdOpEmulator SplitStore(SplitStoreImage);
  SplitStore.setStrictMode(true);
  SplitStore.setRegister(x86reg::RAX, 0xa000);
  SplitStore.setRegisterBytes(mapCapstoneReg(X86_REG_XMM2).Offset,
                              SplitIndices);
  SplitStore.setRegister(mapCapstoneReg(X86_REG_K1).Offset, SplitMask);
  SplitStore.setRegisterBytes(mapCapstoneReg(X86_REG_XMM1).Offset, SplitSource);
  EXPECT_LT(SplitStore.run(ScatterQQOps), ScatterQQOps.size());
  EXPECT_EQ(SplitStore.getRegister(mapCapstoneReg(X86_REG_K1).Offset),
            SplitMask);
  EXPECT_EQ(probeMemory(SplitStore, 0xa020, 4, ProbeTemp++),
            UINT32_C(0x11223344));

  // Decoder details are an input boundary too: no malformed K0, zeroing,
  // non-VSIB, or width-divergent shape may emit even preparatory address ops.
  const std::vector<uint8_t> ValidGather{0x62, 0xf2, 0x7d, 0x49,
                                         0x90, 0x4c, 0x90, 0x08};
  expectMalformedShapeRejected(
      ValidGather, [](cs_x86 &X86) { X86.operands[1].reg = X86_REG_K0; });
  expectMalformedShapeRejected(
      ValidGather, [](cs_x86 &X86) { X86.operands[0].avx_zero_opmask = true; });
  expectMalformedShapeRejected(ValidGather, [](cs_x86 &X86) {
    X86.modrm = static_cast<uint8_t>((X86.modrm & ~7u) | 1u);
  });
  expectMalformedShapeRejected(ValidGather, [](cs_x86 &X86) {
    X86.operands[2].mem.index = X86_REG_XMM2;
    X86.sib_index = X86_REG_XMM2;
  });

  const std::vector<uint8_t> ValidScatter{0x62, 0xf2, 0xfd, 0x49,
                                          0xa1, 0x4c, 0x90, 0x04};
  expectMalformedShapeRejected(ValidScatter,
                               [](cs_x86 &X86) { X86.operands[0].size = 8; });
}

} // namespace
