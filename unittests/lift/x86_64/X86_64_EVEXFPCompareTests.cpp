//===- X86_64_EVEXFPCompareTests.cpp - EVEX FP compare semantics -------===//

#include "gtest/gtest.h"

#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/IR/LLVMContext.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kAddress = 0x1000;

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

template <typename Mutator>
std::vector<LowOp> liftMutatedX64(const std::vector<uint8_t> &Bytes,
                                 Mutator Mutate) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "x86-64 decoder initialization failed";
    return {};
  }

  DecodedInsn Insn{};
  const int Size =
      Dec.decodeOneForLift(Bytes.data(), Bytes.size(), kAddress, Insn);
  if (Size != static_cast<int>(Bytes.size()) || !Insn.Raw ||
      !Insn.Raw->detail) {
    ADD_FAILURE() << "x86-64 instruction decode failed";
    return {};
  }
  if (!Mutate(*Insn.Raw, Insn.Raw->detail->x86)) {
    ADD_FAILURE() << "decoded instruction mutation failed";
    return {};
  }

  std::vector<LowOp> Ops;
  Dec.liftToLow(Insn, Ops);
  return Ops;
}

template <typename T>
void setLane(std::vector<uint8_t> &Bytes, size_t Lane, T Bits) {
  ASSERT_LE((Lane + 1) * sizeof(T), Bytes.size());
  std::memcpy(Bytes.data() + Lane * sizeof(T), &Bits, sizeof(T));
}

uint64_t registerOffsetForVector(size_t Size, unsigned Index) {
  x86_reg Reg = X86_REG_INVALID;
  if (Size == 16)
    Reg = static_cast<x86_reg>(X86_REG_XMM0 + Index);
  else if (Size == 32)
    Reg = static_cast<x86_reg>(X86_REG_YMM0 + Index);
  else if (Size == 64)
    Reg = static_cast<x86_reg>(X86_REG_ZMM0 + Index);
  return mapCapstoneReg(Reg).Offset;
}

BinaryImage makeMemoryImage(uint64_t Address,
                            const std::vector<uint8_t> &Bytes) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Memory;
  Memory.VA = Address;
  Memory.Size = Bytes.size();
  Memory.Flags = SegmentFlags::Readable;
  Memory.Data = Bytes;
  Image.Segments.push_back(std::move(Memory));
  return Image;
}

constexpr std::array<uint8_t, 32> kRelationResults = {
    0x2, 0x1, 0x3, 0x8, 0xd, 0xe, 0xc, 0x7,
    0xa, 0x9, 0xb, 0x0, 0x5, 0x6, 0x4, 0xf,
    0x2, 0x1, 0x3, 0x8, 0xd, 0xe, 0xc, 0x7,
    0xa, 0x9, 0xb, 0x0, 0x5, 0x6, 0x4, 0xf,
};

constexpr std::array<bool, 32> kSignalsOnQuietNaN = {
    false, true,  true,  false, false, true,  true,  false,
    false, true,  true,  false, false, true,  true,  false,
    true,  false, false, true,  true,  false, false, true,
    true,  false, false, true,  true,  false, false, true,
};

MedVar temporary(int Id, uint16_t Size) {
  MedVar Value;
  Value.Kind = MedVar::Temp;
  Value.TheArch = Arch::X64;
  Value.Id = Id;
  Value.Size = Size;
  return Value;
}

MedFunc makeLLVMGuardProbe() {
  MedFunc Function;
  Function.Entry = kAddress;
  Function.Name = "evex_fp_compare_llvm_guard";
  Function.CC = CallingConv::SysV_AMD64;

  MedOp Compare;
  Compare.Addr = Function.Entry;
  Compare.Opcode = NdOp::INTRINSIC;
  Compare.Output = temporary(1, 2);
  Compare.addInput(MedVar::makeConst(
      static_cast<uint16_t>(Intrinsic::X86FPCompare), /*Sz=*/2));
  Compare.addInput(MedVar::makeConst(0, /*Sz=*/1));
  Compare.addInput(temporary(2, 64));
  Compare.addInput(temporary(3, 64));
  Compare.addInput(temporary(4, 2));
  Compare.addInput(MedVar::makeConst(0, /*Sz=*/1));

  MedOp Return;
  Return.Addr = Function.Entry + 1;
  Return.Opcode = NdOp::RETURN;

  MedBlock Block;
  Block.Id = 0;
  Block.Ops = {std::move(Compare), std::move(Return)};
  Function.Blocks.push_back(std::move(Block));
  return Function;
}

} // namespace

TEST(X86EVEXFPCompare, EveryPackedSinglePredicateHasExactTruthAndNaNMode) {
  std::vector<uint8_t> Left(64, 0);
  std::vector<uint8_t> Right(64, 0);
  setLane<uint32_t>(Left, 0, UINT32_C(0x3f800000));
  setLane<uint32_t>(Right, 0, UINT32_C(0x40000000)); // less
  setLane<uint32_t>(Left, 1, UINT32_C(0x40000000));
  setLane<uint32_t>(Right, 1, UINT32_C(0x40000000)); // equal
  setLane<uint32_t>(Left, 2, UINT32_C(0x40400000));
  setLane<uint32_t>(Right, 2, UINT32_C(0x40000000)); // greater
  setLane<uint32_t>(Left, 3, UINT32_C(0x7fc00001));  // quiet NaN
  setLane<uint32_t>(Right, 3, UINT32_C(0x3f800000));

  for (uint8_t Predicate = 0; Predicate < 32; ++Predicate) {
    SCOPED_TRACE(testing::Message()
                 << "predicate=" << static_cast<unsigned>(Predicate));
    // vcmp*ps k1 {k2}, zmm2, zmm3, imm8
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf1, 0x6c, 0x4a, 0xc2, 0xcb, Predicate});
    ASSERT_FALSE(Ops.empty());

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f80);
    Emulator.setRegisterBytes(registerOffsetForVector(64, 2), Left);
    Emulator.setRegisterBytes(registerOffsetForVector(64, 3), Right);
    Emulator.setRegister(x86reg::K2, 0x0f);
    Emulator.setRegister(x86reg::K1, UINT64_MAX);

    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_TRUE(Emulator.getRegister(x86reg::K1));
    EXPECT_EQ(*Emulator.getRegister(x86reg::K1),
              kRelationResults[Predicate]);
    EXPECT_EQ((Emulator.getMXCSR() & 1U) != 0,
              kSignalsOnQuietNaN[Predicate]);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86EVEXFPCompare, SignalingNaNRaisesInvalidForEveryPredicate) {
  std::vector<uint8_t> Left(16, 0);
  std::vector<uint8_t> Right(16, 0);
  setLane<uint32_t>(Left, 0, UINT32_C(0x7f800001));
  setLane<uint32_t>(Right, 0, UINT32_C(0x3f800000));

  for (uint8_t Predicate = 0; Predicate < 32; ++Predicate) {
    SCOPED_TRACE(testing::Message()
                 << "predicate=" << static_cast<unsigned>(Predicate));
    // vcmp*ss k1, xmm2, xmm3, imm8
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf1, 0x6e, 0x08, 0xc2, 0xcb, Predicate});
    ASSERT_FALSE(Ops.empty());

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setMXCSR(0x1f80);
    Emulator.setRegisterBytes(registerOffsetForVector(16, 2), Left);
    Emulator.setRegisterBytes(registerOffsetForVector(16, 3), Right);
    Emulator.setRegister(x86reg::K1, UINT64_MAX);

    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_TRUE(Emulator.getRegister(x86reg::K1));
    EXPECT_EQ(*Emulator.getRegister(x86reg::K1),
              static_cast<uint64_t>((kRelationResults[Predicate] >> 3) & 1));
    EXPECT_NE(Emulator.getMXCSR() & 1U, 0U);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86EVEXFPCompare, PackedDoubleWidthsWriteOnlyArchitecturalKBits) {
  struct WidthCase {
    uint8_t P2;
    size_t VectorSize;
    uint64_t Expected;
  };
  const std::array<WidthCase, 3> Cases = {{{0x08, 16, 0x03},
                                           {0x28, 32, 0x0f},
                                           {0x48, 64, 0xff}}};

  for (const WidthCase &Case : Cases) {
    SCOPED_TRACE(Case.VectorSize);
    // vcmp*pd k1, xmm/ymm/zmm2, xmm/ymm/zmm3, TRUE_UQ
    const std::vector<LowOp> Ops =
        liftX64({0x62, 0xf1, 0xed, Case.P2, 0xc2, 0xcb, 0x0f});
    ASSERT_FALSE(Ops.empty());

    std::vector<uint8_t> Left(Case.VectorSize, 0);
    std::vector<uint8_t> Right(Case.VectorSize, 0);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(registerOffsetForVector(Case.VectorSize, 2), Left);
    Emulator.setRegisterBytes(registerOffsetForVector(Case.VectorSize, 3),
                              Right);
    Emulator.setRegister(x86reg::K1, UINT64_MAX);

    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    ASSERT_TRUE(Emulator.getRegister(x86reg::K1));
    EXPECT_EQ(*Emulator.getRegister(x86reg::K1), Case.Expected);
  }
}

TEST(X86EVEXFPCompare, HighImmediateBitsAreIgnored) {
  std::vector<uint8_t> Left(16, 0);
  std::vector<uint8_t> Right(16, 0);
  setLane<uint32_t>(Left, 0, UINT32_C(0x3f800000));
  setLane<uint32_t>(Right, 0, UINT32_C(0x40000000));

  struct ImmediateCase {
    uint8_t Immediate;
    uint64_t Expected;
  };
  const std::array<ImmediateCase, 4> Cases = {{{0x00, 0},
                                               {0x20, 0},
                                               {0x1f, 1},
                                               {0xff, 1}}};
  for (const ImmediateCase &Case : Cases) {
    SCOPED_TRACE(testing::Message()
                 << "immediate=" << static_cast<unsigned>(Case.Immediate));
    const uint8_t DecodableImmediate = Case.Immediate & UINT8_C(0x1f);
    const std::vector<LowOp> Ops = liftMutatedX64(
        {0x62, 0xf1, 0x6e, 0x08, 0xc2, 0xcb, DecodableImmediate},
        [&](cs_insn &Insn, cs_x86 &X86) {
          Insn.bytes[Insn.size - 1] = Case.Immediate;
          for (uint8_t Index = 0; Index < X86.op_count; ++Index) {
            if (X86.operands[Index].type != X86_OP_IMM)
              continue;
            X86.operands[Index].imm = Case.Immediate;
            break;
          }
          return true;
        });
    ASSERT_FALSE(Ops.empty());

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::XMM2, Left);
    Emulator.setRegisterBytes(x86reg::XMM3, Right);
    Emulator.setRegister(x86reg::K1, UINT64_MAX);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::K1), Case.Expected);
  }
}

TEST(X86EVEXFPCompare, PackedSaeIgnoresLengthAndReadsOldDestinationMask) {
  const std::vector<uint8_t> Zeroes(64, 0);
  const uint64_t OldDestination = UINT64_C(0xffffffffffff8421);
  for (uint8_t Length : {UINT8_C(0x00), UINT8_C(0x20), UINT8_C(0x40),
                         UINT8_C(0x60)}) {
    SCOPED_TRACE(testing::Message()
                 << "encoded_length=" << static_cast<unsigned>(Length));
    // EVEX.b fixes packed register-source SAE at 512 bits.  aaa=1 makes K1
    // both destination and writemask, so the old K1 value must be consumed
    // before the zero-extended result is committed.
    const std::vector<LowOp> Ops = liftMutatedX64(
        {0x62, 0xf1, 0x6c, 0x59, 0xc2, 0xcb, 0x0f},
        [&](cs_insn &Insn, cs_x86 &X86) {
          const uint8_t P2 = static_cast<uint8_t>(0x19 | Length);
          Insn.bytes[3] = P2;
          X86.opcode[3] = P2;
          return true;
        });
    ASSERT_FALSE(Ops.empty());

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(registerOffsetForVector(64, 2), Zeroes);
    Emulator.setRegisterBytes(registerOffsetForVector(64, 3), Zeroes);
    Emulator.setRegister(x86reg::K1, OldDestination);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::K1), UINT64_C(0x8421));
    EXPECT_EQ(Emulator.getMXCSR(), UINT32_C(0x1f80));
  }
}

TEST(X86EVEXFPCompare, ScalarFormsIgnoreEncodedLength) {
  struct ScalarCase {
    uint8_t P1;
    std::vector<uint8_t> Left;
    std::vector<uint8_t> Right;
  };

  std::vector<uint8_t> SingleLeft(16, 0);
  std::vector<uint8_t> SingleRight(16, 0);
  setLane<uint32_t>(SingleLeft, 0, UINT32_C(0x3f800000));
  setLane<uint32_t>(SingleRight, 0, UINT32_C(0x3f800000));
  std::vector<uint8_t> DoubleLeft(16, 0);
  std::vector<uint8_t> DoubleRight(16, 0);
  setLane<uint64_t>(DoubleLeft, 0, UINT64_C(0x3ff0000000000000));
  setLane<uint64_t>(DoubleRight, 0, UINT64_C(0x3ff0000000000000));
  const std::array<ScalarCase, 2> Cases = {{
      {0x6e, SingleLeft, SingleRight},
      {0xef, DoubleLeft, DoubleRight},
  }};

  for (const ScalarCase &Case : Cases) {
    for (uint8_t Length : {UINT8_C(0x00), UINT8_C(0x20), UINT8_C(0x40),
                           UINT8_C(0x60)}) {
      SCOPED_TRACE(testing::Message()
                   << "p1=" << static_cast<unsigned>(Case.P1)
                   << " encoded_length=" << static_cast<unsigned>(Length));
      const std::vector<LowOp> Ops = liftX64(
          {0x62, 0xf1, Case.P1, static_cast<uint8_t>(0x08 | Length), 0xc2,
           0xcb, 0x00});
      ASSERT_FALSE(Ops.empty());

      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      NdOpEmulator Emulator(Image);
      Emulator.setStrictMode(true);
      Emulator.setRegisterBytes(x86reg::XMM2, Case.Left);
      Emulator.setRegisterBytes(x86reg::XMM3, Case.Right);
      Emulator.setRegister(x86reg::K1, UINT64_MAX);
      ASSERT_EQ(Emulator.run(Ops), Ops.size());
      EXPECT_EQ(Emulator.getRegister(x86reg::K1), 1U);
    }
  }
}

TEST(X86EVEXFPCompare, ScalarFormsUseOnlyLaneZeroAndMaskBitZero) {
  struct ScalarCase {
    uint8_t P1;
    size_t ElementSize;
    uint64_t SignalingNaN;
    uint64_t One;
  };
  const std::array<ScalarCase, 2> Cases = {{
      {0x6e, 4, UINT64_C(0x7f800001), UINT64_C(0x3f800000)},
      {0xef, 8, UINT64_C(0x7ff0000000000001),
       UINT64_C(0x3ff0000000000000)},
  }};

  for (const ScalarCase &Case : Cases) {
    SCOPED_TRACE(Case.ElementSize);
    std::vector<uint8_t> Left(16, 0);
    std::vector<uint8_t> Right(16, 0);
    if (Case.ElementSize == 4) {
      setLane<uint32_t>(Left, 0, static_cast<uint32_t>(Case.One));
      setLane<uint32_t>(Right, 0, static_cast<uint32_t>(Case.One));
      setLane<uint32_t>(Left, 1, static_cast<uint32_t>(Case.SignalingNaN));
      setLane<uint32_t>(Right, 1, static_cast<uint32_t>(Case.SignalingNaN));
    } else {
      setLane<uint64_t>(Left, 0, Case.One);
      setLane<uint64_t>(Right, 0, Case.One);
      setLane<uint64_t>(Left, 1, Case.SignalingNaN);
      setLane<uint64_t>(Right, 1, Case.SignalingNaN);
    }

    // Signaling EQ sees only the equal low lane; upper signaling NaNs are not
    // architectural inputs to a scalar compare.
    const std::vector<LowOp> Active =
        liftX64({0x62, 0xf1, Case.P1, 0x08, 0xc2, 0xcb, 0x10});
    ASSERT_FALSE(Active.empty());
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegisterBytes(x86reg::XMM2, Left);
    Emulator.setRegisterBytes(x86reg::XMM3, Right);
    Emulator.setRegister(x86reg::K1, UINT64_MAX);
    ASSERT_EQ(Emulator.run(Active), Active.size());
    ASSERT_TRUE(Emulator.getRegister(x86reg::K1));
    EXPECT_EQ(*Emulator.getRegister(x86reg::K1), 1U);
    EXPECT_EQ(Emulator.getMXCSR() & 1U, 0U);

    // k2[1] cannot activate a scalar compare.  With k2[0] clear, even a low
    // signaling NaN and subnormal pair is ignored, even when both exception
    // classes are unmasked, and every destination bit is cleared.
    if (Case.ElementSize == 4) {
      setLane<uint32_t>(Left, 0, static_cast<uint32_t>(Case.SignalingNaN));
      setLane<uint32_t>(Right, 0, UINT32_C(0x00000001));
    } else {
      setLane<uint64_t>(Left, 0, Case.SignalingNaN);
      setLane<uint64_t>(Right, 0, UINT64_C(0x0000000000000001));
    }
    const std::vector<LowOp> Inactive =
        liftX64({0x62, 0xf1, Case.P1, 0x0a, 0xc2, 0xcb, 0x10});
    ASSERT_FALSE(Inactive.empty());
    NdOpEmulator Masked(Image);
    Masked.setStrictMode(true);
    Masked.setMXCSR(0x1e00);
    Masked.setRegisterBytes(x86reg::XMM2, Left);
    Masked.setRegisterBytes(x86reg::XMM3, Right);
    Masked.setRegister(x86reg::K2, 2);
    Masked.setRegister(x86reg::K1, UINT64_MAX);
    ASSERT_EQ(Masked.run(Inactive), Inactive.size());
    ASSERT_TRUE(Masked.getRegister(x86reg::K1));
    EXPECT_EQ(*Masked.getRegister(x86reg::K1), 0U);
    EXPECT_EQ(Masked.getMXCSR(), 0x1e00U);
  }
}

TEST(X86EVEXFPCompare, InvalidExceptionIsAtomicAndSaeSuppressesIt) {
  std::vector<uint8_t> Left(16, 0);
  std::vector<uint8_t> Right(16, 0);
  setLane<uint32_t>(Left, 0, UINT32_C(0x7f800001));
  setLane<uint32_t>(Right, 0, UINT32_C(0x3f800000));
  const uint64_t OldDestination = UINT64_C(0x123456789abcdef0);

  // Quiet EQ still raises invalid for a signaling NaN.  With invalid
  // unmasked, the status bit is set but the K destination is not committed.
  const std::vector<LowOp> Ordinary =
      liftX64({0x62, 0xf1, 0x6e, 0x08, 0xc2, 0xcb, 0x00});
  ASSERT_FALSE(Ordinary.empty());
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Faulting(Image);
  Faulting.setStrictMode(true);
  Faulting.setMXCSR(0x1f00);
  Faulting.setRegisterBytes(x86reg::XMM2, Left);
  Faulting.setRegisterBytes(x86reg::XMM3, Right);
  Faulting.setRegister(x86reg::K1, OldDestination);
  EXPECT_LT(Faulting.run(Ordinary), Ordinary.size());
  EXPECT_EQ(Faulting.getRegister(x86reg::K1), OldDestination);
  EXPECT_NE(Faulting.getMXCSR() & 1U, 0U);
  EXPECT_FALSE(Faulting.skips().any());

  // The register-source SAE form computes the unordered false result without
  // changing exception status, even when invalid is unmasked in MXCSR.
  const std::vector<LowOp> Sae =
      liftX64({0x62, 0xf1, 0x6e, 0x18, 0xc2, 0xcb, 0x00});
  ASSERT_FALSE(Sae.empty());
  NdOpEmulator Suppressed(Image);
  Suppressed.setStrictMode(true);
  Suppressed.setMXCSR(0x1f00);
  Suppressed.setRegisterBytes(x86reg::XMM2, Left);
  Suppressed.setRegisterBytes(x86reg::XMM3, Right);
  Suppressed.setRegister(x86reg::K1, OldDestination);
  ASSERT_EQ(Suppressed.run(Sae), Sae.size());
  EXPECT_EQ(Suppressed.getRegister(x86reg::K1), 0U);
  EXPECT_EQ(Suppressed.getMXCSR(), 0x1f00U);
  EXPECT_FALSE(Suppressed.skips().any());
}

TEST(X86EVEXFPCompare, DenormalDazMaskAndSaeSemanticsAreExact) {
  std::vector<uint8_t> Left(16, 0);
  std::vector<uint8_t> Right(16, 0);
  setLane<uint32_t>(Left, 0, UINT32_C(0x80000001));
  setLane<uint32_t>(Right, 0, UINT32_C(0x00000000));
  const std::vector<LowOp> Ordinary =
      liftX64({0x62, 0xf1, 0x6e, 0x08, 0xc2, 0xcb, 0x00});
  ASSERT_FALSE(Ordinary.empty());

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;

  // With DAZ clear, the subnormal remains nonzero and raises the denormal
  // status flag.  The default denormal mask permits the false result to commit.
  NdOpEmulator Exact(Image);
  Exact.setStrictMode(true);
  Exact.setMXCSR(0x1f80);
  Exact.setRegisterBytes(x86reg::XMM2, Left);
  Exact.setRegisterBytes(x86reg::XMM3, Right);
  Exact.setRegister(x86reg::K1, UINT64_MAX);
  ASSERT_EQ(Exact.run(Ordinary), Ordinary.size());
  EXPECT_EQ(Exact.getRegister(x86reg::K1), 0U);
  EXPECT_NE(Exact.getMXCSR() & (1U << 1), 0U);

  // DAZ converts the negative subnormal to negative zero before comparison
  // and suppresses the denormal status flag entirely.
  NdOpEmulator Daz(Image);
  Daz.setStrictMode(true);
  Daz.setMXCSR(0x1fc0);
  Daz.setRegisterBytes(x86reg::XMM2, Left);
  Daz.setRegisterBytes(x86reg::XMM3, Right);
  Daz.setRegister(x86reg::K1, UINT64_MAX);
  ASSERT_EQ(Daz.run(Ordinary), Ordinary.size());
  EXPECT_EQ(Daz.getRegister(x86reg::K1), 1U);
  EXPECT_EQ(Daz.getMXCSR() & (1U << 1), 0U);

  // An unmasked denormal exception sets status but prevents any destination
  // write.  The old K value is therefore still observable.
  const uint64_t OldDestination = UINT64_C(0x123456789abcdef0);
  NdOpEmulator Faulting(Image);
  Faulting.setStrictMode(true);
  Faulting.setMXCSR(0x1e80);
  Faulting.setRegisterBytes(x86reg::XMM2, Left);
  Faulting.setRegisterBytes(x86reg::XMM3, Right);
  Faulting.setRegister(x86reg::K1, OldDestination);
  EXPECT_LT(Faulting.run(Ordinary), Ordinary.size());
  EXPECT_EQ(Faulting.getRegister(x86reg::K1), OldDestination);
  EXPECT_NE(Faulting.getMXCSR() & (1U << 1), 0U);

  // SAE still compares the unflushed value when DAZ is clear, but suppresses
  // both exception delivery and sticky-status updates.
  const std::vector<LowOp> Sae =
      liftX64({0x62, 0xf1, 0x6e, 0x18, 0xc2, 0xcb, 0x00});
  ASSERT_FALSE(Sae.empty());
  NdOpEmulator Suppressed(Image);
  Suppressed.setStrictMode(true);
  Suppressed.setMXCSR(0x1e80);
  Suppressed.setRegisterBytes(x86reg::XMM2, Left);
  Suppressed.setRegisterBytes(x86reg::XMM3, Right);
  Suppressed.setRegister(x86reg::K1, OldDestination);
  ASSERT_EQ(Suppressed.run(Sae), Sae.size());
  EXPECT_EQ(Suppressed.getRegister(x86reg::K1), 0U);
  EXPECT_EQ(Suppressed.getMXCSR(), 0x1e80U);
}

TEST(X86EVEXFPCompare, NaNPrecomputationSuppressesSameLaneDenormal) {
  std::vector<uint8_t> Left(16, 0);
  std::vector<uint8_t> Right(16, 0);
  setLane<uint32_t>(Left, 0, UINT32_C(0x7fc00001));
  setLane<uint32_t>(Right, 0, UINT32_C(0x00000001));
  const std::vector<LowOp> Ops =
      liftX64({0x62, 0xf1, 0x6e, 0x08, 0xc2, 0xcb, 0x00});
  ASSERT_FALSE(Ops.empty());

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setMXCSR(0x1f80);
  Emulator.setRegisterBytes(x86reg::XMM2, Left);
  Emulator.setRegisterBytes(x86reg::XMM3, Right);
  Emulator.setRegister(x86reg::K1, UINT64_MAX);
  ASSERT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegister(x86reg::K1), 0U);
  EXPECT_EQ(Emulator.getMXCSR() & 3U, 0U);
}

TEST(X86EVEXFPCompare, PackedMemoryBIsBroadcastAndZeroMaskSkipsLoad) {
  // vcmp*ps k1 {k2}, zmm2, dword ptr [rax]{1to16}, EQ_OQ
  const std::vector<LowOp> Ops =
      liftX64({0x62, 0xf1, 0x6c, 0x5a, 0xc2, 0x08, 0x00});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Address = UINT64_C(0x7000);
  std::vector<uint8_t> Left(64, 0);
  for (unsigned Lane = 0; Lane < 16; ++Lane)
    setLane<uint32_t>(Left, Lane,
                      (Lane & 1) == 0 ? UINT32_C(0x3f800000)
                                      : UINT32_C(0x40000000));

  BinaryImage Unmapped;
  Unmapped.Arch = Arch::X64;
  Unmapped.Bits = Bitness::Bits64;
  NdOpEmulator ZeroMask(Unmapped);
  ZeroMask.setStrictMode(true);
  ZeroMask.setLoadCollect(true);
  ZeroMask.setRegister(x86reg::RAX, Address);
  ZeroMask.setRegisterBytes(registerOffsetForVector(64, 2), Left);
  ZeroMask.setRegister(x86reg::K2, 0);
  ZeroMask.setRegister(x86reg::K1, UINT64_MAX);
  ASSERT_EQ(ZeroMask.run(Ops), Ops.size());
  EXPECT_EQ(ZeroMask.getRegister(x86reg::K1), 0U);
  EXPECT_TRUE(ZeroMask.getLoadRecords().empty());

  const uint32_t One = UINT32_C(0x3f800000);
  std::vector<uint8_t> Scalar(sizeof(One));
  std::memcpy(Scalar.data(), &One, sizeof(One));
  BinaryImage Image = makeMemoryImage(Address, Scalar);
  NdOpEmulator Active(Image);
  Active.setStrictMode(true);
  Active.setLoadCollect(true);
  Active.setRegister(x86reg::RAX, Address);
  Active.setRegisterBytes(registerOffsetForVector(64, 2), Left);
  Active.setRegister(x86reg::K2, UINT64_C(0xffff));
  Active.setRegister(x86reg::K1, UINT64_MAX);
  ASSERT_EQ(Active.run(Ops), Ops.size());
  EXPECT_EQ(Active.getRegister(x86reg::K1), UINT64_C(0x5555));
  ASSERT_EQ(Active.getLoadRecords().size(), 1U);
  EXPECT_EQ(Active.getLoadRecords()[0].Addr, Address);
  EXPECT_EQ(Active.getLoadRecords()[0].Size, 4U);
}

TEST(X86EVEXFPCompare, FullMemoryLoadsOnlyActiveLanesAndFaultIsAtomic) {
  // vcmp*ps k1 {k2}, zmm2, zmmword ptr [rax], EQ_OQ
  const std::vector<LowOp> Ops =
      liftX64({0x62, 0xf1, 0x6c, 0x4a, 0xc2, 0x08, 0x00});
  ASSERT_FALSE(Ops.empty());

  constexpr uint64_t Address = UINT64_C(0x8ffc);
  const uint32_t One = UINT32_C(0x3f800000);
  std::vector<uint8_t> First(64, 0);
  setLane<uint32_t>(First, 0, One);
  setLane<uint32_t>(First, 1, One);
  std::vector<uint8_t> FirstMemoryLane(sizeof(One));
  std::memcpy(FirstMemoryLane.data(), &One, sizeof(One));
  BinaryImage Image = makeMemoryImage(Address, FirstMemoryLane);

  NdOpEmulator OneLane(Image);
  OneLane.setStrictMode(true);
  OneLane.setLoadCollect(true);
  OneLane.setRegister(x86reg::RAX, Address);
  OneLane.setRegisterBytes(registerOffsetForVector(64, 2), First);
  OneLane.setRegister(x86reg::K2, 1);
  OneLane.setRegister(x86reg::K1, UINT64_MAX);
  ASSERT_EQ(OneLane.run(Ops), Ops.size());
  EXPECT_EQ(OneLane.getRegister(x86reg::K1), 1U);
  ASSERT_EQ(OneLane.getLoadRecords().size(), 1U);
  EXPECT_EQ(OneLane.getLoadRecords()[0].Addr, Address);

  const uint64_t OldDestination = UINT64_C(0x0123456789abcdef);
  NdOpEmulator Crossing(Image);
  Crossing.setStrictMode(true);
  Crossing.setRegister(x86reg::RAX, Address);
  Crossing.setRegisterBytes(registerOffsetForVector(64, 2), First);
  Crossing.setRegister(x86reg::K2, 3);
  Crossing.setRegister(x86reg::K1, OldDestination);
  EXPECT_LT(Crossing.run(Ops), Ops.size());
  EXPECT_EQ(Crossing.getRegister(x86reg::K1), OldDestination);
}

TEST(X86EVEXFPCompare, ZeroScalarWriteMaskSuppressesMemoryAccess) {
  // vcmp*ss k1 {k2}, xmm2, dword ptr [rax], EQ_OQ
  const std::vector<LowOp> Ops =
      liftX64({0x62, 0xf1, 0x6e, 0x0a, 0xc2, 0x08, 0x00});
  ASSERT_FALSE(Ops.empty());

  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  NdOpEmulator Suppressed(Image);
  Suppressed.setStrictMode(true);
  Suppressed.setLoadCollect(true);
  Suppressed.setRegister(x86reg::RAX, UINT64_C(0x0000800000000000));
  Suppressed.setRegister(x86reg::K2, 0);
  Suppressed.setRegister(x86reg::K1, UINT64_MAX);
  ASSERT_EQ(Suppressed.run(Ops), Ops.size());
  EXPECT_EQ(Suppressed.getRegister(x86reg::K1), 0U);
  EXPECT_TRUE(Suppressed.getLoadRecords().empty());
  EXPECT_FALSE(Suppressed.skips().any());

  NdOpEmulator Active(Image);
  Active.setStrictMode(true);
  Active.setRegister(x86reg::RAX, UINT64_C(0x0000800000000000));
  Active.setRegister(x86reg::K2, 1);
  Active.setRegister(x86reg::K1, UINT64_C(0x55aa));
  EXPECT_LT(Active.run(Ops), Ops.size());
  EXPECT_EQ(Active.getRegister(x86reg::K1), UINT64_C(0x55aa));
}

TEST(X86EVEXFPCompare, ExactStateFailsClosedAtLLVMBackend) {
  EXPECT_DEATH(
      {
        llvm::LLVMContext Context;
        MedFunc Probe = makeLLVMGuardProbe();
        (void)MedLLVMEmitter().emit({Probe}, Context, "evex-fp-compare-guard",
                                    Arch::X64);
      },
      "floating-point comparison intrinsic requires exact concrete emulation");
}
