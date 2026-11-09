//===- MIPS16FrameTests.cpp - Ordered MIPS16 stack frames ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Manual-derived register lists and memory images qualify frame fields and
/// partial faults independently of the production normalization.

#include "gtest/gtest.h"

#include "neverd/ArchSupport.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/NdOpEmulator.h"
#include "neverd/lift/MIPS/MIPSLifter.h"
#include "neverd/lift/MIPS/MIPSRegs.h"
#ifdef NEVERD_MIPS_UNICORN_REFERENCE
#include "unicorn/unicorn.h"
#endif
#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

using namespace neverd;
namespace {
constexpr unsigned Args[] = {0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 0, 3, 3, 4};
constexpr unsigned Statics[] = {0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 4, 0, 1, 0};
constexpr unsigned Saved[] = {18, 19, 20, 21, 22, 23, 30};
struct Frame {
  bool Extended;
  unsigned Flags, Size, Xs, Ar;
};
std::vector<uint8_t> frameCode(Frame F, Arch Target) {
  std::vector<uint8_t> Code;
  const auto Half = [&](unsigned V) {
    Code.push_back(V >> (archLittleEndian(Target) ? 0 : 8));
    Code.push_back(V >> (archLittleEndian(Target) ? 8 : 0));
  };
  if (F.Extended)
    Half(0xf000 | (F.Xs << 8) | (F.Size & 0xf0) | F.Ar);
  Half(0x6400 | (F.Flags << 4) | (F.Size & 15));
  return Code;
}
unsigned frameBytes(Frame F) {
  return !F.Extended && !F.Size ? 128 : F.Size * 8;
}
std::vector<std::pair<unsigned, int>> accesses(Frame F) {
  std::vector<std::pair<unsigned, int>> R;
  if (F.Extended && (F.Flags & 8))
    for (unsigned I = 0; I < Args[F.Ar]; ++I)
      R.emplace_back(4 + I, I * 4);
  int Offset = 0;
  const auto Add = [&](unsigned Reg) { R.emplace_back(Reg, Offset -= 4); };
  if (F.Flags & 4)
    Add(31);
  if (F.Extended)
    for (unsigned I = F.Xs; I; --I)
      Add(Saved[I - 1]);
  if (F.Flags & 1)
    Add(17);
  if (F.Flags & 2)
    Add(16);
  if (F.Extended)
    for (unsigned I = 0; I < Statics[F.Ar]; ++I)
      Add(7 - I);
  return R;
}
uint32_t initial(unsigned R) { return 0xa5000000u | (R * 0x010203); }
uint32_t loaded(unsigned R) { return 0x87543210u ^ (R * 0x030405); }
BinaryImage stackImage(Arch Target, Frame F, uint32_t Top, size_t Count) {
  BinaryImage Image;
  Image.Arch = Target;
  Image.Bits = Bitness::Bits32;
  const auto Accesses = accesses(F);
  for (unsigned I = 0; I < Count; ++I) {
    const auto [Reg, Offset] = Accesses[I];
    Segment S;
    S.VA = uint32_t(Top + Offset);
    S.Size = S.FileSz = 4;
    S.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    for (unsigned Byte = 0; Byte < 4; ++Byte)
      S.Data.push_back((F.Flags & 8 ? 0x55555555u : loaded(Reg)) >>
                       (8 * (archLittleEndian(Target) ? Byte : 3 - Byte)));
    Image.Segments.push_back(std::move(S));
  }
  return Image;
}
std::vector<LowOp> frameOps(Decoder &Decode, Arch Target, Frame F) {
  const auto Code = frameCode(F, Target);
  DecodedInsn Insn;
  EXPECT_EQ(Decode.decodeOneForLift(Code.data(), Code.size(), 0x10002, Insn),
            int(Code.size()));
  std::vector<LowOp> Ops;
  Decode.liftToLow(Insn, Ops);
  return Ops;
}
class MIPS16Frame : public ::testing::TestWithParam<Arch> {};
TEST_P(MIPS16Frame, AllRegisterFieldsAndFrameBoundariesHaveExactEffects) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  std::vector<Frame> Cases;
  for (unsigned Flags = 0; Flags < 16; ++Flags) {
    for (unsigned Size = 0; Size < 16; ++Size)
      Cases.push_back({false, Flags, Size, 0, 0});
    for (unsigned Xs = 0; Xs < 8; ++Xs)
      for (unsigned Ar = 0; Ar < 15; ++Ar)
        for (unsigned Size : {0u, 16u, 255u})
          Cases.push_back({true, Flags, Size, Xs, Ar});
  }
  for (Frame F : Cases) {
    SCOPED_TRACE(::testing::Message() << F.Extended << ':' << F.Flags << ':'
                                      << F.Size << ':' << F.Xs << ':' << F.Ar);
    const auto Accesses = accesses(F);
    auto Image = stackImage(GetParam(), F, 0x3000, Accesses.size());
    NdOpEmulator E(Image);
    E.setStrictMode(true);
    for (unsigned R = 1; R < 32; ++R)
      E.setRegister(mipsreg::r(R), initial(R));
    const uint32_t SP = F.Flags & 8 ? 0x3000 : 0x3000 - frameBytes(F);
    E.setRegister(mipsreg::SP, SP);
    const auto Ops = frameOps(Decode, GetParam(), F);
    for (const auto &Op : Ops)
      ASSERT_TRUE(E.step(Op));
    EXPECT_EQ(E.getRegister(mipsreg::SP),
              F.Flags & 8 ? 0x3000 - frameBytes(F) : 0x3000u);
    for (unsigned R = 1; R < 32; ++R) {
      if (R == 29)
        continue;
      bool Restored = false;
      for (auto [Reg, Offset] : Accesses)
        Restored |= Reg == R && !(F.Flags & 8);
      EXPECT_EQ(E.getRegister(mipsreg::r(R)), Restored ? loaded(R) : initial(R))
          << R;
    }
    for (auto [Reg, Offset] : Accesses) {
      LowOp Read;
      Read.Opcode = NdOp::LOAD;
      Read.Output = NdVar::reg(mipsreg::V0, 4);
      Read.addInput(NdVar::cst(uint32_t(0x3000 + Offset), 4));
      ASSERT_TRUE(E.step(Read));
      EXPECT_EQ(E.getRegister(mipsreg::V0),
                F.Flags & 8 ? initial(Reg) : loaded(Reg));
    }
  }
}
TEST_P(MIPS16Frame, EveryFaultRetainsPriorEffectsAndTheOriginalStackPointer) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  for (bool Save : {false, true}) {
    Frame F{true, Save ? 15u : 7u, 16, 7, 7};
    const auto A = accesses(F);
    const auto Ops = frameOps(Decode, GetParam(), F);
    for (size_t Count = 0; Count < A.size(); ++Count) {
      auto Image = stackImage(GetParam(), F, 0x3000, Count);
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      for (unsigned R = 1; R < 32; ++R)
        E.setRegister(mipsreg::r(R), initial(R));
      const uint32_t SP = Save ? 0x3000 : 0x3000 - frameBytes(F);
      E.setRegister(mipsreg::SP, SP);
      bool Fault = false;
      for (const auto &Op : Ops)
        if (!E.step(Op)) {
          Fault = true;
          break;
        }
      ASSERT_TRUE(Fault) << Count;
      EXPECT_EQ(E.getRegister(mipsreg::SP), SP);
      for (size_t I = 0; I < A.size(); ++I) {
        auto [Reg, Offset] = A[I];
        EXPECT_EQ(E.getRegister(mipsreg::r(Reg)),
                  !Save && I < Count ? loaded(Reg) : initial(Reg));
        if (Save && I < Count) {
          LowOp Read;
          Read.Opcode = NdOp::LOAD;
          Read.Output = NdVar::reg(mipsreg::V0, 4);
          Read.addInput(NdVar::cst(uint32_t(0x3000 + Offset), 4));
          ASSERT_TRUE(E.step(Read));
          EXPECT_EQ(E.getRegister(mipsreg::V0), initial(Reg));
        }
      }
    }
  }
}
TEST_P(MIPS16Frame, AlignmentIsRequiredOnlyForAccessesAndArithmeticWraps) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  for (bool Save : {false, true})
    for (bool HasAccess : {false, true}) {
      Frame F{false, (Save ? 8u : 0u) | (HasAccess ? 4u : 0u), 0, 0, 0};
      BinaryImage Image;
      Image.Arch = GetParam();
      Image.Bits = Bitness::Bits32;
      NdOpEmulator E(Image);
      E.setStrictMode(true);
      E.setRegister(mipsreg::SP, 0xfffffff9);
      E.setRegister(mipsreg::RA, 123);
      bool Fault = false;
      for (auto &Op : frameOps(Decode, GetParam(), F))
        if (!E.step(Op)) {
          Fault = true;
          break;
        }
      EXPECT_EQ(Fault, HasAccess);
      EXPECT_EQ(E.getRegister(mipsreg::SP),
                HasAccess ? 0xfffffff9u
                          : uint32_t(0xfffffff9u + (Save ? -128 : 128)));
    }
  Frame F{true, 15, 0, 7, 11};
  const auto A = accesses(F);
  auto Image = stackImage(GetParam(), F, 8, A.size());
  NdOpEmulator E(Image);
  E.setStrictMode(true);
  for (unsigned R = 1; R < 32; ++R)
    E.setRegister(mipsreg::r(R), initial(R));
  E.setRegister(mipsreg::SP, 8);
  for (const auto &Op : frameOps(Decode, GetParam(), F))
    ASSERT_TRUE(E.step(Op));
  EXPECT_EQ(E.getRegister(mipsreg::SP), 8u);
}
TEST_P(MIPS16Frame, ReservedArgumentFieldsFailAndFramesAreNeverPadding) {
  Decoder Decode;
  ASSERT_TRUE(Decode.init(GetParam(), InstructionMode::MIPS16));
  MIPSLifter Lift(GetParam(), InstructionMode::MIPS16);
  for (bool Save : {false, true})
    for (unsigned Xs = 0; Xs < 8; ++Xs) {
      auto Code = frameCode({true, Save ? 15u : 7u, 255, Xs, 15}, GetParam());
      DecodedInsn I;
      EXPECT_EQ(Decode.decodeOneForLift(Code.data(), Code.size(), 0x1000, I),
                0);
      cs_insn Raw{};
      Raw.address = 0x1000;
      Raw.size = 4;
      std::copy(Code.begin(), Code.end(), Raw.bytes);
      std::vector<LowOp> Ops;
      EXPECT_THROW(Lift.lift(Raw, Ops), UnliftedInstruction);
      EXPECT_TRUE(Ops.empty());
    }
  for (Frame F : {Frame{false, 0, 0, 0, 0}, Frame{true, 8, 0, 0, 0}}) {
    auto Code = frameCode(F, GetParam());
    cs_insn I{};
    I.size = Code.size();
    std::copy(Code.begin(), Code.end(), I.bytes);
    EXPECT_FALSE(Lift.isAlignmentNop(I));
    EXPECT_FALSE(Lift.isReturn(I));
    EXPECT_FALSE(Lift.isFunctionTerminator(I));
    EXPECT_EQ(Lift.directCallTarget(I), InvalidVA);
    EXPECT_EQ(Lift.delaySlotBytes(I), 0u);
  }
}
TEST_P(MIPS16Frame, FramesMatchPinnedTargetExecution) {
#ifndef NEVERD_MIPS_UNICORN_REFERENCE
  GTEST_SKIP() << "The pinned Unicorn reference was not built";
#else
  uc_engine *Raw = nullptr;
  ASSERT_EQ(uc_open(UC_ARCH_MIPS,
                    static_cast<uc_mode>(UC_MODE_MIPS32 |
                                         (archLittleEndian(GetParam())
                                              ? UC_MODE_LITTLE_ENDIAN
                                              : UC_MODE_BIG_ENDIAN)),
                    &Raw),
            UC_ERR_OK);
  std::unique_ptr<uc_engine, decltype(&uc_close)> E(Raw, uc_close);
  ASSERT_EQ(uc_ctl_set_cpu_model(Raw, UC_CPU_MIPS32_24KF), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x10000, 0x10000, UC_PROT_ALL), UC_ERR_OK);
  ASSERT_EQ(uc_mem_map(Raw, 0x2000, 0x2000, UC_PROT_ALL), UC_ERR_OK);
  unsigned Index = 0;
  for (bool Save : {false, true})
    for (unsigned Xs = 0; Xs < 8; ++Xs)
      for (unsigned Ar = 0; Ar < 15; ++Ar)
        for (unsigned Size : {0u, 255u}) {
          Frame F{true, Save ? 15u : 7u, Size, Xs, Ar};
          auto Image = stackImage(GetParam(), F, 0x3000, accesses(F).size());
          for (const auto &S : Image.Segments)
            ASSERT_EQ(uc_mem_write(Raw, S.VA, S.Data.data(), S.Size),
                      UC_ERR_OK);
          uint32_t SP = Save ? 0x3000 : 0x3000 - frameBytes(F);
          for (unsigned R = 1; R < 32; ++R) {
            uint32_t V = R == 29 ? SP : initial(R);
            ASSERT_EQ(uc_reg_write(Raw, UC_MIPS_REG_0 + R, &V), UC_ERR_OK);
          }
          auto Code = frameCode(F, GetParam());
          uint64_t PC = 0x10002 + 8 * Index++;
          ASSERT_EQ(uc_mem_write(Raw, PC, Code.data(), Code.size()), UC_ERR_OK);
          ASSERT_EQ(uc_emu_start(Raw, PC | 1, PC + Code.size(), 0, 1),
                    UC_ERR_OK)
              << Xs << ':' << Ar << ':' << Size;
          uint32_t Value = 0;
          ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_SP, &Value), UC_ERR_OK);
          EXPECT_EQ(Value, Save ? 0x3000 - frameBytes(F) : 0x3000u);
          for (auto [Reg, Offset] : accesses(F)) {
            if (Save) {
              std::array<uint8_t, 4> Memory{};
              ASSERT_EQ(
                  uc_mem_read(Raw, uint32_t(0x3000 + Offset), Memory.data(), 4),
                  UC_ERR_OK);
              for (unsigned I = 0; I < 4; ++I)
                EXPECT_EQ(
                    Memory[I],
                    uint8_t(initial(Reg) >>
                            (8 * (archLittleEndian(GetParam()) ? I : 3 - I))));
            } else {
              ASSERT_EQ(uc_reg_read(Raw, UC_MIPS_REG_0 + Reg, &Value),
                        UC_ERR_OK);
              EXPECT_EQ(Value, loaded(Reg));
            }
          }
        }
#endif
}
INSTANTIATE_TEST_SUITE_P(ByteOrders, MIPS16Frame,
                         ::testing::Values(Arch::MIPS32, Arch::MIPS32EL));
} // namespace
