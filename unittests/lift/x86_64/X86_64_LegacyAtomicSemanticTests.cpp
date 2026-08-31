//===- X86_64_LegacyAtomicSemanticTests.cpp - exact RMW semantics --------===//

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
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;
constexpr uint64_t kMemoryAddress = 0x8000;

std::vector<LowOp> lift(const std::vector<uint8_t> &Bytes) {
  Decoder Decoder;
  EXPECT_TRUE(Decoder.init(Arch::X64));
  DecodedInsn Instruction{};
  EXPECT_EQ(Decoder.decodeOneForLift(Bytes.data(), Bytes.size(),
                                     kInstructionAddress, Instruction),
            static_cast<int>(Bytes.size()));
  std::vector<LowOp> Ops;
  EXPECT_NO_THROW(Decoder.liftToLow(Instruction, Ops));
  return Ops;
}

const LowOp *findAtomic(const std::vector<LowOp> &Ops, NdOp Opcode) {
  const auto It = std::find_if(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
    return Op.Opcode == Opcode;
  });
  return It == Ops.end() ? nullptr : &*It;
}

const LowOp *findIntrinsic(const std::vector<LowOp> &Ops, Intrinsic Id) {
  const auto It = std::find_if(Ops.begin(), Ops.end(), [&](const LowOp &Op) {
    return Op.Opcode == NdOp::INTRINSIC && Op.NumInputs != 0 &&
           Op.Inputs[0].isConst() &&
           Op.Inputs[0].Offset == static_cast<uint64_t>(Id);
  });
  return It == Ops.end() ? nullptr : &*It;
}

BinaryImage memoryImage(unsigned Size, SegmentFlags Flags,
                        const std::vector<uint8_t> &Value) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Memory;
  Memory.VA = kMemoryAddress;
  Memory.Size = Size;
  Memory.Flags = Flags;
  Memory.Data.assign(Size, 0);
  std::copy_n(Value.begin(), std::min<size_t>(Value.size(), Size),
              Memory.Data.begin());
  Image.Segments.push_back(std::move(Memory));
  return Image;
}

std::vector<uint8_t> scalarBytes(uint64_t Value, unsigned Size) {
  std::vector<uint8_t> Bytes(Size);
  std::memcpy(Bytes.data(), &Value, std::min<unsigned>(Size, sizeof(Value)));
  return Bytes;
}

LowOp loadMemory(unsigned Size, uint64_t Output) {
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::tmp(Output, Size);
  Load.addInput(NdVar::cst(kMemoryAddress, 8));
  return Load;
}

void setFlags(NdOpEmulator &Emulator, uint64_t Value) {
  for (uint64_t Flag :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    Emulator.setRegister(Flag, Value);
}

void expectFlags(NdOpEmulator &Emulator, uint64_t Value) {
  for (uint64_t Flag :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    EXPECT_EQ(Emulator.getRegister(Flag).value_or(2), Value) << Flag;
}

TEST(X86LegacyAtomic, MemoryFormsDistinguishLockedAtomicity) {
  struct Case {
    std::vector<uint8_t> Bytes;
    NdOp Opcode;
    unsigned Width;
    bool Locked;
  };
  const std::array<Case, 8> Cases = {{
      {{0x0f, 0xb1, 0x37}, NdOp::ATOMIC_CMPXCHG, 4, false},
      {{0xf0, 0x0f, 0xb1, 0x37}, NdOp::ATOMIC_CMPXCHG, 4, true},
      {{0x0f, 0xc1, 0x37}, NdOp::ATOMIC_ADD, 4, false},
      {{0xf0, 0x0f, 0xc1, 0x37}, NdOp::ATOMIC_ADD, 4, true},
      {{0x0f, 0xc7, 0x0f}, NdOp::ATOMIC_CMPXCHG, 8, false},
      {{0xf0, 0x0f, 0xc7, 0x0f}, NdOp::ATOMIC_CMPXCHG, 8, true},
      {{0x48, 0x0f, 0xc7, 0x0f}, NdOp::ATOMIC_CMPXCHG, 16, false},
      {{0xf0, 0x48, 0x0f, 0xc7, 0x0f}, NdOp::ATOMIC_CMPXCHG, 16, true},
  }};

  for (const Case &Test : Cases) {
    const std::vector<LowOp> Ops = lift(Test.Bytes);
    const LowOp *Atomic = findAtomic(Ops, Test.Opcode);
    const auto StoreCount =
        std::count_if(Ops.begin(), Ops.end(),
                      [](const LowOp &Op) { return Op.Opcode == NdOp::STORE; });
    if (Test.Locked) {
      ASSERT_NE(Atomic, nullptr) << Test.Width;
      EXPECT_EQ(Atomic->Output.Size, Test.Width);
      EXPECT_EQ(Atomic->MemoryOrdering,
                NdMemoryOrdering::SequentiallyConsistent);
      EXPECT_EQ(StoreCount, 0);
    } else {
      EXPECT_EQ(Atomic, nullptr) << Test.Width;
      EXPECT_EQ(StoreCount, 1);
      EXPECT_EQ(std::count_if(Ops.begin(), Ops.end(),
                              [&](const LowOp &Op) {
                                return Op.Opcode == NdOp::LOAD &&
                                       Op.Output.Size == Test.Width;
                              }),
                1);
    }
    if (Test.Width == 16)
      EXPECT_NE(findIntrinsic(Ops, Intrinsic::RequireAligned), nullptr);
  }
}

TEST(X86LegacyAtomic, CmpxchgAndXaddFaultBeforeArchitecturalCommit) {
  const std::array<std::vector<uint8_t>, 2> Encodings = {
      std::vector<uint8_t>{0x0f, 0xb1, 0x37},
      std::vector<uint8_t>{0x0f, 0xc1, 0x37}};
  for (const std::vector<uint8_t> &Bytes : Encodings) {
    const std::vector<LowOp> Ops = lift(Bytes);
    BinaryImage Image = memoryImage(4, SegmentFlags::Readable,
                                    scalarBytes(UINT64_C(0x11223344), 4));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::RDI, kMemoryAddress);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0x11223344));
    Emulator.setRegister(x86reg::RSI, UINT64_C(0xaabbccdd));
    setFlags(Emulator, 1);

    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::RAX).value_or(0),
              UINT64_C(0x11223344));
    EXPECT_EQ(Emulator.getRegister(x86reg::RSI).value_or(0),
              UINT64_C(0xaabbccdd));
    expectFlags(Emulator, 1);
    ASSERT_TRUE(Emulator.step(loadMemory(4, 0x71000000)));
    EXPECT_EQ(Emulator.getRegister(0x71000000).value_or(0),
              UINT64_C(0x11223344));
  }
}

TEST(X86LegacyAtomic, Cmpxchg32ConditionallyZeroExtendsTheAccumulator) {
  const std::vector<LowOp> Ops = lift({0x0f, 0xb1, 0x37});
  const uint64_t Accumulator = UINT64_C(0xaabbccdd11223344);

  BinaryImage Matching =
      memoryImage(4, SegmentFlags::Readable | SegmentFlags::Writable,
                  scalarBytes(UINT64_C(0x11223344), 4));
  NdOpEmulator Success(Matching);
  Success.setStrictMode(true);
  Success.setRegister(x86reg::RDI, kMemoryAddress);
  Success.setRegister(x86reg::RAX, Accumulator);
  Success.setRegister(x86reg::RSI, UINT64_C(0x55667788));
  EXPECT_EQ(Success.run(Ops), Ops.size());
  EXPECT_EQ(Success.getRegister(x86reg::ZF).value_or(0), 1u);
  EXPECT_EQ(Success.getRegister(x86reg::RAX).value_or(0), Accumulator);

  BinaryImage Different =
      memoryImage(4, SegmentFlags::Readable | SegmentFlags::Writable,
                  scalarBytes(UINT64_C(0x88776655), 4));
  NdOpEmulator Failure(Different);
  Failure.setStrictMode(true);
  Failure.setRegister(x86reg::RDI, kMemoryAddress);
  Failure.setRegister(x86reg::RAX, Accumulator);
  Failure.setRegister(x86reg::RSI, UINT64_C(0x55667788));
  EXPECT_EQ(Failure.run(Ops), Ops.size());
  EXPECT_EQ(Failure.getRegister(x86reg::ZF).value_or(1), 0u);
  EXPECT_EQ(Failure.getRegister(x86reg::RAX).value_or(UINT64_MAX),
            UINT64_C(0x0000000088776655));
}

TEST(X86LegacyAtomic, CmpxchgReadOnlyMismatchStillFaultsBeforeCommit) {
  const std::vector<LowOp> Ops = lift({0x0f, 0xb1, 0x37});
  BinaryImage ReadOnly = memoryImage(4, SegmentFlags::Readable,
                                     scalarBytes(UINT64_C(0x88776655), 4));
  NdOpEmulator Emulator(ReadOnly);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RDI, kMemoryAddress);
  Emulator.setRegister(x86reg::RAX, UINT64_C(0xaabbccdd11223344));
  Emulator.setRegister(x86reg::RSI, UINT64_C(0x55667788));
  setFlags(Emulator, 1);

  EXPECT_LT(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegister(x86reg::RAX).value_or(0),
            UINT64_C(0xaabbccdd11223344));
  expectFlags(Emulator, 1);
  ASSERT_TRUE(Emulator.step(loadMemory(4, 0x71500000)));
  EXPECT_EQ(Emulator.getRegister(0x71500000).value_or(0), UINT64_C(0x88776655));
}

TEST(X86LegacyAtomic, Xadd32CommitsSumFlagsAndZeroExtendsSource) {
  const std::vector<LowOp> Ops = lift({0x0f, 0xc1, 0x37});
  BinaryImage Image =
      memoryImage(4, SegmentFlags::Readable | SegmentFlags::Writable,
                  scalarBytes(UINT64_C(0xffffffff), 4));
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RDI, kMemoryAddress);
  Emulator.setRegister(x86reg::RSI, UINT64_C(0xdeadbeef00000001));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegister(x86reg::RSI).value_or(UINT64_MAX),
            UINT64_C(0x00000000ffffffff));
  EXPECT_EQ(Emulator.getRegister(x86reg::CF).value_or(0), 1u);
  EXPECT_EQ(Emulator.getRegister(x86reg::PF).value_or(0), 1u);
  EXPECT_EQ(Emulator.getRegister(x86reg::AF).value_or(0), 1u);
  EXPECT_EQ(Emulator.getRegister(x86reg::ZF).value_or(0), 1u);
  EXPECT_EQ(Emulator.getRegister(x86reg::SF).value_or(1), 0u);
  EXPECT_EQ(Emulator.getRegister(x86reg::OF).value_or(1), 0u);
  ASSERT_TRUE(Emulator.step(loadMemory(4, 0x71600000)));
  EXPECT_EQ(Emulator.getRegister(0x71600000).value_or(UINT64_MAX), 0u);
}

TEST(X86LegacyAtomic, Cmpxchg8bPreservesOrZeroExtendsAccumulatorPair) {
  const std::vector<LowOp> Ops = lift({0x0f, 0xc7, 0x0f});
  const uint32_t OldLow = 0x89abcdef;
  const uint32_t OldHigh = 0x76543210;
  const uint32_t NewLow = 0x11223344;
  const uint32_t NewHigh = 0xaabbccdd;
  const uint64_t Old = (static_cast<uint64_t>(OldHigh) << 32) | OldLow;
  const uint64_t New = (static_cast<uint64_t>(NewHigh) << 32) | NewLow;

  BinaryImage Matching = memoryImage(
      8, SegmentFlags::Readable | SegmentFlags::Writable, scalarBytes(Old, 8));
  NdOpEmulator Success(Matching);
  Success.setStrictMode(true);
  Success.setRegister(x86reg::RDI, kMemoryAddress);
  Success.setRegister(x86reg::RAX, UINT64_C(0xfeedface00000000) | OldLow);
  Success.setRegister(x86reg::RDX, UINT64_C(0xcafebabe00000000) | OldHigh);
  Success.setRegister(x86reg::RBX, UINT64_C(0x1111111100000000) | NewLow);
  Success.setRegister(x86reg::RCX, UINT64_C(0x2222222200000000) | NewHigh);
  EXPECT_EQ(Success.run(Ops), Ops.size());
  EXPECT_EQ(Success.getRegister(x86reg::ZF).value_or(0), 1u);
  EXPECT_EQ(Success.getRegister(x86reg::RAX).value_or(0),
            UINT64_C(0xfeedface00000000) | OldLow);
  EXPECT_EQ(Success.getRegister(x86reg::RDX).value_or(0),
            UINT64_C(0xcafebabe00000000) | OldHigh);
  ASSERT_TRUE(Success.step(loadMemory(8, 0x71700000)));
  EXPECT_EQ(Success.getRegister(0x71700000).value_or(0), New);

  const uint64_t Different = UINT64_C(0x0123456787654321);
  BinaryImage Mismatch =
      memoryImage(8, SegmentFlags::Readable | SegmentFlags::Writable,
                  scalarBytes(Different, 8));
  NdOpEmulator Failure(Mismatch);
  Failure.setStrictMode(true);
  Failure.setRegister(x86reg::RDI, kMemoryAddress);
  Failure.setRegister(x86reg::RAX, UINT64_C(0xfeedface00000000) | OldLow);
  Failure.setRegister(x86reg::RDX, UINT64_C(0xcafebabe00000000) | OldHigh);
  Failure.setRegister(x86reg::RBX, NewLow);
  Failure.setRegister(x86reg::RCX, NewHigh);
  EXPECT_EQ(Failure.run(Ops), Ops.size());
  EXPECT_EQ(Failure.getRegister(x86reg::ZF).value_or(1), 0u);
  EXPECT_EQ(Failure.getRegister(x86reg::RAX).value_or(UINT64_MAX),
            UINT64_C(0x0000000087654321));
  EXPECT_EQ(Failure.getRegister(x86reg::RDX).value_or(UINT64_MAX),
            UINT64_C(0x0000000001234567));
  ASSERT_TRUE(Failure.step(loadMemory(8, 0x71800000)));
  EXPECT_EQ(Failure.getRegister(0x71800000).value_or(0), Different);
}

TEST(X86LegacyAtomic, Cmpxchg16bCommitsAll128BitsOrNothing) {
  const std::vector<LowOp> Ops = lift({0x48, 0x0f, 0xc7, 0x0f});
  const uint64_t OldLow = UINT64_C(0x0123456789abcdef);
  const uint64_t OldHigh = UINT64_C(0xfedcba9876543210);
  const uint64_t NewLow = UINT64_C(0x1111222233334444);
  const uint64_t NewHigh = UINT64_C(0xaaaabbbbccccdddd);
  std::vector<uint8_t> Old(16);
  std::memcpy(Old.data(), &OldLow, 8);
  std::memcpy(Old.data() + 8, &OldHigh, 8);

  BinaryImage Writable =
      memoryImage(16, SegmentFlags::Readable | SegmentFlags::Writable, Old);
  NdOpEmulator Success(Writable);
  Success.setStrictMode(true);
  Success.setRegister(x86reg::RDI, kMemoryAddress);
  Success.setRegister(x86reg::RAX, OldLow);
  Success.setRegister(x86reg::RDX, OldHigh);
  Success.setRegister(x86reg::RBX, NewLow);
  Success.setRegister(x86reg::RCX, NewHigh);
  EXPECT_EQ(Success.run(Ops), Ops.size());
  EXPECT_EQ(Success.getRegister(x86reg::ZF).value_or(0), 1u);
  ASSERT_TRUE(Success.step(loadMemory(16, 0x72000000)));
  const auto Stored = Success.getRegisterBytes(0x72000000);
  ASSERT_TRUE(Stored.has_value());
  std::vector<uint8_t> Expected(16);
  std::memcpy(Expected.data(), &NewLow, 8);
  std::memcpy(Expected.data() + 8, &NewHigh, 8);
  EXPECT_EQ(*Stored, Expected);

  BinaryImage ReadOnly = memoryImage(16, SegmentFlags::Readable, Old);
  NdOpEmulator Fault(ReadOnly);
  Fault.setStrictMode(true);
  Fault.setRegister(x86reg::RDI, kMemoryAddress);
  Fault.setRegister(x86reg::RAX, OldLow);
  Fault.setRegister(x86reg::RDX, OldHigh);
  Fault.setRegister(x86reg::RBX, NewLow);
  Fault.setRegister(x86reg::RCX, NewHigh);
  setFlags(Fault, 0);
  EXPECT_LT(Fault.run(Ops), Ops.size());
  EXPECT_EQ(Fault.getRegister(x86reg::RAX).value_or(0), OldLow);
  EXPECT_EQ(Fault.getRegister(x86reg::RDX).value_or(0), OldHigh);
  expectFlags(Fault, 0);
  ASSERT_TRUE(Fault.step(loadMemory(16, 0x73000000)));
  EXPECT_EQ(Fault.getRegisterBytes(0x73000000),
            std::optional<std::vector<uint8_t>>(Old));
}

TEST(X86LegacyAtomic, Cmpxchg16bMismatchReturnsOldPairWithoutWriting) {
  const std::vector<LowOp> Ops = lift({0x48, 0x0f, 0xc7, 0x0f});
  const uint64_t OldLow = UINT64_C(0x0123456789abcdef);
  const uint64_t OldHigh = UINT64_C(0xfedcba9876543210);
  std::vector<uint8_t> Old(16);
  std::memcpy(Old.data(), &OldLow, 8);
  std::memcpy(Old.data() + 8, &OldHigh, 8);

  BinaryImage Image =
      memoryImage(16, SegmentFlags::Readable | SegmentFlags::Writable, Old);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RDI, kMemoryAddress);
  Emulator.setRegister(x86reg::RAX, UINT64_C(0x1111111111111111));
  Emulator.setRegister(x86reg::RDX, UINT64_C(0x2222222222222222));
  Emulator.setRegister(x86reg::RBX, UINT64_C(0x3333333333333333));
  Emulator.setRegister(x86reg::RCX, UINT64_C(0x4444444444444444));

  EXPECT_EQ(Emulator.run(Ops), Ops.size());
  EXPECT_EQ(Emulator.getRegister(x86reg::ZF).value_or(1), 0u);
  EXPECT_EQ(Emulator.getRegister(x86reg::RAX).value_or(0), OldLow);
  EXPECT_EQ(Emulator.getRegister(x86reg::RDX).value_or(0), OldHigh);
  ASSERT_TRUE(Emulator.step(loadMemory(16, 0x73500000)));
  EXPECT_EQ(Emulator.getRegisterBytes(0x73500000),
            std::optional<std::vector<uint8_t>>(Old));
}

TEST(X86LegacyAtomic, Cmpxchg16bRejectsMisalignmentBeforeStateCommit) {
  const uint64_t OldLow = UINT64_C(0x0123456789abcdef);
  const uint64_t OldHigh = UINT64_C(0xfedcba9876543210);
  std::vector<uint8_t> Old(32, 0);
  std::memcpy(Old.data() + 1, &OldLow, 8);
  std::memcpy(Old.data() + 9, &OldHigh, 8);
  const std::array<std::vector<uint8_t>, 2> Encodings = {
      std::vector<uint8_t>{0x48, 0x0f, 0xc7, 0x0f},
      std::vector<uint8_t>{0xf0, 0x48, 0x0f, 0xc7, 0x0f}};
  for (const std::vector<uint8_t> &Bytes : Encodings) {
    SCOPED_TRACE(Bytes.front() == 0xf0 ? "locked" : "unlocked");
    const std::vector<LowOp> Ops = lift(Bytes);
    BinaryImage Writable =
        memoryImage(32, SegmentFlags::Readable | SegmentFlags::Writable, Old);
    NdOpEmulator Emulator(Writable);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::RDI, kMemoryAddress + 1);
    Emulator.setRegister(x86reg::RAX, OldLow);
    Emulator.setRegister(x86reg::RDX, OldHigh);
    Emulator.setRegister(x86reg::RBX, UINT64_C(0x1111222233334444));
    Emulator.setRegister(x86reg::RCX, UINT64_C(0xaaaabbbbccccdddd));
    setFlags(Emulator, 1);

    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::RAX).value_or(0), OldLow);
    EXPECT_EQ(Emulator.getRegister(x86reg::RDX).value_or(0), OldHigh);
    expectFlags(Emulator, 1);
    LowOp Load = loadMemory(16, 0x74000000);
    Load.Inputs[0] = NdVar::cst(kMemoryAddress + 1, 8);
    ASSERT_TRUE(Emulator.step(Load));
    std::vector<uint8_t> Expected(16);
    std::copy_n(Old.begin() + 1, 16, Expected.begin());
    EXPECT_EQ(Emulator.getRegisterBytes(0x74000000),
              std::optional<std::vector<uint8_t>>(Expected));
  }
}

TEST(X86LegacyAtomic, Cmpxchg16bAlignsTheFinalSegmentedAddress) {
  const std::vector<LowOp> Ops = lift({0x65, 0x48, 0x0f, 0xc7, 0x0f});
  const LowOp *Alignment = findIntrinsic(Ops, Intrinsic::RequireAligned);
  ASSERT_NE(Alignment, nullptr);
  EXPECT_EQ(Alignment->MemoryAddressSpace, NdMemoryAddressSpace::X86GS);

  const uint64_t OldLow = UINT64_C(0x0123456789abcdef);
  const uint64_t OldHigh = UINT64_C(0xfedcba9876543210);
  const uint64_t NewLow = UINT64_C(0x1111222233334444);
  const uint64_t NewHigh = UINT64_C(0xaaaabbbbccccdddd);
  std::vector<uint8_t> Old(16);
  std::memcpy(Old.data(), &OldLow, 8);
  std::memcpy(Old.data() + 8, &OldHigh, 8);
  BinaryImage Writable =
      memoryImage(16, SegmentFlags::Readable | SegmentFlags::Writable, Old);

  // The raw GS offset is deliberately unaligned; only the final linear
  // address is aligned and therefore architecturally admissible.
  NdOpEmulator Success(Writable);
  Success.setStrictMode(true);
  ASSERT_TRUE(Success.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS,
                                                kMemoryAddress - 8));
  Success.setRegister(x86reg::RDI, 8);
  Success.setRegister(x86reg::RAX, OldLow);
  Success.setRegister(x86reg::RDX, OldHigh);
  Success.setRegister(x86reg::RBX, NewLow);
  Success.setRegister(x86reg::RCX, NewHigh);
  EXPECT_EQ(Success.run(Ops), Ops.size());
  EXPECT_EQ(Success.getRegister(x86reg::ZF).value_or(0), 1u);
  ASSERT_TRUE(Success.step(loadMemory(16, 0x75000000)));
  std::vector<uint8_t> Expected(16);
  std::memcpy(Expected.data(), &NewLow, 8);
  std::memcpy(Expected.data() + 8, &NewHigh, 8);
  EXPECT_EQ(Success.getRegisterBytes(0x75000000),
            std::optional<std::vector<uint8_t>>(Expected));

  // Conversely, an aligned offset cannot hide a misaligned GS base.
  NdOpEmulator Fault(Writable);
  Fault.setStrictMode(true);
  ASSERT_TRUE(Fault.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86GS,
                                              kMemoryAddress + 1));
  Fault.setRegister(x86reg::RDI, 0);
  Fault.setRegister(x86reg::RAX, OldLow);
  Fault.setRegister(x86reg::RDX, OldHigh);
  Fault.setRegister(x86reg::RBX, NewLow);
  Fault.setRegister(x86reg::RCX, NewHigh);
  setFlags(Fault, 1);
  EXPECT_LT(Fault.run(Ops), Ops.size());
  EXPECT_EQ(Fault.getRegister(x86reg::RAX).value_or(0), OldLow);
  EXPECT_EQ(Fault.getRegister(x86reg::RDX).value_or(0), OldHigh);
  expectFlags(Fault, 1);
}

TEST(X86LegacyAtomic, WideCmpxchgReadOnlyMismatchFaultsBeforeCommit) {
  const uint64_t Old8 = UINT64_C(0x0123456787654321);
  const std::array<std::vector<uint8_t>, 2> Cmpxchg8b = {
      std::vector<uint8_t>{0x0f, 0xc7, 0x0f},
      std::vector<uint8_t>{0xf0, 0x0f, 0xc7, 0x0f}};
  for (const std::vector<uint8_t> &Bytes : Cmpxchg8b) {
    SCOPED_TRACE(Bytes.front() == 0xf0 ? "locked-cmpxchg8b" : "cmpxchg8b");
    const std::vector<LowOp> Ops = lift(Bytes);
    BinaryImage Image =
        memoryImage(8, SegmentFlags::Readable, scalarBytes(Old8, 8));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::RDI, kMemoryAddress);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0xaaaa000011111111));
    Emulator.setRegister(x86reg::RDX, UINT64_C(0xbbbb000022222222));
    Emulator.setRegister(x86reg::RBX, UINT64_C(0x33333333));
    Emulator.setRegister(x86reg::RCX, UINT64_C(0x44444444));
    setFlags(Emulator, 1);

    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::RAX).value_or(0),
              UINT64_C(0xaaaa000011111111));
    EXPECT_EQ(Emulator.getRegister(x86reg::RDX).value_or(0),
              UINT64_C(0xbbbb000022222222));
    expectFlags(Emulator, 1);
    ASSERT_TRUE(Emulator.step(loadMemory(8, 0x76000000)));
    EXPECT_EQ(Emulator.getRegister(0x76000000).value_or(0), Old8);
  }

  const uint64_t OldLow = UINT64_C(0x0123456789abcdef);
  const uint64_t OldHigh = UINT64_C(0xfedcba9876543210);
  std::vector<uint8_t> Old16(16);
  std::memcpy(Old16.data(), &OldLow, 8);
  std::memcpy(Old16.data() + 8, &OldHigh, 8);
  const std::array<std::vector<uint8_t>, 2> Cmpxchg16b = {
      std::vector<uint8_t>{0x48, 0x0f, 0xc7, 0x0f},
      std::vector<uint8_t>{0xf0, 0x48, 0x0f, 0xc7, 0x0f}};
  for (const std::vector<uint8_t> &Bytes : Cmpxchg16b) {
    SCOPED_TRACE(Bytes.front() == 0xf0 ? "locked-cmpxchg16b" : "cmpxchg16b");
    const std::vector<LowOp> Ops = lift(Bytes);
    BinaryImage Image = memoryImage(16, SegmentFlags::Readable, Old16);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::RDI, kMemoryAddress);
    Emulator.setRegister(x86reg::RAX, UINT64_C(0x1111111111111111));
    Emulator.setRegister(x86reg::RDX, UINT64_C(0x2222222222222222));
    Emulator.setRegister(x86reg::RBX, UINT64_C(0x3333333333333333));
    Emulator.setRegister(x86reg::RCX, UINT64_C(0x4444444444444444));
    setFlags(Emulator, 1);

    EXPECT_LT(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::RAX).value_or(0),
              UINT64_C(0x1111111111111111));
    EXPECT_EQ(Emulator.getRegister(x86reg::RDX).value_or(0),
              UINT64_C(0x2222222222222222));
    expectFlags(Emulator, 1);
    ASSERT_TRUE(Emulator.step(loadMemory(16, 0x76100000)));
    EXPECT_EQ(Emulator.getRegisterBytes(0x76100000),
              std::optional<std::vector<uint8_t>>(Old16));
  }
}

TEST(X86LegacyAtomic, UnalignedTwoFourAndEightByteRmwFormsAreExact) {
  enum class Kind { Cmpxchg, Xadd, Cmpxchg8b };
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    Kind Operation;
    unsigned Width;
  };
  const std::array<Case, 14> Cases = {{
      {"cmpxchg16", {0x66, 0x0f, 0xb1, 0x37}, Kind::Cmpxchg, 2},
      {"lock-cmpxchg16", {0x66, 0xf0, 0x0f, 0xb1, 0x37}, Kind::Cmpxchg, 2},
      {"cmpxchg32", {0x0f, 0xb1, 0x37}, Kind::Cmpxchg, 4},
      {"lock-cmpxchg32", {0xf0, 0x0f, 0xb1, 0x37}, Kind::Cmpxchg, 4},
      {"cmpxchg64", {0x48, 0x0f, 0xb1, 0x37}, Kind::Cmpxchg, 8},
      {"lock-cmpxchg64", {0xf0, 0x48, 0x0f, 0xb1, 0x37}, Kind::Cmpxchg, 8},
      {"xadd16", {0x66, 0x0f, 0xc1, 0x37}, Kind::Xadd, 2},
      {"lock-xadd16", {0x66, 0xf0, 0x0f, 0xc1, 0x37}, Kind::Xadd, 2},
      {"xadd32", {0x0f, 0xc1, 0x37}, Kind::Xadd, 4},
      {"lock-xadd32", {0xf0, 0x0f, 0xc1, 0x37}, Kind::Xadd, 4},
      {"xadd64", {0x48, 0x0f, 0xc1, 0x37}, Kind::Xadd, 8},
      {"lock-xadd64", {0xf0, 0x48, 0x0f, 0xc1, 0x37}, Kind::Xadd, 8},
      {"cmpxchg8b", {0x0f, 0xc7, 0x0f}, Kind::Cmpxchg8b, 8},
      {"lock-cmpxchg8b", {0xf0, 0x0f, 0xc7, 0x0f}, Kind::Cmpxchg8b, 8},
  }};

  for (const Case &Test : Cases) {
    SCOPED_TRACE(Test.Name);
    const uint64_t Old = Test.Width == 2   ? UINT64_C(0x1234)
                         : Test.Width == 4 ? UINT64_C(0x11223344)
                                           : UINT64_C(0x1122334455667788);
    const uint64_t New = Test.Width == 2   ? UINT64_C(0x5678)
                         : Test.Width == 4 ? UINT64_C(0xaabbccdd)
                                           : UINT64_C(0xaabbccddeeff0011);
    std::vector<uint8_t> Contents(32, 0);
    std::memcpy(Contents.data() + 1, &Old, Test.Width);
    BinaryImage Image = memoryImage(
        32, SegmentFlags::Readable | SegmentFlags::Writable, Contents);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::RDI, kMemoryAddress + 1);
    if (Test.Operation == Kind::Cmpxchg) {
      Emulator.setRegister(x86reg::RAX, Old);
      Emulator.setRegister(x86reg::RSI, New);
    } else if (Test.Operation == Kind::Xadd) {
      Emulator.setRegister(x86reg::RSI, 1);
    } else {
      Emulator.setRegister(x86reg::RAX, static_cast<uint32_t>(Old));
      Emulator.setRegister(x86reg::RDX, Old >> 32);
      Emulator.setRegister(x86reg::RBX, static_cast<uint32_t>(New));
      Emulator.setRegister(x86reg::RCX, New >> 32);
    }

    const std::vector<LowOp> Ops = lift(Test.Bytes);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_FALSE(Emulator.skips().any());
    LowOp Load = loadMemory(Test.Width, 0x76200000);
    Load.Inputs[0] = NdVar::cst(kMemoryAddress + 1, 8);
    ASSERT_TRUE(Emulator.step(Load));
    const uint64_t Expected = Test.Operation == Kind::Xadd ? Old + 1 : New;
    EXPECT_EQ(Emulator.getRegister(0x76200000).value_or(UINT64_MAX), Expected);
  }
}

TEST(X86LegacyAtomic, SegmentedUnalignedXaddUsesFinalLinearAddress) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    NdMemoryAddressSpace AddressSpace;
    unsigned Width;
  };
  const std::array<Case, 8> Cases = {{
      {"fs-xadd32", {0x64, 0x0f, 0xc1, 0x37}, NdMemoryAddressSpace::X86FS, 4},
      {"fs-lock-xadd32",
       {0x64, 0xf0, 0x0f, 0xc1, 0x37},
       NdMemoryAddressSpace::X86FS,
       4},
      {"gs-xadd32", {0x65, 0x0f, 0xc1, 0x37}, NdMemoryAddressSpace::X86GS, 4},
      {"gs-lock-xadd32",
       {0x65, 0xf0, 0x0f, 0xc1, 0x37},
       NdMemoryAddressSpace::X86GS,
       4},
      {"fs-xadd64",
       {0x64, 0x48, 0x0f, 0xc1, 0x37},
       NdMemoryAddressSpace::X86FS,
       8},
      {"fs-lock-xadd64",
       {0x64, 0xf0, 0x48, 0x0f, 0xc1, 0x37},
       NdMemoryAddressSpace::X86FS,
       8},
      {"gs-xadd64",
       {0x65, 0x48, 0x0f, 0xc1, 0x37},
       NdMemoryAddressSpace::X86GS,
       8},
      {"gs-lock-xadd64",
       {0x65, 0xf0, 0x48, 0x0f, 0xc1, 0x37},
       NdMemoryAddressSpace::X86GS,
       8},
  }};

  for (const Case &Test : Cases) {
    SCOPED_TRACE(Test.Name);
    const uint64_t Old =
        Test.Width == 4 ? UINT64_C(0x11223344) : UINT64_C(0x1122334455667788);
    std::vector<uint8_t> Contents(32, 0);
    std::memcpy(Contents.data() + 1, &Old, Test.Width);
    BinaryImage Image = memoryImage(
        32, SegmentFlags::Readable | SegmentFlags::Writable, Contents);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(Test.AddressSpace,
                                                   kMemoryAddress - 0x20));
    Emulator.setRegister(x86reg::RDI, 0x21);
    Emulator.setRegister(x86reg::RSI, 1);

    const std::vector<LowOp> Ops = lift(Test.Bytes);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::RSI).value_or(UINT64_MAX), Old);
    LowOp Load = loadMemory(Test.Width, 0x76300000);
    Load.Inputs[0] = NdVar::cst(kMemoryAddress + 1, 8);
    ASSERT_TRUE(Emulator.step(Load));
    EXPECT_EQ(Emulator.getRegister(0x76300000).value_or(UINT64_MAX), Old + 1);
  }
}

TEST(X86LegacyAtomic, XaddSnapshotsAliasedAddressBeforeRegisterWriteback) {
  struct Case {
    const char *Name;
    std::vector<uint8_t> Bytes;
    unsigned Width;
  };
  const std::array<Case, 4> Cases = {{
      {"xadd32", {0x0f, 0xc1, 0x36}, 4},
      {"lock-xadd32", {0xf0, 0x0f, 0xc1, 0x36}, 4},
      {"xadd64", {0x48, 0x0f, 0xc1, 0x36}, 8},
      {"lock-xadd64", {0xf0, 0x48, 0x0f, 0xc1, 0x36}, 8},
  }};
  for (const Case &Test : Cases) {
    SCOPED_TRACE(Test.Name);
    constexpr uint64_t Old = 5;
    BinaryImage Image =
        memoryImage(Test.Width, SegmentFlags::Readable | SegmentFlags::Writable,
                    scalarBytes(Old, Test.Width));
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setRegister(x86reg::RSI, kMemoryAddress);

    const std::vector<LowOp> Ops = lift(Test.Bytes);
    ASSERT_EQ(Emulator.run(Ops), Ops.size());
    EXPECT_EQ(Emulator.getRegister(x86reg::RSI).value_or(UINT64_MAX), Old);
    ASSERT_TRUE(Emulator.step(loadMemory(Test.Width, 0x76400000)));
    EXPECT_EQ(Emulator.getRegister(0x76400000).value_or(UINT64_MAX),
              kMemoryAddress + Old);
  }
}

} // namespace
