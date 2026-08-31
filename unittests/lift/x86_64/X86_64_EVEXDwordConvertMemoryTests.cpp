//===- X86_64_EVEXDwordConvertMemoryTests.cpp - EVEX convert memory -----===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;

struct ConvertCase {
  const char *Name;
  unsigned Id;
  uint8_t P1;
  uint8_t Opcode;
  bool IntegerToFloat;
  bool Unsigned;
};

constexpr std::array<ConvertCase, 4> kExactCases = {{
    {"signed-int-to-float", X86_INS_VCVTDQ2PS, 0x7c, 0x5b, true, false},
    {"unsigned-int-to-float", X86_INS_VCVTUDQ2PS, 0x7f, 0x7a, true, true},
    {"float-to-signed-truncate", X86_INS_VCVTTPS2DQ, 0x7e, 0x5b, false, false},
    {"float-to-unsigned-truncate", X86_INS_VCVTTPS2UDQ, 0x7c, 0x78, false,
     true},
}};

std::vector<LowOp> liftX64(const std::vector<uint8_t> &Bytes,
                           unsigned ExpectedId) {
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
  if (!Insn.Raw || Insn.Raw->id != ExpectedId) {
    ADD_FAILURE() << "unexpected instruction id";
    return {};
  }
  std::vector<LowOp> Ops;
  try {
    Dec.liftToLow(Insn, Ops);
  } catch (const UnliftedInstruction &) {
    ADD_FAILURE() << "instruction was not lifted";
    return {};
  }
  return Ops;
}

void expectStrictlyUnlifted(const std::vector<uint8_t> &Bytes,
                            unsigned ExpectedId) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_EQ(Insn.Raw->id, ExpectedId);
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

template <typename Mutator>
void expectMutatedLiftFailsClosed(const std::vector<uint8_t> &Bytes,
                                  unsigned ExpectedId, Mutator Mutate) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(),
                                 kInstructionAddress, Insn),
            static_cast<int>(Bytes.size()));
  ASSERT_NE(Insn.Raw, nullptr);
  ASSERT_EQ(Insn.Raw->id, ExpectedId);
  ASSERT_NE(Insn.Raw->detail, nullptr);
  ASSERT_TRUE(Mutate(*Insn.Raw, Insn.Raw->detail->x86));
  std::vector<LowOp> Ops;
  EXPECT_THROW(Dec.liftToLow(Insn, Ops), UnliftedInstruction);
  EXPECT_TRUE(Ops.empty());
}

BinaryImage emptyImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  return Image;
}

void addDword(BinaryImage &Image, uint64_t Address, uint32_t Value) {
  Segment Memory;
  Memory.VA = Address;
  Memory.Size = sizeof(Value);
  Memory.Flags = SegmentFlags::Readable;
  Memory.Data.resize(sizeof(Value));
  std::memcpy(Memory.Data.data(), &Value, sizeof(Value));
  Image.Segments.push_back(std::move(Memory));
}

uint32_t sourceBits(const ConvertCase &Case) {
  if (Case.IntegerToFloat)
    return Case.Unsigned ? UINT32_C(17) : std::bit_cast<uint32_t>(int32_t(-17));
  return std::bit_cast<uint32_t>(Case.Unsigned ? 17.75f : -17.75f);
}

uint32_t convertedBits(const ConvertCase &Case) {
  if (Case.IntegerToFloat) {
    const float Value = Case.Unsigned ? 17.0f : -17.0f;
    return std::bit_cast<uint32_t>(Value);
  }
  return Case.Unsigned ? UINT32_C(17) : std::bit_cast<uint32_t>(int32_t(-17));
}

void setDword(std::vector<uint8_t> &Bytes, unsigned Lane, uint32_t Value) {
  ASSERT_LE((Lane + 1) * sizeof(Value), Bytes.size());
  std::memcpy(Bytes.data() + Lane * sizeof(Value), &Value, sizeof(Value));
}

TEST(X86EVEXDwordConvertMemory,
     FullTupleSparseMaskLoadsOnlyTheActiveSourceLaneAndMerges) {
  constexpr uint64_t Base = UINT64_C(0x4000);
  constexpr unsigned ActiveLane = 11;
  constexpr uint64_t Mask = UINT64_C(1) << ActiveLane;

  for (const ConvertCase &Case : kExactCases) {
    SCOPED_TRACE(Case.Name);
    // vcvt* zmm0 {k1}, zmmword ptr [rax]
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf1, Case.P1, 0x49, Case.Opcode, 0x00}, Case.Id);
    ASSERT_FALSE(Ops.empty());

    std::vector<uint8_t> OldDestination(64);
    for (unsigned Byte = 0; Byte < OldDestination.size(); ++Byte)
      OldDestination[Byte] = static_cast<uint8_t>(0x80 + Byte);
    std::vector<uint8_t> Expected = OldDestination;
    setDword(Expected, ActiveLane, convertedBits(Case));

    BinaryImage Image = emptyImage();
    addDword(Image, Base + ActiveLane * 4, sourceBits(Case));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    Emulator.setRegister(x86reg::RAX, Base);
    Emulator.setRegister(x86reg::K1, Mask);
    Emulator.setRegisterBytes(x86reg::vectorReg(0), OldDestination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(0)), Expected);
    ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, Base + ActiveLane * 4);
    EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 4u);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86EVEXDwordConvertMemory,
     BroadcastZeroMaskSuppressesUnmappedAndUsesOneScalarLoadWhenActive) {
  constexpr uint64_t Address = UINT64_C(0x5000);
  constexpr uint64_t ActiveMask = UINT64_C(0x8001);

  for (const ConvertCase &Case : kExactCases) {
    SCOPED_TRACE(Case.Name);
    // vcvt* zmm0 {k1}{z}, dword ptr [rax]{1to16}
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf1, Case.P1, 0xd9, Case.Opcode, 0x00}, Case.Id);
    ASSERT_FALSE(Ops.empty());

    BinaryImage Unmapped = emptyImage();
    NdOpEmulator Suppressed(Unmapped);
    Suppressed.setStrictMode(true);
    Suppressed.setLoadCollect(true);
    Suppressed.setRegister(x86reg::RAX, Address);
    Suppressed.setRegister(x86reg::K1, 0);
    Suppressed.setRegisterBytes(x86reg::vectorReg(0),
                                std::vector<uint8_t>(64, 0xcc));
    ASSERT_EQ(Suppressed.run(Ops), Ops.size());
    EXPECT_EQ(Suppressed.getRegisterBytes(x86reg::vectorReg(0)),
              std::vector<uint8_t>(64, 0));
    EXPECT_TRUE(Suppressed.getLoadRecords().empty());
    EXPECT_FALSE(Suppressed.skips().any());

    std::vector<uint8_t> Expected(64, 0);
    setDword(Expected, 0, convertedBits(Case));
    setDword(Expected, 15, convertedBits(Case));
    BinaryImage Image = emptyImage();
    addDword(Image, Address, sourceBits(Case));
    NdOpEmulator Active(Image);
    Active.setStrictMode(true);
    Active.setLoadCollect(true);
    Active.setRegister(x86reg::RAX, Address);
    Active.setRegister(x86reg::K1, ActiveMask);
    Active.setRegisterBytes(x86reg::vectorReg(0),
                            std::vector<uint8_t>(64, 0xcc));
    ASSERT_EQ(Active.run(Ops), Ops.size());
    EXPECT_EQ(Active.getRegisterBytes(x86reg::vectorReg(0)), Expected);
    ASSERT_EQ(Active.getLoadRecords().size(), 1u);
    EXPECT_EQ(Active.getLoadRecords()[0].Addr, Address);
    EXPECT_EQ(Active.getLoadRecords()[0].Size, 4u);
    EXPECT_FALSE(Active.skips().any());
  }
}

TEST(X86EVEXDwordConvertMemory,
     SegmentAddr32Disp8AndDecodedTupleMutationsFailClosed) {
  // vcvtdq2ps zmm0 {k1}, zmmword ptr fs:[eax + 0x80]
  const std::vector<uint8_t> Encoding = {0x64, 0x67, 0x62, 0xf1, 0x7c,
                                         0x49, 0x5b, 0x40, 0x02};

  const std::vector<LowOp> Ops = liftX64(Encoding, X86_INS_VCVTDQ2PS);
  ASSERT_FALSE(Ops.empty());
  constexpr uint64_t FsBase = UINT64_C(0x100000000);
  constexpr uint32_t Eax = UINT32_C(0xfffff000);
  constexpr uint64_t LinearAddress = FsBase + UINT64_C(0xfffff080);
  BinaryImage Image = emptyImage();
  addDword(Image, LinearAddress, std::bit_cast<uint32_t>(int32_t(-17)));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setLoadCollect(true);
  ASSERT_TRUE(
      Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS, FsBase));
  Emulator.setRegister(x86reg::RAX, UINT64_C(0xaaaaaaaa00000000) | Eax);
  Emulator.setRegister(x86reg::K1, 1);
  Emulator.setRegisterBytes(x86reg::vectorReg(0), std::vector<uint8_t>(64, 0));
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  std::vector<uint8_t> Expected(64, 0);
  setDword(Expected, 0, std::bit_cast<uint32_t>(-17.0f));
  EXPECT_EQ(Emulator.getRegisterBytes(x86reg::vectorReg(0)), Expected);
  ASSERT_EQ(Emulator.getLoadRecords().size(), 1u);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Addr, LinearAddress);
  EXPECT_EQ(Emulator.getLoadRecords()[0].Size, 4u);
  EXPECT_FALSE(Emulator.skips().any());

  expectMutatedLiftFailsClosed(Encoding, X86_INS_VCVTDQ2PS,
                               [](cs_insn &Insn, cs_x86 &) {
                                 Insn.bytes[6] = 0x5c;
                                 return true;
                               });
  expectMutatedLiftFailsClosed(Encoding, X86_INS_VCVTDQ2PS,
                               [](cs_insn &Insn, cs_x86 &) {
                                 Insn.bytes[8] = 0x03;
                                 return true;
                               });
  expectMutatedLiftFailsClosed(Encoding, X86_INS_VCVTDQ2PS,
                               [](cs_insn &Insn, cs_x86 &) {
                                 Insn.bytes[0] = 0x65;
                                 return true;
                               });
  expectMutatedLiftFailsClosed(Encoding, X86_INS_VCVTDQ2PS,
                               [](cs_insn &Insn, cs_x86 &) {
                                 Insn.bytes[1] = 0x66;
                                 return true;
                               });

  const std::vector<uint8_t> Broadcast = {0x62, 0xf1, 0x7c, 0xd9, 0x5b, 0x00};
  expectMutatedLiftFailsClosed(
      Broadcast, X86_INS_VCVTDQ2PS, [](cs_insn &, cs_x86 &X86) {
        for (uint8_t Index = 0; Index < X86.op_count; ++Index) {
          if (X86.operands[Index].type != X86_OP_MEM)
            continue;
          X86.operands[Index].avx_bcast = X86_AVX_BCAST_INVALID;
          return true;
        }
        return false;
      });
  expectMutatedLiftFailsClosed(
      Broadcast, X86_INS_VCVTDQ2PS, [](cs_insn &, cs_x86 &X86) {
        for (uint8_t Index = 0; Index < X86.op_count; ++Index) {
          if (X86.operands[Index].type != X86_OP_MEM)
            continue;
          X86.operands[Index].size = 64;
          return true;
        }
        return false;
      });
}

TEST(X86EVEXDwordConvertMemory,
     MXCSRRoundedMemoryFormsRemainExplicitlyFailClosed) {
  // VCVTPS2DQ and VCVTPS2UDQ require MXCSR.RC in ordinary EVEX forms.
  expectStrictlyUnlifted({0x62, 0xf1, 0x7d, 0xc9, 0x5b, 0x00},
                         X86_INS_VCVTPS2DQ);
  expectStrictlyUnlifted({0x62, 0xf1, 0x7d, 0xd9, 0x5b, 0x00},
                         X86_INS_VCVTPS2DQ);
  expectStrictlyUnlifted({0x62, 0xf1, 0x7c, 0xc9, 0x79, 0x00},
                         X86_INS_VCVTPS2UDQ);
  expectStrictlyUnlifted({0x62, 0xf1, 0x7c, 0xd9, 0x79, 0x00},
                         X86_INS_VCVTPS2UDQ);
}

} // namespace
