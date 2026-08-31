//===- X86_64_SystemSideeffectTests.cpp - system effect contracts -------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <optional>
#include <utility>
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
    ADD_FAILURE() << "failed to decode the complete system instruction";
    return {};
  }

  LiftedInstruction Result;
  Result.Id = Insn.Id;
  try {
    Dec.liftToLow(Insn, Result.Ops);
  } catch (const std::exception &Error) {
    ADD_FAILURE() << Error.what();
    Result.Ops.clear();
  }
  return Result;
}

const LowOp *findIntrinsic(const std::vector<LowOp> &Ops, Intrinsic Id) {
  for (const LowOp &Op : Ops)
    if (Op.Opcode == NdOp::INTRINSIC && Op.NumInputs != 0 &&
        Op.Inputs[0].isConst() &&
        Op.Inputs[0].Offset == static_cast<uint64_t>(Id))
      return &Op;
  return nullptr;
}

size_t indexOf(const std::vector<LowOp> &Ops, const LowOp *Needle) {
  return static_cast<size_t>(Needle - Ops.data());
}

void addSegment(BinaryImage &Image, uint64_t Address, std::vector<uint8_t> Data,
                SegmentFlags Flags) {
  Segment S;
  S.VA = Address;
  S.Size = Data.size();
  S.Flags = Flags;
  S.Data = std::move(Data);
  Image.Segments.push_back(std::move(S));
}

std::optional<std::vector<uint8_t>> readBytes(NdOpEmulator &Emulator,
                                              uint64_t Address, uint16_t Size) {
  LowOp Load;
  Load.Addr = kInstructionAddress + 1;
  Load.Opcode = NdOp::LOAD;
  Load.Output = NdVar::reg(x86reg::vectorReg(31), Size);
  Load.addInput(NdVar::cst(Address, 8));
  if (!Emulator.step(Load))
    return std::nullopt;
  return Emulator.getRegisterBytes(x86reg::vectorReg(31));
}

std::vector<uint8_t> pattern(uint8_t Seed) {
  std::vector<uint8_t> Bytes(64);
  for (size_t I = 0; I < Bytes.size(); ++I)
    Bytes[I] = static_cast<uint8_t>(Seed + I * 17);
  return Bytes;
}

TEST(X86SystemSideeffect, WrssAndWrussRetainAddressSourceWidthAndSegment) {
  struct Case {
    std::vector<uint8_t> Bytes;
    unsigned InstructionId;
    Intrinsic IntrinsicId;
    uint16_t Width;
    x86_reg Source;
    x86_reg Base;
    x86_reg Index;
  };
  const std::vector<Case> Cases = {
      {{0x64, 0x0f, 0x38, 0xf6, 0x44, 0x8b, 0x20},
       X86_INS_WRSSD,
       Intrinsic::CetWrss,
       4,
       X86_REG_EAX,
       X86_REG_RBX,
       X86_REG_RCX},
      {{0x64, 0x48, 0x0f, 0x38, 0xf6, 0x44, 0x8b, 0x20},
       X86_INS_WRSSQ,
       Intrinsic::CetWrss,
       8,
       X86_REG_RAX,
       X86_REG_RBX,
       X86_REG_RCX},
      {{0x64, 0x66, 0x0f, 0x38, 0xf5, 0x44, 0x8b, 0x20},
       X86_INS_WRUSSD,
       Intrinsic::CetWruss,
       4,
       X86_REG_EAX,
       X86_REG_RBX,
       X86_REG_RCX},
      {{0x64, 0x66, 0x48, 0x0f, 0x38, 0xf5, 0x44, 0x8b, 0x20},
       X86_INS_WRUSSQ,
       Intrinsic::CetWruss,
       8,
       X86_REG_RAX,
       X86_REG_RBX,
       X86_REG_RCX},
      {{0x64, 0x62, 0x0c, 0x7c, 0x08, 0x66, 0x54, 0xb5, 0x20},
       X86_INS_WRSSD,
       Intrinsic::CetWrss,
       4,
       X86_REG_R26D,
       X86_REG_R29,
       X86_REG_R14},
      {{0x64, 0x62, 0x0c, 0xfc, 0x08, 0x66, 0x54, 0xb5, 0x20},
       X86_INS_WRSSQ,
       Intrinsic::CetWrss,
       8,
       X86_REG_R26,
       X86_REG_R29,
       X86_REG_R14},
      {{0x64, 0x62, 0x0c, 0x7d, 0x08, 0x65, 0x54, 0xb5, 0x20},
       X86_INS_WRUSSD,
       Intrinsic::CetWruss,
       4,
       X86_REG_R26D,
       X86_REG_R29,
       X86_REG_R14},
      {{0x64, 0x62, 0x0c, 0xfd, 0x08, 0x65, 0x54, 0xb5, 0x20},
       X86_INS_WRUSSQ,
       Intrinsic::CetWruss,
       8,
       X86_REG_R26,
       X86_REG_R29,
       X86_REG_R14},
  };

  constexpr uint64_t FsBase = 0x4000;
  constexpr uint64_t TargetOffset = 0xa0;
  const std::vector<uint8_t> OldTarget(8, 0xa5);

  for (const Case &Current : Cases) {
    SCOPED_TRACE(Current.InstructionId);
    const LiftedInstruction Lifted = liftX64(Current.Bytes);
    ASSERT_EQ(Lifted.Id, Current.InstructionId);
    ASSERT_FALSE(Lifted.Ops.empty());
    const LowOp *Effect = findIntrinsic(Lifted.Ops, Current.IntrinsicId);
    ASSERT_NE(Effect, nullptr);
    EXPECT_EQ(Effect->Output.Size, 0u);
    ASSERT_EQ(Effect->NumInputs, 3u);
    EXPECT_EQ(Effect->MemoryAddressSpace, NdMemoryAddressSpace::X86FS);
    EXPECT_EQ(Effect->Inputs[1].Size, 8u);
    EXPECT_TRUE(Effect->Inputs[2].isReg());
    const RegInfo Source = mapCapstoneReg(Current.Source);
    const RegInfo Base = mapCapstoneReg(Current.Base);
    const RegInfo Index = mapCapstoneReg(Current.Index);
    EXPECT_EQ(Effect->Inputs[2].Offset, Source.Offset);
    EXPECT_EQ(Effect->Inputs[2].Size, Current.Width);
    EXPECT_EQ(
        std::count_if(Lifted.Ops.begin(), Lifted.Ops.end(),
                      [](const LowOp &Op) { return Op.Opcode == NdOp::STORE; }),
        0);
    EXPECT_EQ(findIntrinsic(Lifted.Ops, Intrinsic::RequireAligned), nullptr);

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    addSegment(Image, FsBase + TargetOffset, OldTarget,
               SegmentFlags::Readable | SegmentFlags::Writable);
    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                                   FsBase));
    Emulator.setRegister(Source.Offset, UINT64_C(0x8877665544332211));
    Emulator.setRegister(Base.Offset, 0x40);
    Emulator.setRegister(Index.Offset, 0x10);
    Emulator.setRegister(x86reg::CF, 1);
    Emulator.setRegister(x86reg::PF, 0);
    Emulator.setRegister(x86reg::AF, 1);
    Emulator.setRegister(x86reg::ZF, 0);
    Emulator.setRegister(x86reg::SF, 1);
    Emulator.setRegister(x86reg::OF, 0);
    EXPECT_EQ(Emulator.run(Lifted.Ops), indexOf(Lifted.Ops, Effect));
    EXPECT_EQ(Emulator.getRegister(Source.Offset),
              UINT64_C(0x8877665544332211));
    EXPECT_EQ(Emulator.getRegister(x86reg::CF), 1u);
    EXPECT_EQ(Emulator.getRegister(x86reg::PF), 0u);
    EXPECT_EQ(Emulator.getRegister(x86reg::AF), 1u);
    EXPECT_EQ(Emulator.getRegister(x86reg::ZF), 0u);
    EXPECT_EQ(Emulator.getRegister(x86reg::SF), 1u);
    EXPECT_EQ(Emulator.getRegister(x86reg::OF), 0u);
    EXPECT_EQ(readBytes(Emulator, FsBase + TargetOffset, 8), OldTarget);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

TEST(X86SystemSideeffect, EnqueueRetainsPortalCommandResultAndFaultBoundary) {
  struct Case {
    std::vector<uint8_t> Bytes;
    unsigned InstructionId;
    Intrinsic IntrinsicId;
    x86_reg Portal;
    x86_reg Base;
    x86_reg Index;
    bool Address32;
  };
  const std::vector<Case> Cases = {
      {{0x64, 0xf2, 0x0f, 0x38, 0xf8, 0x44, 0x8b, 0x20},
       X86_INS_ENQCMD,
       Intrinsic::Enqcmd,
       X86_REG_RAX,
       X86_REG_RBX,
       X86_REG_RCX,
       false},
      {{0x64, 0xf3, 0x0f, 0x38, 0xf8, 0x44, 0x8b, 0x20},
       X86_INS_ENQCMDS,
       Intrinsic::Enqcmds,
       X86_REG_RAX,
       X86_REG_RBX,
       X86_REG_RCX,
       false},
      {{0x67, 0x64, 0xf2, 0x0f, 0x38, 0xf8, 0x44, 0x8b, 0x20},
       X86_INS_ENQCMD,
       Intrinsic::Enqcmd,
       X86_REG_EAX,
       X86_REG_EBX,
       X86_REG_ECX,
       true},
      {{0x64, 0x62, 0x0c, 0x7f, 0x08, 0xf8, 0x54, 0xb5, 0x20},
       X86_INS_ENQCMD,
       Intrinsic::Enqcmd,
       X86_REG_R26,
       X86_REG_R29,
       X86_REG_R14,
       false},
      {{0x64, 0x62, 0x0c, 0x7e, 0x08, 0xf8, 0x54, 0xb5, 0x20},
       X86_INS_ENQCMDS,
       Intrinsic::Enqcmds,
       X86_REG_R26,
       X86_REG_R29,
       X86_REG_R14,
       false},
      {{0x67, 0x64, 0x62, 0x0c, 0x7f, 0x08, 0xf8, 0x54, 0xb5, 0x20},
       X86_INS_ENQCMD,
       Intrinsic::Enqcmd,
       X86_REG_R26D,
       X86_REG_R29D,
       X86_REG_R14D,
       true},
  };

  constexpr uint64_t FsBase = 0x4000;
  constexpr uint64_t SourceOffset = 0xa0;
  constexpr uint64_t PortalAddress = 0x8000;
  const std::vector<uint8_t> Command = pattern(0x31);
  const std::vector<uint8_t> OldPortal(64, 0xa5);

  for (const Case &Current : Cases) {
    SCOPED_TRACE(Current.InstructionId);
    const LiftedInstruction Lifted = liftX64(Current.Bytes);
    ASSERT_EQ(Lifted.Id, Current.InstructionId);
    ASSERT_FALSE(Lifted.Ops.empty());
    const LowOp *Effect = findIntrinsic(Lifted.Ops, Current.IntrinsicId);
    ASSERT_NE(Effect, nullptr);
    ASSERT_EQ(Effect->NumInputs, 3u);
    EXPECT_TRUE(Effect->Output.isReg());
    EXPECT_EQ(Effect->Output.Offset, x86reg::ZF);
    EXPECT_EQ(Effect->Output.Size, 1u);
    EXPECT_EQ(Effect->MemoryAddressSpace, NdMemoryAddressSpace::X86FS);
    EXPECT_EQ(Effect->Inputs[1].Size, 8u);
    const RegInfo Portal = mapCapstoneReg(Current.Portal);
    const RegInfo Base = mapCapstoneReg(Current.Base);
    const RegInfo Index = mapCapstoneReg(Current.Index);
    if (!Current.Address32) {
      EXPECT_TRUE(Effect->Inputs[2].isReg());
      EXPECT_EQ(Effect->Inputs[2].Offset, Portal.Offset);
    } else {
      EXPECT_TRUE(Effect->Inputs[2].isTemp());
    }
    EXPECT_EQ(Effect->Inputs[2].Size, 8u);
    EXPECT_EQ(
        std::count_if(Lifted.Ops.begin(), Lifted.Ops.end(),
                      [](const LowOp &Op) { return Op.Opcode == NdOp::LOAD; }),
        0);

    EXPECT_EQ(findIntrinsic(Lifted.Ops, Intrinsic::RequireAligned), nullptr);

    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    addSegment(Image, FsBase + SourceOffset, Command, SegmentFlags::Readable);
    addSegment(Image, PortalAddress, OldPortal,
               SegmentFlags::Readable | SegmentFlags::Writable);

    NdOpEmulator Emulator(Image);
    Emulator.setStrictMode(true);
    Emulator.setLoadCollect(true);
    ASSERT_TRUE(Emulator.setMemoryAddressSpaceBase(NdMemoryAddressSpace::X86FS,
                                                   FsBase));
    Emulator.setRegister(Portal.Offset, Current.Address32
                                            ? UINT64_C(0x100008000)
                                            : PortalAddress);
    Emulator.setRegister(Base.Offset,
                         Current.Address32 ? UINT64_C(0x100000040) : 0x40);
    Emulator.setRegister(Index.Offset,
                         Current.Address32 ? UINT64_C(0x100000010) : 0x10);
    Emulator.setRegister(x86reg::CF, 1);
    Emulator.setRegister(x86reg::PF, 0);
    Emulator.setRegister(x86reg::AF, 1);
    Emulator.setRegister(x86reg::ZF, 0);
    Emulator.setRegister(x86reg::SF, 1);
    Emulator.setRegister(x86reg::OF, 0);

    EXPECT_EQ(Emulator.run(Lifted.Ops), indexOf(Lifted.Ops, Effect));
    EXPECT_TRUE(Emulator.getLoadRecords().empty());
    EXPECT_EQ(Emulator.getRegister(Portal.Offset),
              Current.Address32 ? UINT64_C(0x100008000) : PortalAddress);
    EXPECT_EQ(Emulator.getRegister(x86reg::CF), 1u);
    EXPECT_EQ(Emulator.getRegister(x86reg::PF), 0u);
    EXPECT_EQ(Emulator.getRegister(x86reg::AF), 1u);
    EXPECT_EQ(Emulator.getRegister(x86reg::ZF), 0u);
    EXPECT_EQ(Emulator.getRegister(x86reg::SF), 1u);
    EXPECT_EQ(Emulator.getRegister(x86reg::OF), 0u);
    EXPECT_EQ(readBytes(Emulator, PortalAddress, 64), OldPortal);
    EXPECT_FALSE(Emulator.skips().any());
  }
}

} // namespace
