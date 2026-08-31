//===- X86_64_APXStackTests.cpp - APX stack semantics -------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t kInstructionAddress = 0x1000;
constexpr uint64_t kStackBase = 0x7000;
constexpr uint64_t kInitialRsp = 0x7800;

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
  const int Decoded = Dec.decodeOneForLift(
      Bytes.data(), Bytes.size(), kInstructionAddress, Insn);
  if (Decoded != static_cast<int>(Bytes.size())) {
    ADD_FAILURE() << "failed to decode complete instruction";
    return {};
  }
  LiftedInstruction Result;
  Result.Id = Insn.Id;
  Dec.liftToLow(Insn, Result.Ops);
  return Result;
}

std::vector<uint8_t> encodePair(bool Push, bool Ppx, unsigned V,
                                unsigned B) {
  return {
      0x62,
      static_cast<uint8_t>(0x44 | 0x80 | 0x10 |
                           ((B & 8) != 0 ? 0 : 0x20) |
                           ((B & 16) != 0 ? 0x08 : 0)),
      static_cast<uint8_t>((Ppx ? 0x80 : 0) | (((~V) & 0xf) << 3) |
                           0x04),
      static_cast<uint8_t>(0x10 | ((V & 16) != 0 ? 0 : 0x08)),
      static_cast<uint8_t>(Push ? 0xff : 0x8f),
      static_cast<uint8_t>(0xc0 | (Push ? 0x30 : 0) | (B & 7)),
  };
}

std::optional<unsigned> decodeIdX64(const std::vector<uint8_t> &Bytes) {
  Decoder Dec;
  if (!Dec.init(Arch::X64))
    return std::nullopt;
  DecodedInsn Insn{};
  const int Decoded = Dec.decodeOneForLift(
      Bytes.data(), Bytes.size(), kInstructionAddress, Insn);
  if (Decoded != static_cast<int>(Bytes.size()))
    return std::nullopt;
  return Insn.Id;
}

BinaryImage makeStackImage() {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Stack;
  Stack.VA = kStackBase;
  Stack.Size = 0x1000;
  Stack.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Stack.Data.resize(Stack.Size);
  Image.Segments.push_back(std::move(Stack));
  return Image;
}

BinaryImage makeStackWindow(uint64_t Address, uint64_t Size,
                            SegmentFlags Flags = SegmentFlags::Readable |
                                                 SegmentFlags::Writable) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  Segment Stack;
  Stack.VA = Address;
  Stack.Size = Size;
  Stack.Flags = Flags;
  Stack.Data.resize(Stack.Size);
  Image.Segments.push_back(std::move(Stack));
  return Image;
}

void writeQword(BinaryImage &Image, uint64_t Address, uint64_t Value) {
  ASSERT_EQ(Image.Segments.size(), 1u);
  Segment &Stack = Image.Segments.front();
  ASSERT_GE(Address, Stack.VA);
  const uint64_t Offset = Address - Stack.VA;
  ASSERT_LE(Offset + sizeof(Value), Stack.Data.size());
  std::memcpy(Stack.Data.data() + Offset, &Value, sizeof(Value));
}

uint64_t probeQword(NdOpEmulator &Emulator, uint64_t Address) {
  constexpr uint64_t ProbeTemp = UINT64_C(0x70000000);
  LowOp Load;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::tmp(ProbeTemp, 8);
  Load.addInput(NdVar::cst(0, 8));
  Load.addInput(NdVar::cst(Address, 8));
  EXPECT_TRUE(Emulator.step(Load));
  return Emulator.getRegister(ProbeTemp).value_or(0);
}

bool hasOpcode(const std::vector<LowOp> &Ops, NdOp Opcode) {
  for (const LowOp &Op : Ops)
    if (Op.Opcode == Opcode)
      return true;
  return false;
}

unsigned countOpcode(const std::vector<LowOp> &Ops, NdOp Opcode) {
  unsigned Count = 0;
  for (const LowOp &Op : Ops)
    if (Op.Opcode == Opcode)
      ++Count;
  return Count;
}

bool hasIntrinsic(const std::vector<LowOp> &Ops, Intrinsic Id) {
  for (const LowOp &Op : Ops)
    if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs > 0 &&
        Op.Inputs[0].isConst() &&
        Op.Inputs[0].Offset == static_cast<uint64_t>(Id))
      return true;
  return false;
}

TEST(X86APXStack, PpxSingleRegisterStackOperationsAreExact) {
  constexpr uint64_t Value = UINT64_C(0x8877665544332211);
  const RegInfo R29 = mapCapstoneReg(X86_REG_R29);
  ASSERT_EQ(R29.Size, 8u);

  // REX2.W=1 is the PPX performance hint and still has ordinary PUSH/POP
  // functional semantics.  B4:B3 select the extended register bank.
  const LiftedInstruction Push = liftX64({0xd5, 0x19, 0x55});
  EXPECT_EQ(Push.Id, X86_INS_PUSHP);
  ASSERT_FALSE(Push.Ops.empty());
  BinaryImage PushImage = makeStackImage();
  NdOpEmulator PushEmulator(PushImage);
  PushEmulator.setStrictMode(true);
  PushEmulator.setRegister(x86reg::RSP, kInitialRsp);
  PushEmulator.setRegister(R29.Offset, Value);
  ASSERT_EQ(PushEmulator.run(Push.Ops), Push.Ops.size());
  EXPECT_EQ(PushEmulator.getRegister(x86reg::RSP), kInitialRsp - 8);
  EXPECT_EQ(probeQword(PushEmulator, kInitialRsp - 8), Value);
  EXPECT_FALSE(PushEmulator.skips().any());

  const LiftedInstruction Pop = liftX64({0xd5, 0x19, 0x5d});
  EXPECT_EQ(Pop.Id, X86_INS_POPP);
  ASSERT_FALSE(Pop.Ops.empty());
  BinaryImage PopImage = makeStackImage();
  writeQword(PopImage, kInitialRsp, Value);
  NdOpEmulator PopEmulator(PopImage);
  PopEmulator.setStrictMode(true);
  PopEmulator.setRegister(x86reg::RSP, kInitialRsp);
  PopEmulator.setRegister(R29.Offset, 0);
  ASSERT_EQ(PopEmulator.run(Pop.Ops), Pop.Ops.size());
  EXPECT_EQ(PopEmulator.getRegister(R29.Offset), Value);
  EXPECT_EQ(PopEmulator.getRegister(x86reg::RSP), kInitialRsp + 8);
  EXPECT_FALSE(PopEmulator.skips().any());
}

TEST(X86APXStack, PpxPopRspUsesArchitecturalWriteOrdering) {
  constexpr uint64_t LoadedRsp = UINT64_C(0x7123456789abcdef);
  const LiftedInstruction Pop = liftX64({0xd5, 0x08, 0x5c});
  EXPECT_EQ(Pop.Id, X86_INS_POPP);
  ASSERT_FALSE(Pop.Ops.empty());

  BinaryImage Image = makeStackImage();
  writeQword(Image, kInitialRsp, LoadedRsp);
  NdOpEmulator Emulator(Image);
  Emulator.setStrictMode(true);
  Emulator.setRegister(x86reg::RSP, kInitialRsp);
  ASSERT_EQ(Emulator.run(Pop.Ops), Pop.Ops.size());
  // POP increments the stack pointer before writing the explicit destination;
  // when that destination is RSP, the loaded value is therefore final.
  EXPECT_EQ(Emulator.getRegister(x86reg::RSP), LoadedRsp);
  EXPECT_FALSE(Emulator.skips().any());
}

TEST(X86APXStack, PairInstructionsUseExactStackSemantics) {
  constexpr uint64_t VValue = UINT64_C(0x8877665544332211);
  constexpr uint64_t BValue = UINT64_C(0x1020304050607080);
  const RegInfo VReg = mapCapstoneReg(X86_REG_R29);
  const RegInfo BReg = mapCapstoneReg(X86_REG_R20);
  ASSERT_EQ(VReg.Size, 8u);
  ASSERT_EQ(BReg.Size, 8u);

  for (const bool Ppx : {false, true}) {
    SCOPED_TRACE(Ppx ? "PPX" : "ordinary");
    const LiftedInstruction Push = liftX64(encodePair(true, Ppx, 29, 20));
    EXPECT_EQ(Push.Id, Ppx ? X86_INS_PUSH2P : X86_INS_PUSH2);
    ASSERT_FALSE(Push.Ops.empty());
    EXPECT_TRUE(hasIntrinsic(Push.Ops, Intrinsic::RequireAligned));
    EXPECT_TRUE(hasIntrinsic(Push.Ops, Intrinsic::MaskedStoreQ));
    EXPECT_FALSE(hasOpcode(Push.Ops, NdOp::LOAD));
    EXPECT_FALSE(hasOpcode(Push.Ops, NdOp::STORE));
    BinaryImage PushImage = makeStackImage();
    NdOpEmulator PushEmulator(PushImage);
    PushEmulator.setStrictMode(true);
    PushEmulator.setRegister(x86reg::RSP, kInitialRsp);
    PushEmulator.setRegister(VReg.Offset, VValue);
    PushEmulator.setRegister(BReg.Offset, BValue);
    ASSERT_EQ(PushEmulator.run(Push.Ops), Push.Ops.size());
    EXPECT_EQ(PushEmulator.getRegister(x86reg::RSP), kInitialRsp - 16);
    EXPECT_EQ(probeQword(PushEmulator, kInitialRsp - 8), VValue);
    EXPECT_EQ(probeQword(PushEmulator, kInitialRsp - 16), BValue);
    EXPECT_FALSE(PushEmulator.skips().any());

    const LiftedInstruction Pop = liftX64(encodePair(false, Ppx, 29, 20));
    EXPECT_EQ(Pop.Id, Ppx ? X86_INS_POP2P : X86_INS_POP2);
    ASSERT_FALSE(Pop.Ops.empty());
    EXPECT_TRUE(hasIntrinsic(Pop.Ops, Intrinsic::RequireAligned));
    EXPECT_FALSE(hasIntrinsic(Pop.Ops, Intrinsic::MaskedLoadQ));
    EXPECT_EQ(countOpcode(Pop.Ops, NdOp::LOAD), 2u);
    EXPECT_FALSE(hasOpcode(Pop.Ops, NdOp::STORE));
    BinaryImage PopImage = makeStackImage();
    writeQword(PopImage, kInitialRsp, VValue);
    writeQword(PopImage, kInitialRsp + 8, BValue);
    NdOpEmulator PopEmulator(PopImage);
    PopEmulator.setStrictMode(true);
    PopEmulator.setRegister(x86reg::RSP, kInitialRsp);
    PopEmulator.setRegister(VReg.Offset, 0);
    PopEmulator.setRegister(BReg.Offset, 0);
    ASSERT_EQ(PopEmulator.run(Pop.Ops), Pop.Ops.size());
    EXPECT_EQ(PopEmulator.getRegister(VReg.Offset), VValue);
    EXPECT_EQ(PopEmulator.getRegister(BReg.Offset), BValue);
    EXPECT_EQ(PopEmulator.getRegister(x86reg::RSP), kInitialRsp + 16);
    EXPECT_FALSE(PopEmulator.skips().any());
  }
}

TEST(X86APXStack, PairFaultProgressAndMisalignmentAreExact) {
  constexpr uint64_t VBefore = UINT64_C(0x1111111122222222);
  constexpr uint64_t BBefore = UINT64_C(0x3333333344444444);
  constexpr uint64_t VValue = UINT64_C(0xaaaaaaaa55555555);
  constexpr uint64_t BValue = UINT64_C(0xbbbbbbbb66666666);
  constexpr uint64_t Sentinel = UINT64_C(0x0123456789abcdef);
  const RegInfo VReg = mapCapstoneReg(X86_REG_R29);
  const RegInfo BReg = mapCapstoneReg(X86_REG_R20);

  for (const bool Ppx : {false, true}) {
    SCOPED_TRACE(Ppx ? "PPX" : "ordinary");
    const LiftedInstruction Push = liftX64(encodePair(true, Ppx, 29, 20));
    const LiftedInstruction Pop = liftX64(encodePair(false, Ppx, 29, 20));
    ASSERT_FALSE(Push.Ops.empty());
    ASSERT_FALSE(Pop.Ops.empty());

    // The lower qword is mapped, but the upper qword is not.  A pair store
    // must not expose the otherwise-valid lower write when either access
    // faults.
    BinaryImage PushFaultImage = makeStackWindow(kStackBase, 8);
    writeQword(PushFaultImage, kStackBase, Sentinel);
    NdOpEmulator PushFault(PushFaultImage);
    PushFault.setStrictMode(true);
    PushFault.setRegister(x86reg::RSP, kStackBase + 16);
    PushFault.setRegister(VReg.Offset, VValue);
    PushFault.setRegister(BReg.Offset, BValue);
    EXPECT_LT(PushFault.run(Push.Ops), Push.Ops.size());
    EXPECT_EQ(PushFault.getRegister(x86reg::RSP), kStackBase + 16);
    EXPECT_EQ(PushFault.getRegister(VReg.Offset), VValue);
    EXPECT_EQ(PushFault.getRegister(BReg.Offset), BValue);
    EXPECT_EQ(probeQword(PushFault, kStackBase), Sentinel);
    EXPECT_FALSE(PushFault.skips().any());

    BinaryImage PushDeniedImage =
        makeStackWindow(kStackBase, 16, SegmentFlags::Readable);
    writeQword(PushDeniedImage, kStackBase, Sentinel);
    writeQword(PushDeniedImage, kStackBase + 8, Sentinel);
    NdOpEmulator PushDenied(PushDeniedImage);
    PushDenied.setStrictMode(true);
    PushDenied.setRegister(x86reg::RSP, kStackBase + 16);
    PushDenied.setRegister(VReg.Offset, VValue);
    PushDenied.setRegister(BReg.Offset, BValue);
    EXPECT_LT(PushDenied.run(Push.Ops), Push.Ops.size());
    EXPECT_EQ(PushDenied.getRegister(x86reg::RSP), kStackBase + 16);
    EXPECT_EQ(probeQword(PushDenied, kStackBase), Sentinel);
    EXPECT_EQ(probeQword(PushDenied, kStackBase + 8), Sentinel);

    // POP2 is architecturally two sequential POP operations.  When the first
    // qword succeeds and the second faults, v and RSP+8 remain committed while
    // b retains its incoming value.
    BinaryImage PopFaultImage = makeStackWindow(kStackBase, 8);
    writeQword(PopFaultImage, kStackBase, VValue);
    NdOpEmulator PopFault(PopFaultImage);
    PopFault.setStrictMode(true);
    PopFault.setRegister(x86reg::RSP, kStackBase);
    PopFault.setRegister(VReg.Offset, VBefore);
    PopFault.setRegister(BReg.Offset, BBefore);
    EXPECT_LT(PopFault.run(Pop.Ops), Pop.Ops.size());
    EXPECT_EQ(PopFault.getRegister(x86reg::RSP), kStackBase + 8);
    EXPECT_EQ(PopFault.getRegister(VReg.Offset), VValue);
    EXPECT_EQ(PopFault.getRegister(BReg.Offset), BBefore);
    EXPECT_FALSE(PopFault.skips().any());

    BinaryImage PopDeniedImage =
        makeStackWindow(kStackBase, 16, SegmentFlags::Writable);
    writeQword(PopDeniedImage, kStackBase, VValue);
    writeQword(PopDeniedImage, kStackBase + 8, BValue);
    NdOpEmulator PopDenied(PopDeniedImage);
    PopDenied.setStrictMode(true);
    PopDenied.setRegister(x86reg::RSP, kStackBase);
    PopDenied.setRegister(VReg.Offset, VBefore);
    PopDenied.setRegister(BReg.Offset, BBefore);
    EXPECT_LT(PopDenied.run(Pop.Ops), Pop.Ops.size());
    EXPECT_EQ(PopDenied.getRegister(x86reg::RSP), kStackBase);
    EXPECT_EQ(PopDenied.getRegister(VReg.Offset), VBefore);
    EXPECT_EQ(PopDenied.getRegister(BReg.Offset), BBefore);

    // The architectural alignment check precedes every memory/register
    // effect, for both directions.
    BinaryImage MisalignedImage = makeStackImage();
    NdOpEmulator MisalignedPush(MisalignedImage);
    MisalignedPush.setStrictMode(true);
    MisalignedPush.setRegister(x86reg::RSP, kInitialRsp + 8);
    MisalignedPush.setRegister(VReg.Offset, VValue);
    MisalignedPush.setRegister(BReg.Offset, BValue);
    EXPECT_LT(MisalignedPush.run(Push.Ops), Push.Ops.size());
    EXPECT_EQ(MisalignedPush.getRegister(x86reg::RSP), kInitialRsp + 8);

    NdOpEmulator MisalignedPop(MisalignedImage);
    MisalignedPop.setStrictMode(true);
    MisalignedPop.setRegister(x86reg::RSP, kInitialRsp + 8);
    MisalignedPop.setRegister(VReg.Offset, VBefore);
    MisalignedPop.setRegister(BReg.Offset, BBefore);
    EXPECT_LT(MisalignedPop.run(Pop.Ops), Pop.Ops.size());
    EXPECT_EQ(MisalignedPop.getRegister(x86reg::RSP), kInitialRsp + 8);
    EXPECT_EQ(MisalignedPop.getRegister(VReg.Offset), VBefore);
    EXPECT_EQ(MisalignedPop.getRegister(BReg.Offset), BBefore);
  }
}

TEST(X86APXStack, PairRegisterBankAndArchitecturalConstraintsAreExact) {
  for (const bool Push : {false, true}) {
    for (const bool Ppx : {false, true}) {
      for (unsigned Reg = 0; Reg < 32; ++Reg) {
        if (Reg == 4)
          continue;
        const unsigned Partner = Reg == 31 ? 30 : 31;
        SCOPED_TRACE(::testing::Message()
                     << (Push ? "push" : "pop") << " ppx=" << Ppx
                     << " reg=" << Reg);
        const LiftedInstruction Pair =
            liftX64(encodePair(Push, Ppx, Reg, Partner));
        EXPECT_EQ(Pair.Id,
                  Push ? (Ppx ? X86_INS_PUSH2P : X86_INS_PUSH2)
                       : (Ppx ? X86_INS_POP2P : X86_INS_POP2));
        EXPECT_FALSE(Pair.Ops.empty());
      }
    }
  }

  for (const bool Ppx : {false, true}) {
    EXPECT_FALSE(decodeIdX64(encodePair(true, Ppx, 4, 20)).has_value());
    EXPECT_FALSE(decodeIdX64(encodePair(true, Ppx, 29, 4)).has_value());
    EXPECT_FALSE(decodeIdX64(encodePair(false, Ppx, 4, 20)).has_value());
    EXPECT_FALSE(decodeIdX64(encodePair(false, Ppx, 29, 4)).has_value());
    EXPECT_FALSE(decodeIdX64(encodePair(false, Ppx, 29, 29)).has_value());
    // PUSH2 may legally duplicate its source register.
    EXPECT_EQ(decodeIdX64(encodePair(true, Ppx, 29, 29)),
              Ppx ? X86_INS_PUSH2P : X86_INS_PUSH2);
  }
}

} // namespace
