//===- MedSSACallClobberTests.cpp - SSA call occurrence tests ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check call-specific SSA definitions and preserved register prefixes.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/X86/X86Regs.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace {

using namespace neverd;

LowFunc oneBlockFunction() {
  LowFunc Low;
  Low.Entry = 0x1000;
  Low.Name = "call_occurrences";
  Low.Blocks.resize(1);
  Low.Blocks[0].Id = 0;
  Low.Blocks[0].StartAddr = Low.Entry;
  Low.Blocks[0].EndAddr = Low.Entry + 16;
  return Low;
}

void storeRegister(LowBlock &Block, NdVar Value, va_t Address) {
  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.Addr = Block.StartAddr + 8;
  Store.addInput(NdVar::cst(Address, 8));
  Store.addInput(Value);
  Block.Ops.push_back(Store);
}

void returnVoid(LowBlock &Block) {
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = Block.EndAddr - 1;
  Block.Ops.push_back(Return);
}

TEST(MedSSACallClobber, SameAddressCallsKeepDistinctPreservationContracts) {
  const auto Family = gprFamilyOf(Arch::X64, x86reg::R10);
  ASSERT_TRUE(Family);
  const uint32_t WritesR10 = uint32_t(1) << *Family;
  const std::set<va_t> Probes = {0x4000};
  LowToMedConverter Converter;
  Converter.setStackProbeSlots(&Probes);
  // Reuse the converter while changing both the callee summary and format.
  for (auto Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool PreserveFirst : {true, false}) {
      SCOPED_TRACE(testing::Message() << int(Format) << ':' << PreserveFirst);
      const std::map<va_t, uint32_t> Writes = {
          {0x2000, PreserveFirst ? ~WritesR10 : UINT32_MAX},
          {0x3000, PreserveFirst ? UINT32_MAX : ~WritesR10}};
      Converter.setCallMayWriteGPRs(&Writes);
      auto Low = oneBlockFunction();
      auto &Block = Low.Blocks[0];
      const auto Register = NdVar::reg(x86reg::R10, 8);
      LowOp Define;
      Define.Opcode = NdOp::COPY;
      Define.Addr = Low.Entry;
      Define.Output = Register;
      Define.addInput(NdVar::cst(0x1234, 8));
      Block.Ops.push_back(Define);
      for (unsigned I = 0; I < 3; ++I) {
        LowOp Call;
        Call.Opcode = NdOp::CALL;
        Call.Addr = Low.Entry + 4;
        Call.Seq = I;
        Call.Output = NdVar::reg(x86reg::RAX, 8);
        Call.addInput(NdVar::cst(0x2000 + I * 0x1000, 8));
        Block.Ops.push_back(Call);
        storeRegister(Block, Register, 0x5000 + I * 8);
      }
      returnVoid(Block);
      auto Med = Converter.convert(Low, Arch::X64, Format);
      ASSERT_TRUE(verifyMedFunc(Med, "call-preservation-occurrences"));
      std::vector<const MedOp *> Calls, Stores;
      for (const auto &B : Med.Blocks)
        for (const auto &Op : B.Ops) {
          if (Op.Opcode == NdOp::CALL)
            Calls.push_back(&Op);
          if (Op.Opcode == NdOp::STORE)
            Stores.push_back(&Op);
        }
      ASSERT_EQ(Calls.size(), 3u);
      ASSERT_EQ(Stores.size(), 3u);
      EXPECT_EQ(Calls[0]->Addr, Calls[1]->Addr);
      EXPECT_NE(Calls[0]->CallSiteId, Calls[1]->CallSiteId);
      EXPECT_TRUE(Calls[2]->PreservesCallerSaved);
      const auto Clobber = std::find_if(
          Med.CallClobbers.begin(), Med.CallClobbers.end(),
          [](const auto &C) { return C.Value.RegOff == x86reg::R10; });
      ASSERT_NE(Clobber, Med.CallClobbers.end());
      EXPECT_EQ(Clobber->CallSiteId, Calls[PreserveFirst ? 1 : 0]->CallSiteId);
      EXPECT_EQ(std::count_if(Med.CallClobbers.begin(), Med.CallClobbers.end(),
                              [](const auto &C) {
                                return C.Value.RegOff == x86reg::R10;
                              }),
                1);
      EXPECT_EQ(Stores[0]->Inputs[1].isConst(), PreserveFirst);
      if (PreserveFirst)
        EXPECT_EQ(Stores[0]->Inputs[1].ConstVal, 0x1234u);
      else
        EXPECT_EQ(Stores[0]->Inputs[1], Clobber->Value);
      EXPECT_EQ(Stores[1]->Inputs[1], Clobber->Value);
      EXPECT_EQ(Stores[2]->Inputs[1], Clobber->Value);
    }
}

TEST(MedSSACallClobber, RepeatedCallsChainThePreservedVectorPrefix) {
  LowToMedConverter Converter;
  for (Arch Architecture : {Arch::AArch64, Arch::X64})
    for (auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(testing::Message()
                   << int(Architecture) << ':' << int(Format));
      const auto &TRI = getTargetRegInfo(Architecture);
      const uint16_t Width = Architecture == Arch::AArch64 ? 16 : 32;
      const uint16_t Prefix = Architecture == Arch::AArch64  ? 8
                              : Format == BinaryFormat::COFF ? 16
                                                             : 0;
      const auto Offset = TRI.VecRegBase + 8 * TRI.VecRegStride;
      auto Low = oneBlockFunction();
      auto &Block = Low.Blocks[0];
      // The first call reads an incoming prefix, causing entry seeding before
      // SSA renaming. Later calls must read the preceding call's SSA version.
      for (unsigned I = 0; I < 2; ++I) {
        LowOp Call;
        Call.Opcode = I ? NdOp::INDIR_CALL : NdOp::CALL;
        Call.Addr = Low.Entry + 4;
        Call.Seq = I;
        Call.Output = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
        Call.addInput(NdVar::cst(0x2000 + I * 0x1000, TRI.PointerSize));
        Block.Ops.push_back(Call);
      }
      storeRegister(Block, NdVar::reg(Offset, Width), 0x5000);
      returnVoid(Block);
      auto Med = Converter.convert(Low, Architecture, Format);
      ASSERT_TRUE(verifyMedFunc(Med, "call-vector-prefix-occurrences"));
      std::vector<const MedCallClobber *> Clobbers;
      const MedOp *Store = nullptr;
      for (const auto &C : Med.CallClobbers)
        if (C.Value.RegOff == Offset && C.Value.Size == Width)
          Clobbers.push_back(&C);
      for (const auto &B : Med.Blocks)
        for (const auto &Op : B.Ops)
          if (Op.Opcode == NdOp::STORE)
            Store = &Op;
      ASSERT_EQ(Clobbers.size(), 2u);
      ASSERT_NE(Store, nullptr);
      EXPECT_NE(Clobbers[0]->CallSiteId, Clobbers[1]->CallSiteId);
      EXPECT_NE(Clobbers[0]->Value.SSAVer, Clobbers[1]->Value.SSAVer);
      EXPECT_EQ(Clobbers[0]->PreservedPrefixSize, Prefix);
      EXPECT_EQ(Clobbers[1]->PreservedPrefixSize, Prefix);
      if (Prefix) {
        EXPECT_EQ(Clobbers[1]->PreservedInput, Clobbers[0]->Value);
        EXPECT_EQ(Clobbers[0]->PreservedInput.Kind, MedVar::Reg);
        EXPECT_LT(Clobbers[0]->PreservedInput.SSAVer,
                  Clobbers[0]->Value.SSAVer);
      }
      EXPECT_EQ(Store->Inputs[1], Clobbers[1]->Value);
    }
}

} // namespace
