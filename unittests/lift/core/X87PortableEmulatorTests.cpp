//===- X87PortableEmulatorTests.cpp - Portable x87 LowIR integration ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Decode original x86 instructions through the existing owned-state lifter,
/// then execute the explicit portable slice without any host x87 requirement.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/X87Binary80.h"
#include "neverd/ir/X87Context.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>

using namespace neverd;

namespace {
constexpr uint64_t Entry = 0x1000, DataAddress = 0x3000;
constexpr uint64_t IntegerBit = UINT64_C(1) << 63;

BinaryImage image() {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.X87ContextOverride = true;
  Segment Data;
  Data.VA = DataAddress;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.assign(256, 0);
  Data.Size = Data.FileSz = Data.Data.size();
  auto One = X87Binary80{IntegerBit, 0x3fff}.bytes();
  auto Tiny = X87Binary80{IntegerBit, 0x3fc0}.bytes(); // 2^-63.
  std::copy(One.begin(), One.end(), Data.Data.begin());
  std::copy(Tiny.begin(), Tiny.end(), Data.Data.begin() + 16);
  Img.Segments.push_back(std::move(Data));
  return Img;
}

bool instruction(NdOpEmulator &E, std::span<const uint8_t> Code,
                 uint64_t IP = Entry) {
  Decoder D;
  if (!D.init(Arch::X64))
    return false;
  D.setX87ContextOwnership(true);
  DecodedInsn I;
  if (D.decodeOneForLift(Code.data(), Code.size(), IP, I) != Code.size())
    return false;
  std::vector<LowOp> Ops;
  D.liftToLow(I, Ops);
  for (const auto &Op : Ops)
    if (!E.step(Op))
      return false;
  return true;
}

TEST(X87PortableEmulator, LiftedInstructionsRetainBeyondBinary64Precision) {
  auto Img = image();
  for (bool Strict : {false, true}) {
    NdOpEmulator E(Img);
    E.setStrictMode(Strict);
    E.setX87PortableMode(true);
    ASSERT_TRUE(E.setX86LinearAddressBits(48));
    E.setRegister(x86reg::RAX, DataAddress);
    EXPECT_FALSE(instruction(E, std::array<uint8_t, 2>{0xdb, 0x28}));
    ASSERT_TRUE(instruction(E, std::array<uint8_t, 2>{0xdb, 0xe3})); // FNINIT
    ASSERT_TRUE(instruction(E, std::array<uint8_t, 2>{0xdb, 0x28})); // FLD m80
    E.setRegister(x86reg::RAX, DataAddress + 16);
    ASSERT_TRUE(instruction(E, std::array<uint8_t, 2>{0xdb, 0x28}));
    ASSERT_TRUE(instruction(E, std::array<uint8_t, 2>{0xde, 0xc1})); // FADDP
    const auto Expected = X87Binary80{IntegerBit + 1, 0x3fff}.bytes();
    EXPECT_EQ(E.getRegisterBytes(x86reg::ST7),
              std::vector<uint8_t>(Expected.begin(), Expected.end()));
    EXPECT_EQ(E.getRegister(x86reg::FPU_SW), 0x3800);
    EXPECT_FALSE(E.getX87Context()); // Six retained empty payloads are unknown.
    E.setRegister(x86reg::RAX, DataAddress + 32);
    ASSERT_TRUE(instruction(E, std::array<uint8_t, 2>{0xdb, 0x38})); // FSTP m80
    LowOp Load;
    Load.Opcode = NdOp::LOAD;
    Load.Output = NdVar::tmp(90001, 16);
    Load.addInput(NdVar::cst(DataAddress + 32, 8));
    ASSERT_TRUE(E.step(Load));
    std::vector<uint8_t> ExpectedMemory(16, 0);
    std::copy(Expected.begin(), Expected.end(), ExpectedMemory.begin());
    EXPECT_EQ(E.getRegisterBytes(90001), ExpectedMemory);
    E.reset();
    EXPECT_FALSE(instruction(E, std::array<uint8_t, 2>{0xdb, 0x28}));
    ASSERT_TRUE(instruction(E, std::array<uint8_t, 2>{0xdb, 0xe3}));
    // Reset retains backend choice, but cannot invent operand memory or state.
    EXPECT_FALSE(instruction(E, std::array<uint8_t, 2>{0xdb, 0x28}));
  }
}

TEST(X87PortableEmulator, UnsupportedMemoryAndExternalStateFailClosed) {
  auto Img = image();
  for (bool Strict : {false, true}) {
    NdOpEmulator E(Img);
    E.setStrictMode(Strict);
    E.setX87PortableMode(true);
    ASSERT_TRUE(E.setX86LinearAddressBits(48));
    std::array<uint8_t, 512> State{};
    State[0] = 0x7f;
    State[1] = 3;
    ASSERT_TRUE(E.setX87Context(State));
    E.setRegister(x86reg::RAX, 0xdead0000);
    EXPECT_FALSE(instruction(E, std::array<uint8_t, 2>{0xdb, 0x28}));
    EXPECT_EQ(E.getX87Context(), State);
    EXPECT_FALSE(instruction(E, std::array<uint8_t, 2>{0xd9, 0xfe})); // FSIN
    EXPECT_EQ(E.getX87Context(), State);
    E.setRegister(x86reg::RAX, DataAddress);
    EXPECT_FALSE(instruction(E, std::array<uint8_t, 2>{0xdf, 0x20})); // FBLD
    EXPECT_EQ(E.getX87Context(), State);
    EXPECT_FALSE(instruction(E, std::array<uint8_t, 2>{0xdd, 0x30})); // FNSAVE
    EXPECT_EQ(E.getX87Context(), State);
    LowOp Restore;
    Restore.Opcode = NdOp::INTRINSIC;
    Restore.addInput(NdVar::cst(unsigned(Intrinsic::Fxrstor64Mem), 2));
    Restore.addInput(NdVar::cst(DataAddress, 8));
    EXPECT_FALSE(E.step(Restore));
    EXPECT_EQ(E.getX87Context(), State);
    LowOp MMX;
    MMX.Opcode = NdOp::INTRINSIC;
    MMX.addInput(NdVar::cst(unsigned(Intrinsic::X87ContextMMXEnter), 2));
    MMX.addInput(NdVar::reg(x86reg::X87ContextHandle, 8));
    EXPECT_FALSE(E.step(MMX));
    EXPECT_EQ(E.getX87Context(), State);
    // A caller or external-state import cannot preserve the earlier context
    // merely because the backend knows finite arithmetic.
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.addInput(NdVar::cst(Entry + 128, 8));
    EXPECT_FALSE(E.step(Call));
    E.setRegister(x86reg::FPU_SW, 0);
    EXPECT_FALSE(E.getX87Context());
    EXPECT_FALSE(instruction(E, std::array<uint8_t, 2>{0xdb, 0x28}));
  }
}
} // namespace
