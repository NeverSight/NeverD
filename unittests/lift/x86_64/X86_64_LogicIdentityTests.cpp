//===- X86_64_LogicIdentityTests.cpp - logical identity semantics --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86/X86Regs.h"

#include <bit>
#include <cstdint>
#include <vector>

using namespace neverd;

namespace {

void checkAndIdentity(Arch Target, unsigned Width) {
  for (unsigned RegisterIndex : {0u, 1u, 8u}) {
    if (Target == Arch::X86 && RegisterIndex == 8)
      continue;
    std::vector<uint8_t> Bytes;
    if (Width == 2)
      Bytes.push_back(0x66);
    if (Width == 8 || RegisterIndex == 8)
      Bytes.push_back(0x40 | (Width == 8 ? 8 : 0) |
                      (RegisterIndex == 8 ? 5 : 0));
    Bytes.push_back(Width == 1 ? 0x20 : 0x21);
    Bytes.push_back(0xc0 | ((RegisterIndex & 7) << 3) | (RegisterIndex & 7));

    Decoder Decode;
    ASSERT_TRUE(Decode.init(Target));
    Decode.setStrict(true);
    DecodedInsn Instruction{};
    ASSERT_EQ(Decode.decodeOneForLift(Bytes.data(), Bytes.size(), 0x1000,
                                      Instruction),
              static_cast<int>(Bytes.size()));
    ASSERT_EQ(Instruction.Id, X86_INS_AND);
    std::vector<LowOp> Ops;
    ASSERT_NO_THROW(Decode.liftToLow(Instruction, Ops));
    ASSERT_FALSE(Ops.empty());

    const uint64_t Mask =
        Width == 8 ? UINT64_MAX : (UINT64_C(1) << (Width * 8)) - 1;
    const uint64_t Sign = UINT64_C(1) << (Width * 8 - 1);
    const uint64_t FlagRegisters[] = {x86reg::CF, x86reg::PF, x86reg::AF,
                                      x86reg::ZF, x86reg::SF, x86reg::OF,
                                      x86reg::DF};
    for (uint64_t Input : {UINT64_C(0), UINT64_C(1), UINT64_C(0x80),
                           UINT64_C(0x8000), UINT64_C(0x80000000),
                           UINT64_C(0xffffffff), UINT64_C(0x1122334400000000),
                           UINT64_C(0x11223344ffffffff), UINT64_MAX}) {
      if (Target == Arch::X86)
        Input &= UINT32_MAX;
      for (unsigned Flags : {0u, 0x7fu, 0x15u, 0x2au}) {
        SCOPED_TRACE(::testing::Message()
                     << static_cast<unsigned>(Target) << '/' << Width << '/'
                     << RegisterIndex << '/' << Input << '/' << Flags);
        BinaryImage Image;
        Image.Arch = Target;
        Image.Bits = Target == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
        NdOpEmulator Emulator(Image);
        Emulator.setStrictMode(true);
        const uint64_t Register = RegisterIndex * 8;
        Emulator.setRegister(Register, Input);
        for (unsigned I = 0; I < 7; ++I)
          Emulator.setRegister(FlagRegisters[I], (Flags >> I) & 1);
        ASSERT_EQ(Emulator.run(Ops), Ops.size());
        ASSERT_FALSE(Emulator.skips().any());

        // AND preserves the selected bits, but every native 32-bit GPR
        // destination in long mode also clears the register's high half.
        const uint64_t Expected =
            Target == Arch::X64 && Width == 4 ? Input & UINT32_MAX : Input;
        EXPECT_EQ(Emulator.getRegister(Register), Expected);
        const uint64_t Result = Input & Mask;
        EXPECT_EQ(Emulator.getRegister(x86reg::CF), 0u);
        EXPECT_EQ(Emulator.getRegister(x86reg::OF), 0u);
        EXPECT_EQ(Emulator.getRegister(x86reg::ZF), Result == 0);
        EXPECT_EQ(Emulator.getRegister(x86reg::SF), (Result & Sign) != 0);
        EXPECT_EQ(Emulator.getRegister(x86reg::PF),
                  (std::popcount(static_cast<uint8_t>(Result)) & 1) == 0);
        EXPECT_EQ(Emulator.getRegister(x86reg::DF), (Flags >> 6) & 1);
        // AF is architecturally undefined; neither incoming AF value is an
        // oracle for the value selected by ordinary LowIR.
      }
    }
  }
}

} // namespace

TEST(X86LogicIdentity, AndSelfClearsTheHighHalfOfEvery32BitDestination) {
  checkAndIdentity(Arch::X64, 4);
}

TEST(X86LogicIdentity, AndSelfPreservesUnwrittenNarrowRegisterBits) {
  for (Arch Target : {Arch::X86, Arch::X64})
    for (unsigned Width : {1u, 2u})
      checkAndIdentity(Target, Width);
}

TEST(X86LogicIdentity, AndSelfPreservesTheFull64BitValue) {
  checkAndIdentity(Arch::X64, 8);
}

TEST(X86LogicIdentity, AndSelfPreservesThe32BitValueInProtectedMode) {
  checkAndIdentity(Arch::X86, 4);
}
