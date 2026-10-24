//===- MedPrologueSavesTests.cpp - Callee-save prologue storage ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/lift/X86/X86Regs.h"

#include <initializer_list>
#include <limits>
#include <utility>

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

} // namespace
