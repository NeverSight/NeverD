//===- MedPrologueSavesTests.cpp - Callee-save prologue storage ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/med/MedCallReturnReads.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include "llvm/ADT/StringExtras.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

enum : int { kSP = 1, kRBX = 2, kRCX = 3 };

MedVar reg(int Id, uint64_t RegOff, int Version) {
  MedVar V;
  V.Kind = MedVar::Reg;
  V.TheArch = Arch::X64;
  V.Id = Id;
  V.SSAVer = Version;
  V.Size = 8;
  V.RegOff = RegOff;
  return V;
}
MedVar sp(int Version) { return reg(kSP, reg::RSP, Version); }
MedVar rbx(int Version) { return reg(kRBX, reg::RBX, Version); }
MedVar rcx(int Version) {
  return reg(kRCX, getTargetRegInfo(Arch::X64).Win64ParamRegs[0], Version);
}

MedVar temp(int Id) {
  MedVar V;
  V.Kind = MedVar::Temp;
  V.TheArch = Arch::X64;
  V.Id = Id;
  V.SSAVer = 1;
  V.Size = 8;
  return V;
}

MedVar none() {
  MedVar V;
  V.Id = -1;
  return V;
}

MedOp op(NdOp Opcode, MedVar Output, std::initializer_list<MedVar> Inputs) {
  MedOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  for (const MedVar &Input : Inputs)
    Result.addInput(Input);
  return Result;
}

/// `push rbx; mov rbx, rcx; …; pop rbx; ret` in MedIR: the prologue saves the
/// caller's rbx below the entry stack pointer, the epilogue restores it.
MedFunc pushedRbx(CallingConv Convention = CallingConv::SysV_AMD64) {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.CC = Convention;
  Func.FrameSize = 8;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
               op(NdOp::COPY, rbx(0), {rbx(0)}),
               op(NdOp::COPY, rcx(0), {rcx(0)}),
               op(NdOp::INT_SUB, sp(1), {sp(0), MedVar::makeConst(8, 8)}),
               op(NdOp::STORE, none(), {sp(1), rbx(0)}),
               op(NdOp::COPY, rbx(1), {rcx(0)}),
               op(NdOp::LOAD, temp(10), {sp(1)}),
               op(NdOp::COPY, rbx(2), {temp(10)}),
               op(NdOp::INT_ADD, sp(2), {sp(1), MedVar::makeConst(8, 8)}),
               op(NdOp::RETURN, none(), {rbx(1)})};
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

class MedPrologueSaves : public ::testing::TestWithParam<bool> {
protected:
  void markStackAccesses(MedFunc &Func, Arch Architecture, BinaryFormat Format,
                         StackEntryKind Entry = StackEntryKind::Call) {
    if (GetParam()) {
      markSourceStackAccesses(Func, Architecture, Format, Entry);
    } else {
      markCalleeSavePrologueSaves(Func, Architecture, Format, Entry);
      markReturnAddressReads(Func, Architecture, Format, Entry);
    }
  }
};

INSTANTIATE_TEST_SUITE_P(SourceStackAccesses, MedPrologueSaves,
                         ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool> &Info) {
                           return Info.param ? "Combined" : "Separate";
                         });

const MedOp &save(const MedFunc &Func) { return Func.Blocks[0].Ops[4]; }

TEST_P(MedPrologueSaves, APushedEntryValueIsAPrologueSave) {
  MedFunc Func = pushedRbx();
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_TRUE(save(Func).PrologueSave);
  // Marking keeps every operation where call records find it.
  EXPECT_EQ(Func.Blocks[0].Ops.size(), 10u);
}

TEST(MedSourceStackAccesses, ChangedStoresLoseTheirOldPrologueProof) {
  MedFunc Func = pushedRbx();
  markSourceStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  ASSERT_TRUE(save(Func).PrologueSave);
  Func.Blocks[0].Ops[4].Inputs[1] =
      MedVar::makeConst(0, Func.Blocks[0].Ops[4].Inputs[1].Size);
  markSourceStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(save(Func).PrologueSave);
}

TEST_P(MedPrologueSaves, SaveAndReturnAddressAnnotationsShareTheFrame) {
  for (StackEntryKind Entry :
       {StackEntryKind::Call, StackEntryKind::ProcessEntry}) {
    SCOPED_TRACE(static_cast<int>(Entry));
    MedFunc Func = pushedRbx();
    auto &Ops = Func.Blocks[0].Ops;
    Ops.insert(Ops.end() - 1, op(NdOp::LOAD, temp(20), {sp(2)}));
    markStackAccesses(Func, Arch::X64, BinaryFormat::ELF, Entry);
    EXPECT_TRUE(save(Func).PrologueSave);
    EXPECT_EQ(Ops[9].ReturnAddressRead, Entry == StackEntryKind::Call);
    EXPECT_FALSE(Ops[6].ReturnAddressRead);
    EXPECT_EQ(Ops.size(), 11u);
  }
}

TEST_P(MedPrologueSaves, ARestoreTheBodyReadsKeepsItsSave) {
  MedFunc Func = pushedRbx();
  Func.Blocks[0].Ops.back() = op(NdOp::RETURN, none(), {rbx(2)});
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(save(Func).PrologueSave);
}

TEST_P(MedPrologueSaves, OnlyObservedRestoreJoinsKeepTheirSave) {
  for (bool Observed : {false, true}) {
    SCOPED_TRACE(Observed);
    MedFunc Func = pushedRbx();
    auto &Entry = Func.Blocks.front();
    Entry.Ops.pop_back();
    Entry.Succs = {1};
    MedBlock Join;
    Join.Id = 1;
    Join.Preds = {0};
    PhiNode Phi;
    Phi.Output = rbx(3);
    Phi.Args.push_back({0, rbx(2)});
    Join.Phis.push_back(Phi);
    Join.Ops = {op(NdOp::COPY, temp(20), {rbx(3)}),
                op(NdOp::RETURN, none(), {Observed ? temp(20) : rcx(0)})};
    Func.Blocks.push_back(std::move(Join));
    markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
    EXPECT_EQ(Func.Blocks.front().Ops[4].PrologueSave, !Observed);
  }
}

TEST_P(MedPrologueSaves, AComputedValueIsNoSave) {
  MedFunc Func = pushedRbx();
  Func.Blocks[0].Ops[4] = op(NdOp::STORE, none(), {sp(1), rbx(1)});
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(save(Func).PrologueSave);
}

TEST_P(MedPrologueSaves, AnOverwritePreventsOnlyTheCalleeSaveProof) {
  MedFunc Func = pushedRbx();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.insert(Ops.begin() + 6, op(NdOp::STORE, none(), {sp(1), rcx(0)}));
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  // It no longer restores rbx's entry value. The combined source pass can
  // nevertheless discard that value: its bytes are overwritten before use.
  EXPECT_EQ(save(Func).PrologueSave, GetParam());
  EXPECT_EQ(Ops.size(), 11u);
}

TEST_P(MedPrologueSaves, AReadBeforeTheOverwriteKeepsTheSave) {
  MedFunc Func = pushedRbx();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.insert(Ops.begin() + 6, op(NdOp::LOAD, temp(20), {sp(1)}));
  Ops.insert(Ops.begin() + 7, op(NdOp::STORE, none(), {sp(1), rcx(0)}));
  Ops.back() = op(NdOp::RETURN, none(), {temp(20)});
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(save(Func).PrologueSave);
  EXPECT_EQ(Ops.size(), 12u);
}

TEST_P(MedPrologueSaves, OverwritesKillOnlyTheirOwnEntrySpillBytes) {
  for (uint16_t Bytes : {4, 8}) {
    SCOPED_TRACE(Bytes);
    MedFunc Func = pushedRbx();
    auto &Ops = Func.Blocks[0].Ops;
    MedVar Value = rcx(0);
    Value.Size = Bytes;
    Ops.insert(Ops.begin() + 6, op(NdOp::STORE, none(), {sp(1), Value}));
    markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
    // Reusing a save slot defeats the register-save recognizer. Combined
    // source preparation can still prove the initial bytes unobservable
    // when a complete overwrite precedes every load; a partial one cannot.
    EXPECT_EQ(save(Func).PrologueSave, GetParam() && Bytes == 8);
    EXPECT_FALSE(Ops[6].PrologueSave);
  }
}

TEST(SourceStackSpills, OverwrittenValuesNeedNotComeFromTheEntry) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (MedVar::VarKind Kind :
           {MedVar::Reg, MedVar::Temp, MedVar::Unspecified}) {
        SCOPED_TRACE(testing::Message() << unsigned(Architecture) << ':'
                                        << unsigned(Format) << ':' << Kind);
        const auto &TRI = getTargetRegInfo(Architecture);
        const auto Width = TRI.PointerSize;
        auto SP = reg(1, TRI.StackPointer, 0);
        SP.TheArch = Architecture;
        SP.Size = Width;
        auto Address = temp(10);
        Address.TheArch = Architecture;
        Address.Size = Width;
        auto Value = MedVar::makeUnspecified(Width, Architecture);
        if (Kind != MedVar::Unspecified) {
          Value = reg(2, TRI.IntReturnReg, 3);
          Value.Kind = Kind;
          Value.TheArch = Architecture;
          Value.Size = Width;
        }
        MedFunc Func;
        Func.Entry = 0x1000;
        Func.FrameSize = Width;
        MedBlock Block;
        Block.Id = 0;
        Block.StartAddr = Func.Entry;
        Block.Ops = {
            op(NdOp::COPY, SP, {SP}),
            op(NdOp::INT_SUB, Address, {SP, MedVar::makeConst(Width, Width)}),
            op(NdOp::STORE, none(), {Address, Value}),
            op(NdOp::STORE, none(), {Address, MedVar::makeConst(19, Width)}),
            op(NdOp::RETURN, none(), {MedVar::makeConst(19, Width)})};
        Func.Blocks.push_back(std::move(Block));
        markSourceStackAccesses(Func, Architecture, Format);
        EXPECT_TRUE(Func.Blocks[0].Ops[2].PrologueSave);
        EXPECT_EQ(Func.Blocks[0].Ops.size(), 5u);
      }
}

TEST(SourceStackSpills, FlagsTransfersDoNotObserveScratchButKeepTheirEffects) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (unsigned Mode = 0; Mode < 9; ++Mode) {
      SCOPED_TRACE(testing::Message() << unsigned(Architecture) << ':' << Mode);
      MedFunc Func = pushedRbx(CallingConv::Win64);
      auto &Ops = Func.Blocks[0].Ops;
      Ops.resize(5);
      for (auto &Op : Ops) {
        Op.Output.TheArch = Architecture;
        Op.Output.Size =
            Op.Output.Size ? getTargetRegInfo(Architecture).PointerSize : 0;
        for (unsigned I = 0; I < Op.NumInputs; ++I) {
          Op.Inputs[I].TheArch = Architecture;
          Op.Inputs[I].Size = getTargetRegInfo(Architecture).PointerSize;
        }
      }
      const auto Address = Ops[4].Inputs[0];
      const auto Width = getTargetRegInfo(Architecture).PointerSize;
      Ops[4].Inputs[1] = MedVar::makeUnspecified(Width, Architecture);
      auto Flags = temp(20);
      Flags.Size = Width;
      Flags.TheArch = Architecture;
      Ops.push_back(op(NdOp::INTRINSIC, Flags,
                       {MedVar::makeConst(unsigned(Intrinsic::Pushf), 2)}));
      Ops.push_back(
          op(NdOp::INTRINSIC, none(),
             {MedVar::makeConst(unsigned(Intrinsic::Popf), 2), Flags}));
      switch (Mode) {
      case 1: // An unrelated or unknown intrinsic remains a barrier.
        Ops[5].Inputs[0].ConstVal = 0xFFFF;
        break;
      case 2:
        Ops[5].Output.Size = 3;
        break;
      case 3:
        Ops[6].Inputs[1].Size = 3;
        break;
      case 4:
        Ops[6].NumInputs = 1;
        break;
      case 5:
        Ops[5].Inputs[0].Size = 8;
        break;
      case 6:
        Ops[5].IntrinsicOutputs.push_back(temp(30));
        break;
      case 7:
        Ops[6].MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
        break;
      case 8:
        Func.Blocks[0].ExceptionalSuccs.emplace_back(1, 0x2000);
        break;
      }
      Ops.push_back(
          op(NdOp::STORE, none(), {Address, MedVar::makeConst(19, Width)}));
      Ops.push_back(op(NdOp::RETURN, none(), {MedVar::makeConst(19, Width)}));
      markSourceStackAccesses(Func, Architecture, BinaryFormat::COFF);
      EXPECT_EQ(Ops[4].PrologueSave, Mode == 0);
      EXPECT_EQ(Ops[5].Opcode, NdOp::INTRINSIC);
      EXPECT_EQ(Ops[6].Opcode, NdOp::INTRINSIC);
      EXPECT_FALSE(Ops[5].Dead);
      EXPECT_FALSE(Ops[6].Dead);
      EXPECT_EQ(Ops.size(), 9u);
    }
}

TEST(SourceStackSpills, OnlyUnreadScratchRestoresCanBeIgnored) {
  for (unsigned Mode = 0; Mode < 16; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func = pushedRbx(CallingConv::Win64);
    auto &Ops = Func.Blocks[0].Ops;
    Ops[4].Inputs[1] = MedVar::makeUnspecified(8, Arch::X64);
    const auto Loaded = temp(10);
    auto Restored = reg(30, x86reg::R10, 1);
    // R10 is volatile and cannot be an ordinary ABI result.
    if (Mode == 1)
      Restored = rbx(2);
    if (Mode == 2)
      Restored.RegOff = getTargetRegInfo(Arch::X64).IntReturnReg;
    if (Mode == 3)
      Restored = sp(2);
    Ops[7] = op(NdOp::COPY, Restored, {Loaded});
    Ops[8] = op(NdOp::STORE, none(), {sp(1), MedVar::makeConst(19, 8)});
    Ops[9] = op(NdOp::RETURN, none(), {MedVar::makeConst(19, 8)});
    if (Mode == 4)
      Ops[9].Inputs[0] = Restored;
    if (Mode == 5) {
      PhiNode Phi;
      Phi.Output = temp(40);
      Phi.Args = {{0, Restored}};
      Func.Blocks[0].Phis.push_back(std::move(Phi));
    }
    if (Mode == 6) {
      MedCallClobber Clobber;
      Clobber.Value = temp(40);
      Clobber.PreservedInput = Restored;
      Clobber.PreservedPrefixSize = 4;
      Func.CallClobbers.push_back(Clobber);
    }
    if (Mode == 7)
      Ops.insert(Ops.begin() + 8,
                 op(NdOp::CALL, none(), {MedVar::makeConst(0x2000, 8), sp(1)}));
    if (Mode >= 8 && Mode <= 10) {
      // The slot is dead, but ABI recovery could still observe the restored
      // register at a later call. Only a complete register overwrite kills it.
      Ops.insert(Ops.end() - 1,
                 op(NdOp::CALL, none(), {MedVar::makeConst(0x2000, 8)}));
      if (Mode >= 9) {
        auto Overwritten = Restored;
        ++Overwritten.SSAVer;
        if (Mode == 10)
          Overwritten.Size = 4;
        Ops.insert(Ops.end() - 2, op(NdOp::COPY, Overwritten,
                                     {MedVar::makeConst(0, Overwritten.Size)}));
      }
    }
    if (Mode == 11) {
      Ops[7].Output.RegOff = x86reg::DF;
      Ops[7].Output.Size = 1;
    }
    if (Mode == 12)
      Func.MultiReturn.push_back({Restored.RegOff, Restored.Size, false});
    if (Mode == 13)
      Ops[9] = op(NdOp::INDIR_BR, none(), {MedVar::makeConst(0x2000, 8)});
    if (Mode == 14) {
      // A straight continuation kills the scratch register before its call.
      Ops[9] = op(NdOp::BRANCH, none(), {MedVar::makeConst(0x2000, 8)});
      Func.Blocks[0].Succs = {1};
      auto Overwritten = Restored;
      ++Overwritten.SSAVer;
      MedBlock Next;
      Next.Id = 1;
      Next.StartAddr = 0x2000;
      Next.Preds = {0};
      Next.Ops = {op(NdOp::COPY, Overwritten, {MedVar::makeConst(0, 8)}),
                  op(NdOp::CALL, none(), {MedVar::makeConst(0x3000, 8)}),
                  op(NdOp::RETURN, none(), {MedVar::makeConst(19, 8)})};
      Func.Blocks.push_back(std::move(Next));
    }
    if (Mode == 15)
      Ops[7].Output.RegOff = x86reg::RSI;
    markSourceStackAccesses(
        Func, Arch::X64, Mode == 15 ? BinaryFormat::ELF : BinaryFormat::COFF);
    EXPECT_EQ(Func.Blocks[0].Ops[4].PrologueSave,
              Mode == 0 || Mode == 9 || Mode == 14);
  }
}

TEST(SourceStackSpills, UnreadCalleeInputsRequireTheOriginalCallProof) {
  for (unsigned Mode = 0; Mode != 12; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func = pushedRbx(CallingConv::Win64);
    auto &Ops = Func.Blocks[0].Ops;
    const auto Restored = reg(30, x86reg::R10, 1);
    Ops[4].Inputs[1] = MedVar::makeUnspecified(8, Arch::X64);
    Ops[7] = op(NdOp::COPY, Restored, {temp(10)});
    Ops[8] = op(NdOp::STORE, none(), {sp(1), MedVar::makeConst(19, 8)});
    Ops[9] = op(NdOp::CALL, none(), {MedVar::makeConst(0x2000, 8)});
    Ops[9].CallSiteId = 1;
    Ops[9].Addr = 0x1010;
    Ops[9].OriginSeq = 10;
    Ops[9].CallPreservedGPRs = ~uint32_t{0};
    Func.CallUnreadRegisters.emplace(
        1, MedCallUnreadRegisters{0x2000, 0x1010, 10,
                                  uint32_t{1} << (x86reg::R10 / 8)});
    Ops.push_back(op(NdOp::RETURN, none(), {MedVar::makeConst(19, 8)}));
    if (Mode == 1)
      Func.CallUnreadRegisters.clear();
    if (Mode == 2)
      Func.CallUnreadRegisters.at(1).Families = 0;
    if (Mode == 3)
      Ops[9].Inputs[0] = MedVar::makeConst(0x3000, 8);
    if (Mode == 4)
      Ops[9].addInput(Restored);
    if (Mode == 5)
      Ops[9].Opcode = NdOp::INDIR_CALL;
    if (Mode == 6)
      ++Ops[9].Addr;
    if (Mode == 7)
      ++Ops[9].OriginSeq;
    if (Mode == 8)
      ++Ops[9].CallSiteId;
    if (Mode == 9)
      Ops.back().Inputs[0] = Restored;
    if (Mode == 10)
      Ops.insert(Ops.end() - 1,
                 op(NdOp::CALL, none(), {MedVar::makeConst(0x3000, 8)}));
    if (Mode == 11)
      Ops[9].CallPreservedGPRs = 0;
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
    EXPECT_EQ(save(Func).PrologueSave, Mode == 0 || Mode == 11);
  }
}

TEST_P(MedPrologueSaves, AnArgumentSpillIsNoSave) {
  MedFunc Func = pushedRbx();
  Func.Blocks[0].Ops[4] = op(NdOp::STORE, none(), {sp(1), rcx(0)});
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(save(Func).PrologueSave);
}

TEST_P(MedPrologueSaves, OutOfFrameExtentsAreNotPrivateStorage) {
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF})
    for (int64_t Offset :
         {std::numeric_limits<int64_t>::max(),
          std::numeric_limits<int64_t>::max() - 7,
          std::numeric_limits<int64_t>::max() - 8,
          std::numeric_limits<int64_t>::min(), int64_t{-9}, int64_t{0}}) {
      SCOPED_TRACE(testing::Message() << unsigned(Format) << ':' << Offset);
      MedFunc Func =
          pushedRbx(Format == BinaryFormat::COFF ? CallingConv::Win64
                                                 : CallingConv::SysV_AMD64);
      Func.Blocks[0].Ops[3] =
          op(NdOp::INT_ADD, sp(1),
             {sp(0), MedVar::makeConst(uint64_t(Offset), 8)});
      markStackAccesses(Func, Arch::X64, Format);
      EXPECT_FALSE(save(Func).PrologueSave);
    }
}

/// `mov [rsp+8], rbx; …; mov rbx, [rsp+8]; ret`: MSVC saves rbx in the home
/// slot Win64 hands the callee.
MedFunc homeSlotRbx(CallingConv Convention) {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.CC = Convention;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
               op(NdOp::COPY, rbx(0), {rbx(0)}),
               op(NdOp::COPY, rcx(0), {rcx(0)}),
               op(NdOp::INT_ADD, temp(11), {sp(0), MedVar::makeConst(8, 8)}),
               op(NdOp::STORE, none(), {temp(11), rbx(0)}),
               op(NdOp::COPY, rbx(1), {rcx(0)}),
               op(NdOp::INT_ADD, temp(12), {sp(0), MedVar::makeConst(8, 8)}),
               op(NdOp::LOAD, temp(13), {temp(12)}),
               op(NdOp::COPY, rbx(2), {temp(13)}),
               op(NdOp::RETURN, none(), {rbx(1)})};
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

TEST_P(MedPrologueSaves, AWin64HomeSlotSaveIsPrologueStorage) {
  MedFunc Func = homeSlotRbx(CallingConv::Win64);
  markStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
  EXPECT_TRUE(save(Func).PrologueSave);
}

TEST_P(MedPrologueSaves, OnlyWin64HandsTheCalleeHomeSlots) {
  // Above a System V callee's return address lies its caller's frame.
  MedFunc Func = homeSlotRbx(CallingConv::SysV_AMD64);
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(save(Func).PrologueSave);
}

/// `mov rax, [rsp]; ret`: the function reads the return address its call
/// pushed.
MedFunc readsReturnAddress() {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.CC = CallingConv::SysV_AMD64;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
               op(NdOp::LOAD, temp(20), {sp(0)}),
               op(NdOp::RETURN, none(), {temp(20)})};
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

const MedOp &read(const MedFunc &Func) { return Func.Blocks[0].Ops[1]; }

TEST_P(MedPrologueSaves, ALoadOfTheEntrySlotIsTheReturnAddress) {
  MedFunc Func = readsReturnAddress();
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_TRUE(read(Func).ReturnAddressRead);
}

TEST_P(MedPrologueSaves, AFunctionThatWritesTheSlotReadsItsOwnValue) {
  MedFunc Func = readsReturnAddress();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.insert(Ops.begin() + 1, op(NdOp::STORE, none(), {sp(0), rcx(0)}));
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(Func.Blocks[0].Ops[2].ReturnAddressRead);
}

TEST_P(MedPrologueSaves, ANarrowLoadOfTheEntrySlotIsNoReturnAddress) {
  MedFunc Func = readsReturnAddress();
  MedVar Narrow = temp(20);
  Narrow.Size = 4;
  Func.Blocks[0].Ops[1].Output = Narrow;
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
  EXPECT_FALSE(read(Func).ReturnAddressRead);
}

TEST_P(MedPrologueSaves, AProcessEntryHasNoReturnAddress) {
  // The kernel starts `_start` with argc at the stack pointer.
  MedFunc Func = readsReturnAddress();
  markStackAccesses(Func, Arch::X64, BinaryFormat::ELF,
                    StackEntryKind::ProcessEntry);
  EXPECT_FALSE(read(Func).ReturnAddressRead);
}

TEST_P(MedPrologueSaves, AArch64KeepsItsReturnAddressInTheLinkRegister) {
  MedFunc Func = readsReturnAddress();
  markStackAccesses(Func, Arch::AArch64, BinaryFormat::ELF);
  EXPECT_FALSE(read(Func).ReturnAddressRead);
}

void consumeFrameRead(MedBlock &Block) {
  MedOp Call = op(NdOp::CALL, temp(20), {MedVar::makeConst(0x2000, 8)});
  auto Signature = std::make_shared<FixedIntegerCallArguments>();
  const auto Registers =
      getTargetRegInfo(Arch::X64).integerArgumentLayout(true);
  for (auto Register : Registers.Registers) {
    Signature->Parameters.push_back(
        {SourceABICarrierKind::IntegerRegister, Register, 0, 8});
    Call.addInput(MedVar::makeConst(0, 8));
  }
  Signature->Parameters.push_back({SourceABICarrierKind::Stack, 0, 40, 8});
  Call.addInput(temp(10));
  Call.IntegerCallSignature = std::move(Signature);
  Block.Ops.insert(Block.Ops.end() - 1, std::move(Call));
}

TEST(SourceFrameReads, MissingAndPartialPrivateBytesRemainUnavailable) {
  for (unsigned WrittenBytes : {0, 4, 8}) {
    SCOPED_TRACE(WrittenBytes);
    auto Func = pushedRbx();
    auto &Store = Func.Blocks[0].Ops[4];
    if (WrittenBytes)
      Store.Inputs[1].Size = WrittenBytes;
    else
      Store = op(NdOp::NOP, none(), {});
    consumeFrameRead(Func.Blocks.back());
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
    const auto &Read = Func.Blocks[0].Ops[6];
    if (WrittenBytes == 8)
      EXPECT_EQ(Read.Opcode, NdOp::LOAD);
    else {
      ASSERT_EQ(Read.Opcode, NdOp::COPY);
      ASSERT_EQ(Read.NumInputs, 1);
      EXPECT_EQ(Read.Inputs[0].Kind, MedVar::Unspecified);
      EXPECT_EQ(Read.Inputs[0].Size, 8);
    }
  }
}

TEST(SourceFrameReads, PossibleWritersPreventAnUnavailableClaim) {
  for (auto Barrier :
       {NdOp::CALL, NdOp::INDIR_CALL, NdOp::INTRINSIC, NdOp::ATOMIC_XCHG,
        NdOp::ATOMIC_ADD, NdOp::ATOMIC_CMPXCHG, NdOp::STORE}) {
    SCOPED_TRACE(ndOpName(Barrier));
    auto Func = pushedRbx();
    Func.Blocks[0].Ops[4] =
        op(Barrier, none(), {rcx(0), MedVar::makeConst(0, 8)});
    consumeFrameRead(Func.Blocks.back());
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
    EXPECT_EQ(Func.Blocks[0].Ops[6].Opcode, NdOp::LOAD);
  }
}

TEST(SourceFrameReads, AllPredecessorWritesAndQueryBackedgesAreConsidered) {
  for (unsigned Shape : {0, 1, 2, 3}) {
    SCOPED_TRACE(Shape);
    auto Func = pushedRbx();
    auto &Entry = Func.Blocks[0];
    MedBlock Query;
    Query.Id = 3;
    Query.Ops.assign(Entry.Ops.begin() + 6, Entry.Ops.end());
    Query.Preds = {1, 2};
    Entry.Ops.resize(4);
    Entry.Succs = {1, 2};
    MedBlock Left, Right;
    Left.Id = 1;
    Right.Id = 2;
    Left.Preds = Right.Preds = {0};
    Left.Succs = Right.Succs = {3};
    if (Shape == 1)
      Left.Ops.push_back(op(NdOp::STORE, none(), {sp(1), rbx(0)}));
    if (Shape == 2) {
      Query.Preds.push_back(3);
      Query.Succs = {3};
    }
    if (Shape == 3) // A stale/incomplete edge cannot establish an entry path.
      Query.Preds.clear();
    Func.Blocks.push_back(std::move(Left));
    Func.Blocks.push_back(std::move(Right));
    Func.Blocks.push_back(std::move(Query));
    consumeFrameRead(Func.Blocks.back());
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
    EXPECT_EQ(Func.Blocks.back().Ops[0].Opcode,
              Shape ? NdOp::LOAD : NdOp::COPY);
  }
}

TEST(SourceFrameReads, GeneralPartialLoadsKeepTheirInitializedBits) {
  auto Func = pushedRbx();
  Func.Blocks[0].Ops[4].Inputs[1].Size = 4;
  // Only the low written word is returned. A source-only proof must not
  // replace the whole load with unavailable state merely for its upper word.
  auto LowWord =
      op(NdOp::SUBBYTES, temp(30), {temp(10), MedVar::makeConst(0, 4)});
  LowWord.Output.Size = 4;
  Func.Blocks[0].Ops.back().Inputs[0] = LowWord.Output;
  Func.Blocks[0].Ops.insert(Func.Blocks[0].Ops.end() - 1, LowWord);
  markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
  EXPECT_EQ(Func.Blocks[0].Ops[6].Opcode, NdOp::LOAD);
}

TEST(SourceStackSpills, NontrappingBitCountsDoNotObserveScratchBytes) {
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func = pushedRbx();
    auto &Ops = Func.Blocks[0].Ops;
    Ops.resize(5);
    Ops[4].Inputs[1] = MedVar::makeUnspecified(8, Arch::X64);
    if (Mode == 2)
      Ops.push_back(op(NdOp::LOAD, temp(20), {sp(1)}));
    Ops.push_back(op(Mode == 1 ? NdOp::INT_DIV : NdOp::POPCOUNT, temp(21),
                     {Mode == 2 ? temp(20) : rcx(0)}));
    if (Mode == 1)
      Ops.back().addInput(MedVar::makeConst(0, 8));
    Ops.push_back(op(NdOp::STORE, none(), {sp(1), MedVar::makeConst(19, 8)}));
    Ops.push_back(op(NdOp::RETURN, none(), {temp(21)}));
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
    EXPECT_EQ(save(Func).PrologueSave, Mode == 0);
    EXPECT_FALSE(Ops[Ops.size() - 3].Dead);
  }
}

TEST(SourceStackSpills, ImplicitCallWritesAreNotProjectedFrameStores) {
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func = pushedRbx();
    auto &Ops = Func.Blocks[0].Ops;
    Ops.resize(5);
    Ops[4].Inputs[1] = MedVar::makeUnspecified(8, Arch::X64);
    Ops.push_back(op(NdOp::INT_ADD, sp(2), {sp(1), MedVar::makeConst(8, 8)}));
    Ops.push_back(op(NdOp::CALL, temp(20), {MedVar::makeConst(0x2000, 8)}));
    if (Mode == 2)
      Ops.back().addInput(sp(1));
    if (Mode == 0)
      Ops.push_back(op(NdOp::LOAD, temp(21), {sp(1)}));
    Ops.push_back(op(NdOp::RETURN, none(),
                     {Mode == 0   ? temp(21)
                      : Mode == 1 ? sp(1)
                                  : MedVar::makeConst(19, 8)}));
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::ELF);
    // CALL replaced the old bytes in the machine, but the C frame has no
    // corresponding store. A later load, returned frame address or callee
    // observation must not turn an unavailable value into uninitialized C.
    EXPECT_FALSE(save(Func).PrologueSave);
  }
}

BinaryImage callSlotImage(uint8_t Encoding = 0xe8) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::ELF;
  Segment Code;
  Code.VA = 0x1000;
  Code.Size = 64;
  Code.Data.resize(Code.Size, 0x90);
  Code.Data[8] = Encoding;
  Image.Segments.push_back(std::move(Code));
  return Image;
}

MedFunc observedCallSlot() {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "call_slot";
  Func.FrameSize = 40;
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  Block.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
               op(NdOp::INT_SUB, sp(1), {sp(0), MedVar::makeConst(32, 8)}),
               op(NdOp::INT_SUB, temp(20), {sp(1), MedVar::makeConst(8, 8)}),
               op(NdOp::STORE, none(), {temp(20), MedVar::makeConst(19, 8)}),
               op(NdOp::CALL, temp(21), {MedVar::makeConst(0x2000, 8)}),
               op(NdOp::LOAD, temp(22), {temp(20)}),
               op(NdOp::RETURN, none(), {temp(22)})};
  Block.Ops[4].Addr = 0x1008;
  Block.Ops[5].Addr = 0x100d;
  Func.Blocks.push_back(std::move(Block));
  return Func;
}

TEST(SourceCallReturnReads, ConstantOldContentsCannotHideTheImplicitWrite) {
  for (Arch Architecture : {Arch::X64, Arch::X86}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    auto Func = observedCallSlot();
    auto Image = callSlotImage();
    Image.Arch = Architecture;
    const auto Width = getTargetRegInfo(Architecture).PointerSize;
    for (auto &Op : Func.Blocks[0].Ops) {
      Op.Output.TheArch = Architecture;
      Op.Output.Size = Width;
      for (unsigned I = 0; I < Op.NumInputs; ++I) {
        Op.Inputs[I].TheArch = Architecture;
        Op.Inputs[I].Size = Width;
      }
    }
    Func.Blocks[0].Ops[2].Inputs[1].ConstVal = Width;
    std::string Error;
    EXPECT_FALSE(validateSourceCallReturnReads(Func, Image, Error));
    EXPECT_NE(Error.find("physical CALL return slot"), std::string::npos);
    EXPECT_NE(Error.find("100D"), std::string::npos);
    EXPECT_EQ(Func.Blocks[0].Ops[3].Inputs[1].ConstVal, 19u);
    EXPECT_FALSE(Func.Blocks[0].Ops[3].PrologueSave);
  }
}

TEST(SourceCallReturnReads, OnlyLaterWritesOfEachObservedByteEndTheHazard) {
  for (unsigned Bytes : {1u, 4u, 8u}) {
    auto Func = observedCallSlot();
    auto &Ops = Func.Blocks[0].Ops;
    Ops.insert(Ops.begin() + 5, op(NdOp::STORE, none(),
                                   {temp(20), MedVar::makeConst(29, Bytes)}));
    std::string Error;
    EXPECT_EQ(validateSourceCallReturnReads(Func, callSlotImage(), Error),
              Bytes == 8)
        << Error;
    Ops[6].Output.Size = Bytes;
    EXPECT_TRUE(validateSourceCallReturnReads(Func, callSlotImage(), Error))
        << Error;
  }
  auto Func = observedCallSlot();
  auto &Ops = Func.Blocks[0].Ops;
  Ops.insert(Ops.begin() + 5,
             {op(NdOp::STORE, none(), {temp(20), MedVar::makeConst(29, 4)}),
              op(NdOp::INT_ADD, temp(23), {temp(20), MedVar::makeConst(4, 8)}),
              op(NdOp::STORE, none(), {temp(23), MedVar::makeConst(31, 4)})});
  std::string Error;
  EXPECT_TRUE(validateSourceCallReturnReads(Func, callSlotImage(), Error))
      << Error;
}

TEST(SourceCallReturnReads, EarlierReadsAndSeparatedSlotsStayValid) {
  auto Func = observedCallSlot();
  std::swap(Func.Blocks[0].Ops[4], Func.Blocks[0].Ops[5]);
  std::string Error;
  EXPECT_TRUE(validateSourceCallReturnReads(Func, callSlotImage(), Error))
      << Error;
  Func = observedCallSlot();
  Func.Blocks[0].Ops[2].Inputs[1] = MedVar::makeConst(16, 8);
  EXPECT_TRUE(validateSourceCallReturnReads(Func, callSlotImage(), Error))
      << Error;
}

TEST(SourceCallReturnReads, RepeatedReadsShareCompleteByteHistories) {
  auto Func = observedCallSlot();
  auto &Ops = Func.Blocks[0].Ops;
  const auto Call = Ops[4];
  auto Return = Ops[6];
  Return.Inputs[0] = temp(227);
  Ops.resize(4);
  for (int I = 0; I != 128; ++I) {
    Ops.push_back(op(NdOp::NOP, none(), {}));
    Ops.push_back(op(NdOp::LOAD, temp(100 + I), {temp(20)}));
  }
  Ops.insert(Ops.end(), {Call, Return});
  std::string Error;
  EXPECT_TRUE(validateSourceCallReturnReads(Func, callSlotImage(), Error, 8192))
      << Error;
  // A later observation still reaches the CALL, even when earlier reads of
  // the same bytes have populated the completed-history cache.
  Ops.insert(Ops.end() - 1, op(NdOp::LOAD, temp(300), {temp(20)}));
  EXPECT_FALSE(
      validateSourceCallReturnReads(Func, callSlotImage(), Error, 8192));
  EXPECT_NE(Error.find("physical CALL return slot"), std::string::npos)
      << Error;
}

TEST(SourceCallReturnReads, NonMemoryOperationsDoNotMultiplyByteHistories) {
  for (unsigned Written : {4u, 8u}) {
    auto Func = observedCallSlot();
    auto &Ops = Func.Blocks.front().Ops;
    Ops.insert(Ops.begin() + 5, op(NdOp::STORE, none(),
                                   {temp(20), MedVar::makeConst(29, Written)}));
    Ops.insert(Ops.begin() + 6, 2048, op(NdOp::NOP, none(), {}));
    std::string Error;
    EXPECT_EQ(validateSourceCallReturnReads(Func, callSlotImage(), Error, 8192),
              Written == 8)
        << Error;
    if (Written != 8)
      EXPECT_NE(Error.find("physical CALL return slot"), std::string::npos)
          << Error;
  }
}

TEST(SourceCallReturnReads, BoundedAllocationsSeparateTheFixedFrame) {
  for (bool Bounded : {false, true})
    for (bool Overlap : {false, true}) {
      auto Func = observedCallSlot();
      auto &Ops = Func.Blocks.front().Ops;
      if (!Overlap)
        Ops[2].Opcode = NdOp::INT_ADD;
      Ops.insert(Ops.begin() + 4,
                 {op(Bounded ? NdOp::INT_ZEXT : NdOp::COPY, temp(30),
                     {MedVar::makeUnspecified(Bounded ? 4 : 8, Arch::X64)}),
                  op(NdOp::INT_SUB, sp(2), {sp(1), temp(30)})});
      std::string Error;
      EXPECT_EQ(validateSourceCallReturnReads(Func, callSlotImage(), Error),
                Bounded && !Overlap)
          << Error;
    }
}

TEST(SourceCallReturnReads,
     AlignedUnboundedAllocationsKeepOnlyPossibleAliases) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    const unsigned Width = getTargetRegInfo(Architecture).PointerSize;
    const int Alignment = 2 * Width;
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (unsigned ReadWidth : {1u, Width, Width + 1})
        for (int Displacement = -Alignment; Displacement <= Alignment;
             ++Displacement) {
          SCOPED_TRACE(::testing::Message()
                       << int(Architecture) << ':' << int(Format) << ':'
                       << ReadWidth << ':' << Displacement);
          // An independent byte oracle includes negative allocations: the
          // mask establishes alignment, never a nonnegative size. A complete
          // period on each side covers every possible relative placement.
          bool Overlaps = false;
          for (int Count = -4 * Alignment; Count <= 4 * Alignment;
               Count += Alignment)
            for (unsigned I = 0; I < Width; ++I)
              for (unsigned J = 0; J < ReadWidth; ++J)
                Overlaps |=
                    -Count - int(Width) + int(I) == Displacement + int(J);
          auto Image = callSlotImage();
          Image.Arch = Architecture;
          Image.Format = Format;
          auto Func = observedCallSlot();
          auto &Ops = Func.Blocks.front().Ops;
          Ops[2].Opcode = NdOp::INT_ADD;
          Ops[2].Inputs[1] = MedVar::makeConst(
              uint64_t(Displacement) & (Width == 4 ? UINT32_MAX : UINT64_MAX),
              Width);
          Ops.insert(
              Ops.begin() + 4,
              {op(NdOp::INT_AND, temp(30),
                  {MedVar::makeUnspecified(Width, Architecture),
                   MedVar::makeConst(uint64_t(-Alignment) &
                                         (Width == 4 ? UINT32_MAX : UINT64_MAX),
                                     Width)}),
               op(NdOp::INT_SUB, sp(2), {sp(1), temp(30)})});
          for (auto &Op : Ops) {
            Op.Output.TheArch = Architecture;
            Op.Output.Size = Width;
            for (unsigned I = 0; I < Op.NumInputs; ++I) {
              Op.Inputs[I].TheArch = Architecture;
              Op.Inputs[I].Size = Width;
            }
          }
          Ops[3].Inputs[1].Size = ReadWidth;
          Ops[7].Output.Size = ReadWidth;
          Ops[8].Inputs[0].Size = ReadWidth;
          std::string Error;
          EXPECT_EQ(validateSourceCallReturnReads(Func, Image, Error),
                    !Overlaps)
              << Error;
          if (Overlaps)
            EXPECT_NE(Error.find("physical CALL return slot"),
                      std::string::npos);
        }
  }
}

TEST(SourceCallReturnReads, ResidueSeparationIncludesEveryCall) {
  auto Image = callSlotImage();
  Image.Segments.front().Data[16] = 0xe8;
  for (unsigned Mode = 0; Mode != 4; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Func = observedCallSlot();
    auto &Ops = Func.Blocks.front().Ops;
    Ops[2] = op(NdOp::COPY, temp(20), {sp(1)});
    Ops[5].Addr = 0x1015;
    Ops.insert(Ops.begin() + 4, {op(NdOp::INT_AND, temp(30),
                                    {MedVar::makeUnspecified(8, Arch::X64),
                                     MedVar::makeConst(~uint64_t{15}, 8)}),
                                 op(NdOp::INT_SUB, sp(2), {sp(1), temp(30)})});
    auto Second = op(NdOp::CALL, temp(31), {MedVar::makeConst(0x2000, 8)});
    Second.Addr = 0x1010;
    Ops.insert(
        Ops.begin() + 7,
        {op(NdOp::INT_ADD, sp(3),
            {sp(2), Mode == 2 ? MedVar::makeUnspecified(8, Arch::X64)
                              : MedVar::makeConst(Mode == 1 ? 8 : 0, 8)}),
         Second});
    std::string Error;
    auto Encoding = Image;
    if (Mode == 3)
      Encoding.Segments.front().Data[16] = 0x66;
    EXPECT_EQ(validateSourceCallReturnReads(Func, Encoding, Error), Mode == 0)
        << Error;
    std::vector<MedCallReturnRead> Deferred;
    EXPECT_TRUE(validateSourceCallReturnReads(
        Func, Encoding, Error, limits::ir::kSourceCallReturnReadWork, nullptr,
        nullptr, &Deferred))
        << Error;
    EXPECT_EQ(Deferred.size(), Mode == 0 ? 0u : 1u);
  }
}

TEST(SourceCallReturnReads, BoundedCallResultsKeepPhysicalSlotChecks) {
  auto Image = callSlotImage();
  Image.Segments.front().Data[16] = 0xe8;
  for (bool HasRange : {false, true})
    for (bool Negative : {false, true}) {
      auto Func = observedCallSlot();
      auto &Ops = Func.Blocks.front().Ops;
      // The saved word is above both call slots when the allocation only
      // lowers SP. A signed negative count can instead raise it into that word.
      Ops[2].Opcode = NdOp::INT_ADD;
      auto Result = temp(40);
      Result.Kind = MedVar::Reg;
      Result.RegOff = getTargetRegInfo(Image.Arch).IntReturnReg;
      auto Query = op(NdOp::CALL, Result, {MedVar::makeConst(0x2000, 8)});
      Query.Addr = 0x1008;
      auto Narrow = temp(41);
      Narrow.Size = 4;
      Ops[4].Addr = 0x1010;
      Ops[4].Inputs[0] = MedVar::makeConst(0x3000, 8);
      Ops[5].Addr = 0x1015;
      Ops.insert(Ops.begin() + 4,
                 {Query,
                  op(NdOp::SUBBYTES, Narrow, {Result, MedVar::makeConst(0, 4)}),
                  op(NdOp::INT_SEXT, temp(42), {Narrow}),
                  op(NdOp::INT_SUB, sp(2), {sp(1), temp(42)})});
      MedCallReturnRanges Ranges;
      if (HasRange)
        Ranges.try_emplace(
            0x2000, MedIntegerReturnRange{Image.Arch, 8, 0,
                                          Negative ? UINT32_MAX : UINT16_MAX});
      std::string Error;
      EXPECT_EQ(validateSourceCallReturnReads(
                    Func, Image, Error, limits::ir::kSourceCallReturnReadWork,
                    nullptr, &Ranges),
                HasRange && !Negative)
          << Error;
      if (!HasRange || Negative)
        EXPECT_NE(Error.find("physical CALL return slot"), std::string::npos);
    }
}

TEST(SourceCallReturnReads, NativeWidthExtentsDoNotHidePhysicalCallSlots) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    const auto Width = getTargetRegInfo(Architecture).PointerSize;
    const uint64_t Maximum = (uint64_t{1} << (Width * 8 - 1)) - 1;
    const uint64_t Mask = (Maximum << 1) | 1;
    for (bool WrapSlot : {false, true})
      for (bool WrapLoad : {false, true})
        for (bool HasRange : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << int(Architecture) << ':' << WrapSlot << ':'
                       << WrapLoad << ':' << HasRange);
          const uint64_t MaxCount = Maximum - 64 - (WrapSlot ? 0 : Width - 1);
          const uint64_t Address =
              WrapLoad ? Maximum - Width / 2 + 1 : Maximum - Width + 2;
          const unsigned ReadWidth = WrapLoad ? Width : 1;
          // Independently enumerate the bytes in native modular arithmetic.
          // SP itself fits in both cases; its push or the load can cross the
          // signed coordinate boundary and alias the other interval.
          const uint64_t CallEnd = (uint64_t{0} - 64 - MaxCount) & Mask;
          bool Overlaps = false;
          for (unsigned I = 0; I < Width; ++I)
            for (unsigned J = 0; J < ReadWidth; ++J)
              Overlaps |=
                  ((CallEnd - Width + I) & Mask) == ((Address + J) & Mask);
          ASSERT_EQ(Overlaps, WrapSlot || WrapLoad);

          auto Image = callSlotImage();
          Image.Arch = Architecture;
          Image.Segments.front().Data[16] = 0xe8;
          auto Func = observedCallSlot();
          Func.FrameSize = 64;
          auto Result = temp(40);
          Result.Kind = MedVar::Reg;
          Result.RegOff = getTargetRegInfo(Architecture).IntReturnReg;
          auto &Ops = Func.Blocks.front().Ops;
          Ops = {
              op(NdOp::COPY, sp(0), {sp(0)}),
              op(NdOp::INT_SUB, sp(1), {sp(0), MedVar::makeConst(64, Width)}),
              op(NdOp::CALL, Result, {MedVar::makeConst(0x2000, Width)}),
              op(NdOp::INT_SUB, sp(2), {sp(1), Result}),
              op(NdOp::INT_ADD, temp(41),
                 {sp(0), MedVar::makeConst(Address, Width)}),
              op(NdOp::STORE, none(), {temp(41), MedVar::makeConst(19, Width)}),
              op(NdOp::CALL, none(), {MedVar::makeConst(0x3000, Width)}),
              op(NdOp::LOAD, temp(42), {temp(41)}),
              op(NdOp::RETURN, none(), {temp(42)})};
          for (auto &Op : Ops) {
            Op.Output.TheArch = Architecture;
            Op.Output.Size = Width;
            for (unsigned I = 0; I < Op.NumInputs; ++I) {
              Op.Inputs[I].TheArch = Architecture;
              Op.Inputs[I].Size = Width;
            }
          }
          Ops[2].Addr = 0x1008;
          Ops[6].Addr = 0x1010;
          Ops[7].Addr = 0x1015;
          Ops[5].Inputs[1].Size = ReadWidth;
          Ops[7].Output.Size = ReadWidth;
          Ops[8].Inputs[0].Size = ReadWidth;
          MedCallReturnRanges Ranges;
          Ranges.try_emplace(
              0x2000, MedIntegerReturnRange{Architecture, Width, 0, MaxCount});
          std::string Error;
          const bool Safe = validateSourceCallReturnReads(
              Func, Image, Error, limits::ir::kSourceCallReturnReadWork,
              nullptr, HasRange ? &Ranges : nullptr);
          EXPECT_EQ(Safe, HasRange && !WrapSlot && !WrapLoad) << Error;
          if (!Safe)
            EXPECT_NE(Error.find(WrapLoad ? "native stack-offset"
                                          : "physical CALL return slot"),
                      std::string::npos)
                << Error;
        }
  }
}

TEST(SourceCallReturnReads, SparseStackWritesNeedOnlyOneOperationScan) {
  auto Func = observedCallSlot();
  auto &Ops = Func.Blocks.front().Ops;
  Ops[2].Inputs[1] = MedVar::makeConst(16, 8);
  Ops.insert(Ops.begin() + 4, 2048, op(NdOp::NOP, none(), {}));
  std::string Error;
  EXPECT_TRUE(validateSourceCallReturnReads(Func, callSlotImage(), Error, 4352))
      << Error;
  Ops[2].Inputs[1] = MedVar::makeConst(8, 8);
  EXPECT_FALSE(
      validateSourceCallReturnReads(Func, callSlotImage(), Error, 4352));
  EXPECT_NE(Error.find("physical CALL return slot"), std::string::npos)
      << Error;
}

TEST(SourceCallReturnReads, ManyCallsShareTheConvergedStackBasis) {
  for (bool Cycle : {false, true})
    for (bool Reversed : {false, true}) {
      SCOPED_TRACE(testing::Message() << Cycle << ":" << Reversed);
      auto Func = observedCallSlot();
      auto &Entry = Func.Blocks.front();
      const auto Call = Entry.Ops[4];
      const auto Load = Entry.Ops[5];
      Entry.Ops.resize(3);
      Entry.Ops[2].Opcode = NdOp::INT_ADD;
      Entry.Ops[2].Inputs[1] = MedVar::makeConst(16, 8);
      Entry.Succs = {1};
      constexpr int Blocks = 128;
      for (int I = 1; I <= Blocks; ++I) {
        MedBlock Block;
        Block.Id = I;
        Block.StartAddr = 0x2000 + I * 32;
        Block.Preds = {I - 1};
        if (I < Blocks)
          Block.Succs = {I + 1};
        Block.Ops.assign(24, op(NdOp::NOP, none(), {}));
        Block.Ops.push_back(Call);
        Block.Ops.push_back(Load);
        Func.Blocks.push_back(std::move(Block));
      }
      if (Cycle) {
        Func.Blocks.back().Succs = {1};
        Func.Blocks[1].Preds.push_back(Blocks);
      }
      if (Reversed)
        std::reverse(Func.Blocks.begin() + 1, Func.Blocks.end());
      std::string Error;
      EXPECT_TRUE(
          validateSourceCallReturnReads(Func, callSlotImage(), Error, 16384))
          << Error;
      // Sharing the stack analysis must retain the physical return-slot
      // hazard when the same graph actually reads that slot.
      Func.Blocks[0].Ops[2].Opcode = NdOp::INT_SUB;
      Func.Blocks[0].Ops[2].Inputs[1] = MedVar::makeConst(8, 8);
      EXPECT_FALSE(
          validateSourceCallReturnReads(Func, callSlotImage(), Error, 16384));
      EXPECT_NE(Error.find("physical CALL return slot"), std::string::npos)
          << Error;
    }
}

TEST(SourceCallReturnReads, BackedgeConflictsCannotPublishAProvisionalBasis) {
  for (bool Reversed : {false, true}) {
    auto Func = observedCallSlot();
    auto &Entry = Func.Blocks.front();
    const auto Call = Entry.Ops[4];
    const auto Load = Entry.Ops[5];
    Entry.Ops.resize(3);
    Entry.Ops[2].Opcode = NdOp::INT_ADD;
    Entry.Ops[2].Inputs[1] = MedVar::makeConst(16, 8);
    Entry.Succs = {1};
    MedBlock Loop;
    Loop.Id = 1;
    Loop.StartAddr = 0x2000;
    Loop.Preds = {0, 2};
    Loop.Succs = {2};
    Loop.Ops = {Call, Load};
    Func.Blocks.push_back(Loop);
    MedBlock Backedge;
    Backedge.Id = 2;
    Backedge.StartAddr = 0x3000;
    Backedge.Preds = {1};
    Backedge.Succs = {1};
    Backedge.Ops = {
        op(NdOp::INT_ADD, sp(2), {sp(1), MedVar::makeConst(24, 8)})};
    Func.Blocks.push_back(Backedge);
    if (Reversed)
      std::swap(Func.Blocks[1], Func.Blocks[2]);
    std::string Error;
    EXPECT_FALSE(validateSourceCallReturnReads(Func, callSlotImage(), Error));
    EXPECT_NE(Error.find("physical CALL return slot"), std::string::npos)
        << Error;
  }
}

TEST(SourceCallReturnReads, TailJumpsDoNotAcquireAPush) {
  std::string Error;
  EXPECT_TRUE(validateSourceCallReturnReads(observedCallSlot(),
                                            callSlotImage(0xe9), Error))
      << Error;
  EXPECT_FALSE(validateSourceCallReturnReads(observedCallSlot(),
                                             callSlotImage(0x90), Error));
}

TEST(SourceCallReturnReads, LocalByteWritesBoundExceptionalEntryObservations) {
  for (bool Incoming : {false, true})
    for (unsigned Bytes : {0u, 1u, 8u}) {
      SCOPED_TRACE(testing::Message() << Incoming << ":" << Bytes);
      auto Func = observedCallSlot();
      auto &Block = Func.Blocks.front();
      if (Incoming)
        Block.ExceptionalPreds.emplace_back(-1, Block.StartAddr);
      else
        Block.ExceptionalSuccs.emplace_back(-1, 0x3000);
      if (Bytes)
        Block.Ops.insert(
            Block.Ops.begin() + 5,
            op(NdOp::STORE, none(), {temp(20), MedVar::makeConst(29, Bytes)}));
      std::string Error;
      EXPECT_EQ(validateSourceCallReturnReads(Func, callSlotImage(), Error),
                Bytes == 8)
          << Error;
      // Reading before CALL is protected by the initial complete store,
      // including when dispatch can enter this block exceptionally.
      Func = observedCallSlot();
      if (Incoming)
        Func.Blocks[0].ExceptionalPreds.emplace_back(-1, Func.Entry);
      else
        Func.Blocks[0].ExceptionalSuccs.emplace_back(-1, 0x3000);
      std::swap(Func.Blocks[0].Ops[4], Func.Blocks[0].Ops[5]);
      EXPECT_TRUE(validateSourceCallReturnReads(Func, callSlotImage(), Error))
          << Error;
      // Without that local definition, an exceptional entry cannot inherit
      // the ordinary caller's frame history merely from its normal CFG.
      if (Incoming) {
        Func.Blocks[0].Ops.erase(Func.Blocks[0].Ops.begin() + 3);
        EXPECT_FALSE(
            validateSourceCallReturnReads(Func, callSlotImage(), Error));
        EXPECT_NE(Error.find("exceptional entry"), std::string::npos);
      }
    }
}

MedFunc exceptionalCallSlot(va_t Source) {
  auto Func = observedCallSlot();
  MedBlock Handler;
  Handler.Id = 1;
  Handler.StartAddr = 0x1020;
  Handler.Ops = {Func.Blocks[0].Ops[5], Func.Blocks[0].Ops[6]};
  Handler.Ops[0].Addr = Handler.StartAddr;
  auto &Entry = Func.Blocks[0];
  Entry.Ops.resize(5);
  Entry.Ops[3].Addr = 0x1006;
  Entry.Ops.push_back(
      op(NdOp::STORE, none(), {temp(20), MedVar::makeConst(29, 8)}));
  Entry.Ops.back().Addr = 0x100d;
  Entry.Ops.push_back(op(NdOp::COPY, temp(23), {MedVar::makeConst(31, 8)}));
  Entry.Ops.back().Addr = 0x1010;
  Entry.ExceptionalSuccs.emplace_back(Handler.Id, Handler.StartAddr,
                                      ExceptionalEdgeKind::SEHFilter, 0, -1,
                                      Source);
  Handler.ExceptionalPreds = Entry.ExceptionalSuccs;
  Handler.ExceptionalPreds[0].BlockId = Entry.Id;
  Func.Blocks.push_back(std::move(Handler));
  return Func;
}

TEST(SourceCallReturnReads, ExceptionalSourcesRetainEveryPossibleCallWrite) {
  for (va_t Source :
       {va_t{0x1006}, va_t{0x1008}, va_t{0x100d}, va_t{0x1010}, InvalidVA})
    for (unsigned Bytes : {0u, 4u, 8u}) {
      SCOPED_TRACE(testing::Message() << Source << ":" << Bytes);
      auto Func = exceptionalCallSlot(Source);
      if (Bytes)
        Func.Blocks[1].Ops.insert(
            Func.Blocks[1].Ops.begin(),
            op(NdOp::STORE, none(), {temp(20), MedVar::makeConst(37, Bytes)}));
      std::string Error;
      // The filter can observe a CALL push even if the throwing CALL never
      // returns. A later parent store helps only when it definitely completed
      // before the exception; a handler store kills just its own bytes.
      EXPECT_EQ(validateSourceCallReturnReads(Func, callSlotImage(), Error),
                Bytes == 8 || Source == 0x1006 || Source == 0x1010)
          << Error;
    }
}

TEST(SourceCallReturnReads, ExceptionalHistoryRequiresTheCurrentSourceEdge) {
  for (unsigned Change = 0; Change != 5; ++Change) {
    SCOPED_TRACE(Change);
    auto Func = exceptionalCallSlot(0x1010);
    auto &Entry = Func.Blocks[0];
    auto &Handler = Func.Blocks[1];
    switch (Change) {
    case 0:
      Entry.ExceptionalSuccs.clear();
      break;
    case 1:
      Handler.ExceptionalPreds.clear();
      break;
    case 2:
      Handler.ExceptionalPreds[0].SourceVA = 0x1006;
      break;
    case 3:
      Entry.ExceptionalSuccs[0].SourceVA = 0x1030;
      Handler.ExceptionalPreds[0].SourceVA = 0x1030;
      break;
    case 4:
      Entry.ExceptionalSuccs[0].TargetVA = 0x1030;
      Handler.ExceptionalPreds[0].TargetVA = 0x1030;
      break;
    }
    std::string Error;
    EXPECT_FALSE(validateSourceCallReturnReads(Func, callSlotImage(), Error));
    EXPECT_NE(Error.find("exceptional entry"), std::string::npos) << Error;
  }
}

TEST(SourceCallReturnReads, RestoredRegistrationStacksNeedFreshSourceFacts) {
  for (unsigned Change = 0; Change != 12; ++Change) {
    SCOPED_TRACE(Change);
    auto Func = observedCallSlot();
    auto &Ops = Func.Blocks[0].Ops;
    Ops[2].Inputs[1].ConstVal = 4;
    const auto Call = Ops[4], Load = Ops[5], Return = Ops[6];
    Ops.resize(4);
    Ops.push_back(op(NdOp::INT_SUB, temp(23),
                     {sp(1), MedVar::makeConst(Change == 11 ? 0 : 16, 8)}));
    Ops.push_back(
        op(NdOp::INT_ADD, temp(24), {sp(1), MedVar::makeConst(8, 8)}));
    Ops.push_back(op(NdOp::STORE, none(), {temp(24), temp(23)}));
    Ops.push_back(op(NdOp::LOAD, temp(25), {temp(24)}));
    Ops.push_back(op(NdOp::COPY, sp(2), {temp(25)}));
    Ops.insert(Ops.end(), {Call, Load, Return});
    for (size_t I = 0; I < Ops.size(); ++I) {
      auto &Op = Ops[I];
      Op.Addr = I == 9 ? 0x1008 : 0x1100 + I;
      Op.OriginSeq = 0;
      Op.Output.TheArch = Arch::X86;
      if (Op.Output.Size)
        Op.Output.Size = 4;
      for (unsigned K = 0; K < Op.NumInputs; ++K) {
        Op.Inputs[K].TheArch = Arch::X86;
        Op.Inputs[K].Size = 4;
      }
    }
    LowFunc Source;
    Source.Entry = Func.Entry;
    Source.ExceptionMetadata.emplace();
    auto &EH = *Source.ExceptionMetadata;
    EH.CodeRange = {Func.Entry, 0x1200};
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    EH.Registration.emplace();
    Source.RegistrationStates.emplace();
    auto &States = *Source.RegistrationStates;
    States.Complete = States.CallbackStatesComplete =
        States.RegistrationLifetimeComplete = true;
    States.FrameValues = {{Ops[1].Addr, 0, -28},
                          {Ops[8].Addr, 0, Change == 11 ? -28 : -44}};
    Source.Blocks.emplace_back();
    auto ToLow = [](const MedVar &V) {
      return V.isConst()             ? NdVar::cst(V.ConstVal, V.Size)
             : V.Kind == MedVar::Reg ? NdVar::reg(V.RegOff, V.Size)
                                     : NdVar::tmp(V.Id, V.Size);
    };
    for (const auto &Op : Ops) {
      LowOp Original;
      Original.Addr = Op.Addr;
      Original.Seq = Op.OriginSeq;
      Original.Opcode = Op.Opcode;
      Original.Output = ToLow(Op.Output);
      for (unsigned K = 0; K < Op.NumInputs; ++K)
        Original.addInput(ToLow(Op.Inputs[K]));
      Source.Blocks[0].Ops.push_back(Original);
    }
    auto Image = callSlotImage();
    Image.Arch = Arch::X86;
    Image.Format = BinaryFormat::COFF;
    switch (Change) {
    case 2:
      States.Complete = false;
      break;
    case 3:
      States.CallbackStatesComplete = false;
      break;
    case 4:
      States.RegistrationLifetimeComplete = false;
      break;
    case 5:
      Ops[8].OriginSeq = 1;
      break;
    case 6:
      Source.Blocks[0].Ops[8].Opcode = NdOp::INT_ZEXT;
      break;
    case 7:
      Source.Blocks[0].Ops[9].Inputs[0].Offset += 4;
      break;
    case 8:
      EH.Registration->RealignedFrame.emplace();
      break;
    case 9:
      Image.Format = BinaryFormat::ELF;
      break;
    case 10:
      States.FrameValues[1].EstablishedFrameOffset.reset();
      break;
    }
    std::string Error;
    EXPECT_EQ(validateSourceCallReturnReads(
                  Func, Image, Error, limits::ir::kSourceCallReturnReadWork,
                  Change == 1 ? nullptr : &Source),
              Change == 0)
        << Error;
  }
}

/// Convert an x64-shaped fixture to PE32 widths, keeping narrower stores.
void useX86Widths(MedFunc &Func) {
  for (auto &Block : Func.Blocks)
    for (auto &Op : Block.Ops) {
      Op.Output.TheArch = Arch::X86;
      if (Op.Output.Size)
        Op.Output.Size = 4;
      for (unsigned K = 0; K < Op.NumInputs; ++K) {
        Op.Inputs[K].TheArch = Arch::X86;
        if (Op.Inputs[K].Size == 8)
          Op.Inputs[K].Size = 4;
      }
    }
}

/// The source of an EH3/EH4 parent whose registration-state proof is
/// incomplete.
LowFunc incompleteRegistration(va_t Entry, ExceptionEncoding Encoding,
                               int32_t SeededLevel) {
  LowFunc Source;
  Source.Entry = Entry;
  Source.ExceptionMetadata.emplace();
  auto &EH = *Source.ExceptionMetadata;
  EH.CodeRange = {Entry, 0x1200};
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Encoding = Encoding;
  EH.Registration.emplace();
  EH.Registration->HandlerVA = 0x2800;
  EH.Registration->ScopeTableVA = 0x3000;
  EH.Registration->SeededTryLevel = SeededLevel;
  Source.RegistrationStates.emplace();
  return Source;
}

/// An EH4 prologue pushes the record (seeded level, scope table, handler)
/// with its node at entry-20, saves the stack in SavedESP at entry-28, and
/// reloads it before a CALL. A local at entry-36 is read after the CALL.
MedFunc savedStackReload(unsigned Change) {
  auto Func = observedCallSlot();
  auto &Ops = Func.Blocks[0].Ops;
  Ops[2].Inputs[1].ConstVal = 4;
  const auto Call = Ops[4], Load = Ops[5], Return = Ops[6];
  Ops.resize(4);
  auto At = [&](int64_t Offset, int Temp) {
    Ops.push_back(op(NdOp::INT_ADD, temp(Temp),
                     {sp(1), MedVar::makeConst(uint64_t(Offset), 8)}));
  };
  const auto Push = [&](int64_t Offset, int Temp, uint64_t Value) {
    At(Offset, Temp);
    Ops.push_back(
        op(NdOp::STORE, none(), {temp(Temp), MedVar::makeConst(Value, 8)}));
  };
  Push(24, 30, Change == 6 || Change == 9 ? 0xffffffffu : 0xfffffffeu);
  Push(20, 31, 0x3000);
  Push(16, 32, Change == 5 ? 0x2804 : 0x2800);
  Ops.push_back(op(NdOp::INT_SUB, temp(23), {sp(1), MedVar::makeConst(16, 8)}));
  At(4, 24);
  Ops.push_back(
      op(NdOp::STORE, none(),
         {temp(24), Change == 7 ? MedVar::makeConst(0x500, 8) : temp(23)}));
  if (Change == 3) {
    Ops.push_back(
        op(NdOp::INT_SUB, temp(26), {sp(1), MedVar::makeConst(20, 8)}));
    Ops.push_back(op(NdOp::STORE, none(), {temp(24), temp(26)}));
  }
  if (Change == 4)
    Ops.push_back(op(NdOp::STORE, none(), {temp(24), MedVar::makeConst(0, 2)}));
  if (Change == 8)
    Ops.push_back(op(NdOp::ATOMIC_XCHG, temp(27), {temp(24), temp(23)}));
  Ops.push_back(op(NdOp::LOAD, temp(25), {temp(24)}));
  Ops.push_back(op(NdOp::COPY, sp(2), {temp(25)}));
  Ops.insert(Ops.end(), {Call, Load, Return});
  for (size_t I = 0; I < Ops.size(); ++I) {
    Ops[I].Addr = Ops[I].Opcode == NdOp::CALL ? 0x1008 : 0x1100 + I;
    Ops[I].OriginSeq = 0;
  }
  useX86Widths(Func);
  return Func;
}

TEST(SourceCallReturnReads, RegistrationProtocolPlacesSavedStackReloads) {
  for (unsigned Change = 0; Change != 10; ++Change) {
    SCOPED_TRACE(Change);
    const auto Func = savedStackReload(Change);
    auto Image = callSlotImage();
    Image.Arch = Arch::X86;
    Image.Format = BinaryFormat::COFF;
    auto Source = incompleteRegistration(
        Func.Entry,
        Change == 9 ? ExceptionEncoding::X86ScopeTableEH3
                    : ExceptionEncoding::X86ScopeTableEH4,
        Change == 9 ? -1 : -2);
    if (Change == 2)
      Source.RegistrationStates->Complete =
          Source.RegistrationStates->RegistrationLifetimeComplete = true;
    // Without a complete state proof, the reload holds the saved entry-48
    // only by the frame protocol: one exact stack value saved in the word
    // below the node of the pushed record. Then the CALL's slot ends at
    // entry-48, below the local.
    std::string Error;
    const bool Proved = validateSourceCallReturnReads(
        Func, Image, Error, limits::ir::kSourceCallReturnReadWork,
        Change == 1 ? nullptr : &Source);
    EXPECT_EQ(Proved, Change == 0 || Change == 9) << Error;
    if (!Proved)
      EXPECT_NE(Error.find("physical CALL return slot"), std::string::npos)
          << Error;
  }
}

/// A filter entered by the runtime: ESP and EBP are its stack and frame
/// roots. It pushes two words, CALLs, then reads the parent's words at
/// entry-12 and entry+4 through EBP. A slot taken for an entry-relative push
/// (entry-12, or entry+4 above the root) would cover one of them.
MedFunc runtimeCallback(unsigned Change) {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "callback_slot";
  Func.FrameSize = 40;
  const auto Frame = [](int Version) { return reg(4, reg::RBP, Version); };
  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = Func.Entry;
  Entry.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
               op(NdOp::INT_SUB, sp(1), {sp(0), MedVar::makeConst(32, 8)}),
               op(NdOp::COPY, temp(50), {MedVar::makeConst(7, 8)}),
               op(NdOp::RETURN, none(), {})};
  Entry.Ops[0].Addr = 0x1000;
  Entry.Ops[1].Addr = 0x1001;
  Entry.Ops[2].Addr = 0x1010;
  Entry.Ops[3].Addr = 0x1011;
  MedBlock Filter;
  Filter.Id = 1;
  Filter.StartAddr = 0x1040;
  Filter.Ops = {
      op(NdOp::COPY, sp(7), {sp(0)}),
      op(NdOp::COPY, Frame(3), {Frame(0)}),
      op(Change == 2 ? NdOp::INT_ADD : NdOp::INT_SUB, sp(8),
         {sp(7), MedVar::makeConst(8, 8)}),
      op(NdOp::CALL, temp(51), {MedVar::makeConst(0x2000, 8)}),
      op(NdOp::INT_SUB, temp(40), {Frame(3), MedVar::makeConst(8, 8)}),
      op(NdOp::LOAD, temp(41), {temp(40)}),
      op(NdOp::INT_ADD, temp(42), {Frame(3), MedVar::makeConst(8, 8)}),
      op(NdOp::LOAD, temp(43), {temp(42)}),
      op(NdOp::INT_ADD, temp(44), {temp(41), temp(43)}),
      op(NdOp::RETURN, none(), {temp(44)})};
  const va_t Addresses[] = {0x1040, 0x1040, 0x1044, 0x1048, 0x104d,
                            0x1050, 0x1053, 0x1056, 0x1059, 0x105c};
  for (size_t I = 0; I < Filter.Ops.size(); ++I)
    Filter.Ops[I].Addr = Addresses[I];
  Filter.Ops[0].RegistrationRoot =
      MedOp::RegistrationRootKind::CallbackStackPointer;
  Filter.Ops[1].RegistrationRoot =
      MedOp::RegistrationRootKind::EstablishedFramePointer;
  for (auto *Root : {&Filter.Ops[0], &Filter.Ops[1]})
    Root->OriginSeq = Change == 1 && Root == &Filter.Ops[0] ? 0 : -1;
  Entry.ExceptionalSuccs.emplace_back(Filter.Id, Filter.StartAddr,
                                      ExceptionalEdgeKind::SEHFilter, 0, -1,
                                      0x1010);
  Filter.ExceptionalPreds = Entry.ExceptionalSuccs;
  Filter.ExceptionalPreds[0].BlockId = Entry.Id;
  Func.Blocks.push_back(std::move(Entry));
  Func.Blocks.push_back(std::move(Filter));
  useX86Widths(Func);
  return Func;
}

TEST(SourceCallReturnReads, RuntimeCallbackStacksStayBelowTheParentFrame) {
  for (unsigned Change = 0; Change != 4; ++Change) {
    SCOPED_TRACE(Change);
    const auto Func = runtimeCallback(Change);
    auto Image = callSlotImage();
    Image.Arch = Arch::X86;
    Image.Format = BinaryFormat::COFF;
    Image.Segments[0].Size = 128;
    Image.Segments[0].Data.resize(128, 0x90);
    Image.Segments[0].Data[0x48] = 0xe8;
    const auto Source = incompleteRegistration(
        Func.Entry, ExceptionEncoding::X86ScopeTableEH4, -2);
    // The runtime runs the filter on its dispatcher stack, below the parent
    // frame, so a CALL at or below the filter's stack root cannot write the
    // parent's words. A CALL above that root, or a stack without the checked
    // root, has no such separation.
    std::string Error;
    EXPECT_EQ(validateSourceCallReturnReads(
                  Func, Image, Error, limits::ir::kSourceCallReturnReadWork,
                  Change == 3 ? nullptr : &Source),
              Change == 0)
        << Error;
  }
}

TEST(SourceCallReturnReads, AStoreOnAnotherPathDoesNotHideACall) {
  auto Func = observedCallSlot();
  auto &Entry = Func.Blocks[0];
  const auto Call = Entry.Ops[4];
  const auto Load = Entry.Ops[5];
  const auto Return = Entry.Ops[6];
  const auto Store = Entry.Ops[3];
  Entry.Ops.resize(4);
  Entry.Succs = {1, 2};
  MedBlock Left, Right, Join;
  Left.Id = 1;
  Left.Preds = {0};
  Left.Succs = {3};
  Left.Ops = {Call};
  Right.Id = 2;
  Right.Preds = {0};
  Right.Succs = {3};
  Right.Ops = {Store};
  Join.Id = 3;
  Join.Preds = {1, 2};
  Join.Ops = {Load, Return};
  Func.Blocks.insert(Func.Blocks.end(), {Left, Right, Join});
  std::string Error;
  EXPECT_FALSE(validateSourceCallReturnReads(Func, callSlotImage(), Error));
  Func.Blocks[1].Ops.push_back(Store);
  EXPECT_TRUE(validateSourceCallReturnReads(Func, callSlotImage(), Error))
      << Error;
}

TEST(SourceCallReturnReads, DistinctSlotsShareOneBackwardHistory) {
  // A CALL at an unknown stack position may write any slot. Each of many
  // slots is written again after it and read only after a long chain of
  // blocks, so every slot's history crosses the whole chain. Those histories
  // must cross it together, not once per observed byte.
  constexpr int Slots = 64, Chain = 512;
  for (int Missing : {-1, 0, 41}) {
    SCOPED_TRACE(Missing);
    auto Func = observedCallSlot();
    auto &Entry = Func.Blocks[0];
    const auto Call = Entry.Ops[4];
    Entry.Ops.resize(2);
    Entry.Ops[1].Inputs[1] = MedVar::makeConst(8 * Slots + 32, 8);
    std::vector<MedOp> Loads;
    for (int Slot = 0; Slot < Slots; ++Slot) {
      Entry.Ops.push_back(op(NdOp::INT_ADD, temp(100 + Slot),
                             {sp(1), MedVar::makeConst(8 * Slot, 8)}));
      Loads.push_back(op(NdOp::LOAD, temp(300 + Slot), {temp(100 + Slot)}));
      Loads.back().Addr = 0x2000 + Slot;
    }
    Entry.Ops.push_back(
        op(NdOp::COPY, sp(2), {MedVar::makeUnspecified(8, Arch::X64)}));
    Entry.Ops.push_back(Call);
    for (int Slot = 0; Slot < Slots; ++Slot)
      if (Slot != Missing)
        Entry.Ops.push_back(op(NdOp::STORE, none(),
                               {temp(100 + Slot), MedVar::makeConst(Slot, 8)}));
    Entry.Succs = {1};
    for (int Id = 1; Id <= Chain; ++Id) {
      MedBlock Block;
      Block.Id = Id;
      Block.StartAddr = 0x3000 + Id;
      Block.Preds = {Id - 1};
      if (Id < Chain)
        Block.Succs = {Id + 1};
      Func.Blocks.push_back(std::move(Block));
    }
    auto &Reads = Func.Blocks.back().Ops;
    Reads.insert(Reads.end(), Loads.begin(), Loads.end());
    Reads.push_back(op(NdOp::RETURN, none(), {temp(300)}));
    std::string Error;
    EXPECT_EQ(
        validateSourceCallReturnReads(Func, callSlotImage(), Error, 16384),
        Missing < 0)
        << Error;
    if (Missing >= 0)
      EXPECT_NE(Error.find("load at 0x" + llvm::utohexstr(0x2000 + Missing) +
                           " observes a physical CALL return slot from 0x1008"),
                std::string::npos)
          << Error;
  }
}

TEST(SourceCallReturnReads, ALaterCallCanReachTheNextLoopIteration) {
  auto Func = observedCallSlot();
  auto &Entry = Func.Blocks[0];
  const auto Call = Entry.Ops[4];
  const auto Load = Entry.Ops[5];
  Entry.Ops.resize(4);
  Entry.Succs = {1};
  MedBlock Loop;
  Loop.Id = 1;
  Loop.Preds = {0, 1};
  Loop.Succs = {1};
  Loop.Ops = {Load, Call};
  Func.Blocks.push_back(Loop);
  std::string Error;
  EXPECT_FALSE(validateSourceCallReturnReads(Func, callSlotImage(), Error));
}

TEST(SourceCallReturnReads, BalancedLoopCallsSeparateOnlyProvenLocalBytes) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool Drifts : {false, true})
      for (int Slot : {8, -8, -24}) {
        SCOPED_TRACE(testing::Message() << static_cast<int>(Format) << ":"
                                        << Drifts << ":" << Slot);
        auto Func = observedCallSlot();
        auto Image = callSlotImage();
        Image.Format = Format;
        Image.Segments[0].Data[16] = 0xe8;
        Image.Segments[0].Data[24] = 0xe8;
        auto &Entry = Func.Blocks[0];
        auto Load = Entry.Ops[5];
        Load.Addr = 0x1008;
        auto Call = Entry.Ops[4];
        Call.Addr = 0x1010;
        auto RestoredCall = Call;
        RestoredCall.Addr = 0x1018;
        RestoredCall.Output = temp(23);
        Entry.Ops.resize(4);
        Entry.Ops[2] =
            op(NdOp::INT_ADD, temp(20), {sp(1), MedVar::makeConst(Slot, 8)});
        Entry.Succs = {1};
        MedBlock Loop;
        Loop.Id = 1;
        Loop.StartAddr = 0x1008;
        Loop.Preds = {0, 1};
        Loop.Succs = {1};
        PhiNode Stack;
        Stack.Output = sp(2);
        Stack.Args = {{0, sp(1)}, {1, sp(4)}};
        Loop.Phis.push_back(std::move(Stack));
        Loop.Ops = {Load,
                    op(NdOp::INT_SUB, sp(3), {sp(2), MedVar::makeConst(16, 8)}),
                    Call,
                    op(NdOp::INT_ADD, sp(4),
                       {sp(3), MedVar::makeConst(Drifts ? 24 : 16, 8)}),
                    RestoredCall};
        Func.Blocks.push_back(std::move(Loop));
        std::string Error;
        EXPECT_EQ(validateSourceCallReturnReads(Func, Image, Error),
                  !Drifts && Slot == 8)
            << Error;
      }
}

TEST(SourceCallReturnReads,
     MissingStackAndMalformedGraphsDoNotGrantSeparation) {
  auto Func = observedCallSlot();
  Func.Blocks[0].Ops.insert(
      Func.Blocks[0].Ops.begin() + 4,
      op(NdOp::COPY, sp(2), {MedVar::makeUnspecified(8, Arch::X64)}));
  std::string Error;
  EXPECT_FALSE(validateSourceCallReturnReads(Func, callSlotImage(), Error));
  Func = observedCallSlot();
  Func.Blocks[0].Preds = {99};
  EXPECT_FALSE(validateSourceCallReturnReads(Func, callSlotImage(), Error));
  EXPECT_NE(Error.find("complete normal"), std::string::npos);
}

TEST(SourceCallReturnReads, LinkRegisterCallsDoNotWriteAStackSlot) {
  for (Arch Architecture : {Arch::ARM, Arch::AArch64}) {
    auto Image = callSlotImage();
    Image.Arch = Architecture;
    std::string Error;
    EXPECT_TRUE(validateSourceCallReturnReads(observedCallSlot(), Image, Error))
        << Error;
  }
}

TEST(SourceCallReturnReads,
     BudgetExhaustionNeverPublishesAnUncheckedProjection) {
  std::string Error;
  EXPECT_FALSE(validateSourceCallReturnReads(observedCallSlot(),
                                             callSlotImage(), Error, 0));
  EXPECT_NE(Error.find("work budget"), std::string::npos);
  EXPECT_TRUE(validateSourceCallReturnReads(observedCallSlot(),
                                            callSlotImage(0xe9), Error));
  EXPECT_TRUE(Error.empty());
}

TEST(SourceCallReturnReads, DeferredLoadsWaitForTheSettledObservation) {
  auto Func = observedCallSlot();
  Func.Blocks[0].Ops[5].OriginSeq = 7;
  std::vector<MedCallReturnRead> Deferred;
  std::string Error;
  EXPECT_TRUE(validateSourceCallReturnReads(
      Func, callSlotImage(), Error, limits::ir::kSourceCallReturnReadWork,
      nullptr, nullptr, &Deferred))
      << Error;
  ASSERT_EQ(Deferred.size(), 1u);
  EXPECT_EQ(Deferred[0].Load, 0x100du);
  EXPECT_EQ(Deferred[0].OriginSeq, 7);
  EXPECT_NE(Deferred[0].Diagnostic.find("physical CALL return slot"),
            std::string::npos);
  // Other refusals do not wait.
  EXPECT_FALSE(validateSourceCallReturnReads(Func, callSlotImage(), Error, 0,
                                             nullptr, nullptr, &Deferred));
  EXPECT_NE(Error.find("work budget"), std::string::npos);
  // The function returns the loaded value, so it is still refused, and the
  // diagnostic names what observes it.
  Func.CallReturnReads = {Deferred[0]};
  EXPECT_FALSE(validateObservedCallReturnReads(Func, callSlotImage(), Error));
  EXPECT_EQ(Error.rfind(Deferred[0].Diagnostic + "; ", 0), 0u) << Error;
  EXPECT_NE(Error.find("the RETURN at 0x0 returns the value"),
            std::string::npos)
      << Error;
}

TEST(SourceCallReturnReads, EachDeferredLoadLeavesTheProofOnce) {
  // Two loads after the CALL read its slot; one on the other path reads the
  // same bytes after a store rewrote them. The proof records each of the two
  // once, and what it then proves about the third stands.
  auto Func = observedCallSlot();
  auto &Entry = Func.Blocks[0];
  const auto Store = Entry.Ops[3];
  const auto Call = Entry.Ops[4];
  auto Load = Entry.Ops[5];
  Entry.Ops.resize(4);
  Entry.Succs = {1, 2};
  MedBlock Called, Rewritten;
  Called.Id = 1;
  Called.StartAddr = 0x1008;
  Called.Preds = {0};
  Called.Ops = {Call, Load};
  Load.Output = temp(23);
  Load.Addr = 0x1010;
  Called.Ops.push_back(Load);
  Called.Ops.push_back(op(NdOp::RETURN, none(), {temp(22)}));
  Rewritten.Id = 2;
  Rewritten.StartAddr = 0x1020;
  Rewritten.Preds = {0};
  Load.Output = temp(24);
  Load.Addr = 0x1024;
  Rewritten.Ops = {Store, Load, op(NdOp::RETURN, none(), {temp(24)})};
  Func.Blocks.insert(Func.Blocks.end(), {Called, Rewritten});
  std::string Error;
  EXPECT_FALSE(validateSourceCallReturnReads(Func, callSlotImage(), Error));
  EXPECT_NE(Error.find("load at 0x100D observes"), std::string::npos) << Error;
  std::vector<MedCallReturnRead> Deferred;
  EXPECT_TRUE(validateSourceCallReturnReads(
      Func, callSlotImage(), Error, limits::ir::kSourceCallReturnReadWork,
      nullptr, nullptr, &Deferred))
      << Error;
  std::vector<va_t> Loads;
  for (const MedCallReturnRead &Read : Deferred)
    Loads.push_back(Read.Load);
  std::sort(Loads.begin(), Loads.end());
  EXPECT_EQ(Loads, (std::vector<va_t>{0x100d, 0x1010}));
  // Without the rewrite, the third is recorded as well.
  Func.Blocks[2].Ops.erase(Func.Blocks[2].Ops.begin());
  Func.Blocks[2].Ops.insert(Func.Blocks[2].Ops.begin(), Call);
  Deferred.clear();
  EXPECT_TRUE(validateSourceCallReturnReads(
      Func, callSlotImage(), Error, limits::ir::kSourceCallReturnReadWork,
      nullptr, nullptr, &Deferred))
      << Error;
  EXPECT_EQ(Deferred.size(), 3u);
}

/// GCC's `sub rsp, 8; push arg; call; pop rax; pop rdx` stack-argument
/// cleanup in MedIR: the second pop reads padding an earlier CALL's return
/// address was written to, into RDX, which only a join then carries on to a
/// call that does not take it.
MedFunc poppedArgumentPadding() {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.Name = "popped_padding";
  Func.CC = CallingConv::SysV_AMD64;
  Func.FrameSize = 40;
  const auto RDX = [](int Version) { return reg(40, x86reg::RDX, Version); };
  const auto RDI = [](int Version) { return reg(41, x86reg::RDI, Version); };
  MedBlock Entry;
  Entry.Id = 0;
  Entry.StartAddr = Func.Entry;
  Entry.Succs = {1};
  Entry.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
               op(NdOp::INT_SUB, sp(1), {sp(0), MedVar::makeConst(32, 8)}),
               op(NdOp::INT_SUB, temp(20), {sp(1), MedVar::makeConst(8, 8)}),
               op(NdOp::CALL, temp(21), {MedVar::makeConst(0x2000, 8)}),
               op(NdOp::LOAD, temp(22), {temp(20)}),
               op(NdOp::COPY, RDX(1), {temp(22)}),
               op(NdOp::BRANCH, none(), {MedVar::makeConst(0x1020, 8)})};
  Entry.Ops[3].Addr = 0x1008;
  Entry.Ops[4].Addr = 0x100d;
  Entry.Ops[4].OriginSeq = 7;
  Entry.Ops[5].Addr = 0x100d;
  Entry.Ops[5].OriginSeq = 8;
  MedBlock Join;
  Join.Id = 1;
  Join.StartAddr = 0x1020;
  Join.Preds = {0};
  PhiNode Carried;
  Carried.Output = RDX(2);
  Carried.Args = {{0, RDX(1)}};
  Join.Phis.push_back(std::move(Carried));
  Join.Ops = {op(NdOp::COPY, RDI(1), {MedVar::makeConst(5, 8)}),
              op(NdOp::CALL, none(), {MedVar::makeConst(0x3000, 8), RDI(1)}),
              op(NdOp::RETURN, none(), {})};
  Join.Ops[1].Addr = 0x1028;
  Func.Blocks = {std::move(Entry), std::move(Join)};
  MedCallInfo Call;
  Call.BlockId = 1;
  Call.OpIdx = 1;
  Call.TargetAddr = 0x3000;
  Call.Args = {RDI(1)};
  Func.CallInfos.push_back(std::move(Call));
  Func.CallReturnReads = {{0x100d, 7, "observed padding"}};
  return Func;
}

TEST(SourceCallReturnReads, ACleanupPopObservedByNothingIsNotRefused) {
  auto Func = poppedArgumentPadding();
  Func.CallReturnReads.clear();
  std::string Error;
  EXPECT_TRUE(validateSourceCallReturnReads(
      Func, callSlotImage(), Error, limits::ir::kSourceCallReturnReadWork,
      nullptr, nullptr, &Func.CallReturnReads))
      << Error;
  ASSERT_EQ(Func.CallReturnReads.size(), 1u);
  EXPECT_TRUE(validateObservedCallReturnReads(Func, callSlotImage(), Error))
      << Error;
  EXPECT_TRUE(Error.empty());
}

TEST(SourceCallReturnReads, SettledReadersCallsAndReturnsDecideObservation) {
  const auto RDX = [](int Version) { return reg(40, x86reg::RDX, Version); };
  const uint32_t KeepsRDX = 1u << *gprFamilyOf(Arch::X64, x86reg::RDX);
  for (unsigned Mode = 0; Mode != 21; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Func = poppedArgumentPadding();
    auto &Join = Func.Blocks[1];
    auto &Call = Join.Ops[1];
    bool Observed = false;
    // What the diagnostic names as the observer.
    std::string Because;
    // What each callee of this run is passed, by entry, when known.
    std::map<va_t, unsigned> Arity;
    size_t Work = limits::ir::kSourceCallReturnReadWork;
    switch (Mode) {
    case 1: // the function returns what was loaded
      Join.Ops[2].addInput(temp(22));
      Observed = true;
      break;
    case 2: // the settled call passes it
      Func.CallInfos[0].Args.push_back(RDX(2));
      Observed = true;
      Because = "the call at 0x1028 takes the value as an argument";
      break;
    case 3: // no settled arguments: the call may still read RDX
      Func.CallInfos.clear();
      Observed = true;
      Because = "the call at 0x1028, whose arguments are not settled, may "
                "read the value in ";
      break;
    case 4: // RDX survives the call, but a single result never reads it
      Call.CallPreservedGPRs = KeepsRDX;
      break;
    case 5: // ... unless RDX is half of the returned pair
      Call.CallPreservedGPRs = KeepsRDX;
      Func.MultiReturn = {{x86reg::RAX, 8, false}, {x86reg::RDX, 8, false}};
      Observed = true;
      break;
    case 6:  // the join's register is used before the call
    case 17: // a copy of it that nothing reads only moves it
      Join.Ops.insert(
          Join.Ops.begin(),
          Mode == 6
              ? op(NdOp::INT_ADD, temp(23), {RDX(2), MedVar::makeConst(1, 8)})
              : op(NdOp::COPY, reg(42, x86reg::RSI, 1), {RDX(2)}));
      Func.CallInfos[0].OpIdx = 2;
      Observed = Mode == 6;
      break;
    case 7: // no surviving operation performs the load
      Func.Blocks[0].Ops[4].Dead = true;
      break;
    case 8: // an unfinished proof cannot clear the load
      Work = 0;
      Observed = true;
      break;
    case 9: // an exception may leave with RDX still holding the value
      Join.ExceptionalSuccs.push_back(ExceptionalEdge{});
      Observed = true;
      Because = "an exception may leave 0x1020 with the value in ";
      break;
    case 10: // a complete overwrite ends RDX's copy before an unsettled call
      Func.CallInfos.clear();
      Join.Ops.insert(Join.Ops.begin(),
                      op(NdOp::COPY, RDX(3), {MedVar::makeConst(0, 8)}));
      break;
    case 11: // a partial overwrite does not
      Func.CallInfos.clear();
      Join.Ops.insert(Join.Ops.begin(), op(NdOp::COPY, reg(43, x86reg::RDX, 4),
                                           {MedVar::makeConst(0, 4)}));
      Join.Ops[0].Output.Size = 4;
      Observed = true;
      break;
    case 12: // System V publishes a summarized callee's registers instead
      Func.CallInfos.clear();
      Call.CalleeRegisterArgs = 1;
      break;
    case 13: // EDX only views the loaded bytes
    case 14: // ... which an operation then uses
      Func.Blocks[0].Ops.insert(Func.Blocks[0].Ops.begin() + 6,
                                op(NdOp::SUBBYTES, reg(44, x86reg::RDX, 1),
                                   {temp(22), MedVar::makeConst(0, 4)}));
      Func.Blocks[0].Ops[6].Output.Size = 4;
      if (Mode == 14) {
        Func.Blocks[0].Ops.insert(
            Func.Blocks[0].Ops.begin() + 7,
            op(NdOp::INT_ADD, temp(23),
               {Func.Blocks[0].Ops[6].Output, MedVar::makeConst(1, 4)}));
        Func.Blocks[0].Ops[7].Addr = 0x100f;
        Observed = true;
        Because = "the operation at 0x100F uses the value";
      }
      break;
    case 15: // another version of RDX is another value
    case 16: // another variable over RDX's bytes may view it
      Join.Ops.insert(Join.Ops.begin(),
                      op(NdOp::STORE, none(),
                         {Mode == 15 ? RDX(0) : reg(45, x86reg::RDX, 1),
                          MedVar::makeConst(1, 8)}));
      Join.Ops[0].Addr = 0x1022;
      Func.CallInfos[0].OpIdx = 2;
      Observed = Mode == 16;
      if (Observed)
        Because = "the operation at 0x1022 reads ";
      break;
    case 18: // a callee of this run taking two integer arguments: RDX is none
    case 19: // ... or three: RDX, the third, is one
    case 20: // a bound for another function says nothing of this callee
      Func.CallInfos.clear();
      Arity = {{Mode == 20 ? 0x4000 : 0x3000, Mode == 19 ? 3u : 2u}};
      Observed = Mode != 18;
      Because = Mode == 19 ? "the call at 0x1028 passes 3 integer arguments, "
                             "the value in "
                           : "the call at 0x1028, whose arguments are not "
                             "settled, may read the value in ";
      break;
    default:
      break;
    }
    std::string Error;
    EXPECT_EQ(validateObservedCallReturnReads(Func, callSlotImage(), Error,
                                              Work, &Arity),
              !Observed)
        << Error;
    if (Observed) {
      EXPECT_EQ(Error.rfind("observed padding; ", 0), 0u) << Error;
      EXPECT_EQ(Error.find("work budget") != std::string::npos, Mode == 8)
          << Error;
      EXPECT_NE(Error.find(Because), std::string::npos) << Error;
    }
  }
}

TEST(SourceCallReturnReads, NarrowTargetsReturnTheSecondIntegerRegister) {
  // RDX itself reaches the RETURN.
  auto Func = poppedArgumentPadding();
  Func.Blocks[1].Ops.erase(Func.Blocks[1].Ops.begin() + 1);
  Func.CallInfos.clear();
  auto Image = callSlotImage();
  std::string Error;
  // A 64-bit target returns a register pair only as MultiReturn.
  EXPECT_TRUE(validateObservedCallReturnReads(Func, Image, Error)) << Error;
  // An i386 register pair returns a 64-bit result in EDX:EAX.
  Image.Arch = Arch::X86;
  EXPECT_FALSE(validateObservedCallReturnReads(Func, Image, Error));
}

TEST(SourceFrameBorrows, ReadExtentsAndEscapesBoundPrivateStoreRemoval) {
  for (unsigned Case = 0; Case != 22; ++Case) {
    SCOPED_TRACE(Case);
    MedFunc Caller;
    Caller.Entry = 0x1000;
    Caller.CC = CallingConv::Win64;
    Caller.FrameSize = 0x38;
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Caller.Entry;
    auto Word = temp(20);
    Word.Size = 4;
    Block.Ops = {
        op(NdOp::COPY, sp(0), {sp(0)}),
        op(NdOp::INT_SUB, sp(1), {sp(0), MedVar::makeConst(0x38, 8)}),
        op(NdOp::INT_ADD, temp(10), {sp(1), MedVar::makeConst(0x20, 8)}),
        op(NdOp::INT_ADD, temp(11), {sp(1), MedVar::makeConst(0x24, 8)}),
        op(NdOp::STORE, none(),
           {temp(10), MedVar::makeUnspecified(4, Arch::X64)}),
        op(NdOp::STORE, none(), {temp(11), MedVar::makeConst(37, 4)}),
        op(NdOp::CALL, Word, {MedVar::makeConst(0x2000, 8), temp(11)}),
        op(NdOp::INT_ADD, sp(2), {sp(1), MedVar::makeConst(0x38, 8)}),
        op(NdOp::RETURN, none(), {Word})};
    Block.Ops[6].CalleeRegisterArgs = 1;
    Block.Ops[6].CalleeStackArgs = 0;
    Block.Ops[6].CalleeVectorArgs = 0;
    Caller.Blocks.push_back(std::move(Block));
    MedCallInfo Call;
    Call.BlockId = 0;
    Call.OpIdx = 6;
    Call.TargetAddr = 0x2000;
    Call.Args = {temp(11)};
    Caller.CallInfos.push_back(Call);

    MedFunc Callee;
    Callee.Entry = Call.TargetAddr;
    Callee.CC = CallingConv::Win64;
    Callee.Params = {rcx(0)};
    MedBlock Body;
    Body.Id = 0;
    Body.StartAddr = Callee.Entry;
    Body.Ops = {op(NdOp::COPY, rcx(0), {rcx(0)}),
                op(NdOp::LOAD, Word, {rcx(0)}),
                op(NdOp::RETURN, none(), {Word})};
    if (Case == 1 || Case == 2) {
      Body.Ops.insert(
          Body.Ops.begin() + 1,
          op(NdOp::INT_SUB, temp(30), {rcx(0), MedVar::makeConst(4, 8)}));
      Body.Ops[2].Inputs[0] = temp(30);
      if (Case == 2)
        Body.Ops[2].Output.Size = 8;
    } else if (Case == 3) {
      Body.Ops.back().Inputs[0] = rcx(0);
    } else if (Case == 4) {
      Body.Ops.insert(
          Body.Ops.begin() + 1,
          op(NdOp::STORE, none(), {MedVar::makeConst(0x3000, 8), rcx(0)}));
    } else if (Case == 5) {
      auto Narrow = temp(30);
      Narrow.Size = 4;
      Body.Ops.insert(
          Body.Ops.begin() + 1,
          op(NdOp::SUBBYTES, Narrow, {rcx(0), MedVar::makeConst(0, 4)}));
      Body.Ops.insert(
          Body.Ops.begin() + 2,
          op(NdOp::STORE, none(), {MedVar::makeConst(0x3000, 8), Narrow}));
    } else if (Case == 6) {
      Body.ExceptionalSuccs.emplace_back();
    } else if (Case == 7) {
      Callee.SkippedSSA = true;
    } else if (Case == 8) {
      Caller.CallInfos[0].IsIndirect = true;
    } else if (Case == 10) {
      Body.Ops[1].Inputs[0] = reg(8, x86reg::RDX, 0);
    } else if (Case == 9) {
      Body.Ops.insert(Body.Ops.begin() + 1,
                      op(NdOp::INDIR_CALL, temp(30), {rcx(0)}));
    }
    if (Case == 13) {
      Body.Ops[1].Output = reg(9, x86reg::RAX, 0);
      Body.Ops[1].Output.Size = 4;
      Body.Ops.back().Inputs[0] = Body.Ops[1].Output;
    }
    if (Case == 14 || Case == 15) {
      // A narrow non-argument carrier can still disclose pointer bits.
      // AH is not the low slice of its family; a scalar XMM lane is not a GPR.
      auto Input = reg(40, Case == 14 ? x86reg::RAX + 1 : x86reg::XMM0, 0);
      Input.Size = Case == 14 ? 1 : 4;
      Body.Ops.insert(
          Body.Ops.begin() + 1,
          op(NdOp::STORE, none(), {MedVar::makeConst(0x3000, 8), Input}));
    }
    if (Case == 16)
      Callee.EntryKind = StackEntryKind::ProcessEntry;
    if (Case == 17)
      Body.Ops.back().Inputs[0] = sp(0);
    if (Case == 18)
      Body.Ops.insert(
          Body.Ops.begin() + 1,
          op(NdOp::STORE, none(), {MedVar::makeConst(0x3000, 8), sp(0)}));
    if (Case == 19) {
      Body.Ops.insert(
          Body.Ops.begin() + 1,
          op(NdOp::INT_ADD, temp(30), {sp(0), MedVar::makeConst(0x28, 8)}));
      Body.Ops[2].Inputs[0] = temp(30);
    }
    if (Case == 20 || Case == 21) {
      Caller.CallInfos[0].IsIndirect = true;
      Caller.CallInfos[0].Args.clear();
      auto &Invoke = Caller.Blocks[0].Ops[6];
      Invoke.Opcode = NdOp::INDIR_CALL;
      Invoke.CalleeRegisterArgs = 0;
      Invoke.NumInputs = 1;
      if (Case == 21) {
        Invoke.Output = none();
        Caller.Blocks[0].Ops.back().Inputs[0] = MedVar::makeConst(37, 4);
      }
    }
    if (Case == 11)
      Caller.Blocks[0].Ops[2].Inputs[1] = MedVar::makeConst(0x18, 8);
    if (Case == 12)
      Callee.SourceParametersBound = true;
    Callee.Blocks.push_back(std::move(Body));
    const std::map<va_t, const MedFunc *> Callees{{Callee.Entry, &Callee}};
    markUnobservedBorrowedFrameStores(Caller, Arch::X64, BinaryFormat::COFF,
                                      Callees);
    EXPECT_EQ(Caller.Blocks[0].Ops[4].PrologueSave, Case == 0 || Case == 13);
    EXPECT_FALSE(Caller.Blocks[0].Ops[5].PrologueSave);
    EXPECT_EQ(Caller.Blocks[0].Ops[6].Opcode,
              Case >= 20 ? NdOp::INDIR_CALL : NdOp::CALL);
  }
}

} // namespace
