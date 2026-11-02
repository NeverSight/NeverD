//===- X64MemoryCallTargetTests.cpp - Explicit call target loads
//------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/symbolic/SymExec.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::symbolic;

namespace {

constexpr uint64_t TargetValue = UINT64_C(0x123456789abcdef0);

std::vector<LowOp> liftMemoryCall(std::initializer_list<uint8_t> Bytes,
                                  va_t Address = 0x4000) {
  Decoder Dec;
  if (!Dec.init(Arch::X64)) {
    ADD_FAILURE() << "x64 decoder initialization failed";
    return {};
  }
  Dec.setStrict(true);
  DecodedInsn Insn{};
  if (Dec.decodeOneForLift(Bytes.begin(), Bytes.size(), Address, Insn) !=
      static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "memory CALL did not decode completely";
    return {};
  }
  std::vector<LowOp> Ops;
  EXPECT_TRUE(Dec.liftX64MemoryCallToLow(Insn, Ops));
  return Ops;
}

void expectLoadedTarget(
    const std::vector<LowOp> &Ops, uint64_t ExpectedAddress,
    std::initializer_list<std::pair<uint64_t, uint64_t>> Registers = {}) {
  ASSERT_GE(Ops.size(), 2u);
  const LowOp &Call = Ops.back();
  ASSERT_EQ(Call.Opcode, NdOp::INDIR_CALL);
  ASSERT_EQ(Call.NumInputs, 1u);
  ASSERT_TRUE(Call.Inputs[0].isTemp());
  EXPECT_EQ(Call.Inputs[0].Size, 8u);
  EXPECT_EQ(Call.Output, NdVar::reg(x86reg::RAX, 8));

  SymContext Ctx;
  SymState State(Ctx);
  SymExec Exec(Ctx, State);
  for (const auto &[Offset, Value] : Registers)
    State.write(SymSpace::Register, Offset, Ctx.mkConst(64, Value));
  State.store(Ctx.mkConst(64, ExpectedAddress), Ctx.mkConst(64, TargetValue));
  unsigned Loads = 0;
  for (size_t I = 0; I + 1 < Ops.size(); ++I) {
    const LowOp &Op = Ops[I];
    SCOPED_TRACE(I);
    // Computing a CALL operand must not write the architectural register bank
    // or stack. The physical push belongs to the certified control expansion.
    ASSERT_TRUE(Op.Output.isTemp());
    EXPECT_EQ(Op.MemoryOrdering, NdMemoryOrdering::None);
    EXPECT_EQ(Op.MemoryAddressSpace, NdMemoryAddressSpace::Default);
    if (Op.Opcode == NdOp::LOAD) {
      ++Loads;
      ASSERT_EQ(Op.NumInputs, 1u);
      ASSERT_EQ(Op.Output.Size, 8u);
      const auto Address = Ctx.asConst(Exec.operandValue(Op.Inputs[0]));
      ASSERT_TRUE(Address.has_value());
      EXPECT_EQ(Address->getZExtValue(), ExpectedAddress);
      EXPECT_EQ(Op.Output, Call.Inputs[0]);
    }
    ASSERT_EQ(Exec.step(Op), StepResult::Continue);
  }
  EXPECT_EQ(Loads, 1u);
  const auto Target = Ctx.asConst(Exec.operandValue(Call.Inputs[0]));
  ASSERT_TRUE(Target.has_value());
  EXPECT_EQ(Target->getZExtValue(), TargetValue);
  EXPECT_EQ(Exec.unmodelledCount(), 0u);
  for (const auto &[Offset, Value] : Registers) {
    const auto After = Ctx.asConst(State.read(SymSpace::Register, Offset, 8));
    ASSERT_TRUE(After.has_value());
    EXPECT_EQ(After->getZExtValue(), Value);
  }
}

TEST(X64MemoryCallTarget, ExplicitModeLoadsRipSlotWithoutChangingDefaultLift) {
  const std::vector<uint8_t> Bytes = {0xff, 0x15, 0x10, 0, 0, 0};
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x4000, Insn),
            static_cast<int>(Bytes.size()));

  auto ExpectSlot = [&](const std::vector<LowOp> &Ops) {
    ASSERT_EQ(Ops.size(), 1u);
    EXPECT_EQ(Ops.front().Opcode, NdOp::INDIR_CALL);
    ASSERT_EQ(Ops.front().NumInputs, 1u);
    ASSERT_TRUE(Ops.front().Inputs[0].isConst());
    EXPECT_EQ(Ops.front().Inputs[0].Offset, 0x4016u);
  };
  std::vector<LowOp> Before;
  Dec.liftToLow(Insn, Before);
  ExpectSlot(Before);
  std::vector<LowOp> Explicit;
  ASSERT_TRUE(Dec.liftX64MemoryCallToLow(Insn, Explicit));
  expectLoadedTarget(Explicit, 0x4016);
  std::vector<LowOp> After;
  Dec.liftToLow(Insn, After);
  ExpectSlot(After);
}

TEST(X64MemoryCallTarget, RipDisplacementUsesInstructionEnd) {
  expectLoadedTarget(liftMemoryCall({0xff, 0x15, 0xf0, 0xff, 0xff, 0xff}),
                     0x3ff6);
}

TEST(X64MemoryCallTarget, IndexedOperandRetainsScaleAndExtendedRegisters) {
  // call qword ptr [r9 + r10 * 4 + 24]
  expectLoadedTarget(liftMemoryCall({0x43, 0xff, 0x54, 0x91, 0x18}), 0x9024,
                     {{x86reg::R9, 0x9000}, {x86reg::R10, 3}});
  // A redundant REX.W still names an ordinary 64-bit near call.
  expectLoadedTarget(liftMemoryCall({0x48, 0xff, 0x50, 0x08}), 0x9008,
                     {{x86reg::RAX, 0x9000}});
}

TEST(X64MemoryCallTarget, StackAddressIsReadBeforeAnyPhysicalPush) {
  expectLoadedTarget(liftMemoryCall({0xff, 0x14, 0x24}), 0x9000,
                     {{x86reg::RSP, 0x9000}});
  // The return address will overwrite this very slot after the target load.
  expectLoadedTarget(liftMemoryCall({0xff, 0x54, 0x24, 0xf8}), 0x8ff8,
                     {{x86reg::RSP, 0x9000}});
}

TEST(X64MemoryCallTarget, Address32WrapsIndexAndDisplacementBeforeExtension) {
  // call qword ptr [esp + ecx * 4 + 16], with stale high register halves.
  expectLoadedTarget(liftMemoryCall({0x67, 0xff, 0x54, 0x8c, 0x10}), 0x10,
                     {{x86reg::RSP, UINT64_C(0xaaaa0000fffffff0)},
                      {x86reg::RCX, UINT64_C(0xbbbb000000000004)}});
}

TEST(X64MemoryCallTarget, Address32EipRelativeWrapsAtInstructionEnd) {
  expectLoadedTarget(liftMemoryCall({0x67, 0xff, 0x15, 0x10, 0, 0, 0},
                                    UINT64_C(0x1234fffffff8)),
                     0xf);
}

TEST(X64MemoryCallTarget, AbsoluteNegativeDisplacementKeepsAddressWidth) {
  // No-base SIB disp32 is sign-extended with addr64, and zero-extended only
  // after the modulo-32 effective-address calculation with addr32.
  expectLoadedTarget(liftMemoryCall({0xff, 0x14, 0x25, 0xf0, 0xff, 0xff, 0xff}),
                     UINT64_C(0xfffffffffffffff0));
  expectLoadedTarget(
      liftMemoryCall({0x67, 0xff, 0x14, 0x25, 0xf0, 0xff, 0xff, 0xff}),
      UINT64_C(0xfffffff0));
}

TEST(X64MemoryCallTarget, UnsupportedPrefixesAndFarCallsCannotAcquireContract) {
  const std::vector<std::pair<const char *, std::vector<uint8_t>>> Cases = {
      {"fs", {0x64, 0xff, 0x10}},
      {"gs", {0x65, 0xff, 0x10}},
      {"operand size", {0x66, 0xff, 0x10}},
      {"bnd", {0xf2, 0xff, 0x10}},
      {"rep", {0xf3, 0xff, 0x10}},
      {"lock", {0xf0, 0xff, 0x10}},
      {"notrack", {0x3e, 0xff, 0x10}},
      {"segment", {0x2e, 0xff, 0x10}},
      {"duplicate address prefix", {0x67, 0x67, 0xff, 0x10}},
      {"duplicate rex", {0x48, 0x48, 0xff, 0x10}},
      {"legacy prefix after rex", {0x48, 0x67, 0xff, 0x10}},
      {"far memory call", {0xff, 0x18}},
      {"far memory call with rex", {0x48, 0xff, 0x18}},
      {"register call", {0xff, 0xd0}},
      {"direct call", {0xe8, 0x10, 0, 0, 0}},
      {"memory jump", {0xff, 0x20}},
  };
  for (const auto &[Name, Bytes] : Cases) {
    SCOPED_TRACE(Name);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X64));
    Dec.setStrict(true);
    DecodedInsn Insn{};
    const int Size =
        Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x4000, Insn);
    // Invalid encodings may already be refused by the decoder. A decoded
    // prefix-normalized CALL must still fail the narrower loaded-target API.
    if (!Size)
      continue;
    ASSERT_EQ(Size, static_cast<int>(Bytes.size()));
    std::vector<LowOp> Ops;
    EXPECT_FALSE(Dec.liftX64MemoryCallToLow(Insn, Ops));
    EXPECT_TRUE(Ops.empty());
  }
}

TEST(X64MemoryCallTarget, NonX64AndMissingInstructionDoNotAppendOperations) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  const std::vector<uint8_t> Bytes = {0xff, 0x10};
  DecodedInsn Insn{};
  ASSERT_EQ(Dec.decodeOneForLift(Bytes.data(), Bytes.size(), 0x4000, Insn),
            static_cast<int>(Bytes.size()));
  LowOp Sentinel;
  Sentinel.Opcode = NdOp::NOP;
  Sentinel.Addr = 0x1234;
  std::vector<LowOp> Ops{Sentinel};
  EXPECT_FALSE(Dec.liftX64MemoryCallToLow(Insn, Ops));
  ASSERT_EQ(Ops.size(), 1u);
  EXPECT_EQ(Ops.front().Opcode, NdOp::NOP);
  EXPECT_EQ(Ops.front().Addr, Sentinel.Addr);

  ASSERT_TRUE(Dec.init(Arch::X64));
  EXPECT_FALSE(Dec.liftX64MemoryCallToLow(DecodedInsn{}, Ops));
  ASSERT_EQ(Ops.size(), 1u);
  EXPECT_EQ(Ops.front().Addr, Sentinel.Addr);
}

} // namespace
