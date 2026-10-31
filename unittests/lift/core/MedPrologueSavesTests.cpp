//===- MedPrologueSavesTests.cpp - Callee-save prologue storage ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Common.h"
#include "neverd/ir/Limits.h"
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

TEST(SourceStackSpills, OnlyCompleteImageReadsCanCrossACompleteOverwrite) {
  for (unsigned Mode = 0; Mode < 11; ++Mode) {
    SCOPED_TRACE(Mode);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Format = BinaryFormat::COFF;
    Image.Bits = Bitness::Bits64;
    Segment Storage;
    Storage.VA = 0x2000;
    Storage.Size = Storage.FileSz = 8;
    Storage.Data.resize(8);
    Storage.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Segments.push_back(std::move(Storage));
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.CC = CallingConv::Win64;
    Func.FrameSize = 40;
    const auto Home = temp(10);
    auto ImageAddress = MedVar::makeConst(
        Mode == 1 ? 0x2001 : 0x2000, 8,
        Mode == 2   ? ConstantAddressProvenance::Unknown
        : Mode == 3 ? ConstantAddressProvenance::AddressFragment
        : Mode == 4 ? ConstantAddressProvenance::CodeAddress
                    : ConstantAddressProvenance::Address);
    if (Mode == 5)
      ImageAddress = rcx(0);
    if (Mode == 10)
      ImageAddress.AddressOwnerVA = 0x3000;
    auto Read = op(NdOp::LOAD, temp(11), {ImageAddress});
    if (Mode == 6)
      Read.MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Mode == 7)
      Read.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    if (Mode == 8)
      Read = op(NdOp::STORE, none(), {ImageAddress, Home});
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Func.Entry;
    Block.Ops = {
        op(NdOp::COPY, sp(0), {sp(0)}), op(NdOp::COPY, rcx(0), {rcx(0)}),
        op(NdOp::INT_ADD, Home, {sp(0), MedVar::makeConst(8, 8)}),
        op(NdOp::STORE, none(), {Home, rcx(0)}), Read,
        op(NdOp::STORE, none(),
           {Home, MedVar::makeConst(19, Mode == 9 ? 4 : 8)}),
        // Once all original bytes are overwritten, a later opaque borrow
        // may inspect them without making the discarded incoming value live.
        op(NdOp::CALL, temp(12), {MedVar::makeConst(0x3000, 8), Home}),
        op(NdOp::RETURN, none(), {temp(11)})};
    Func.Blocks.push_back(std::move(Block));
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF,
                            StackEntryKind::Call, &Image);
    EXPECT_EQ(Func.Blocks.front().Ops[3].PrologueSave, Mode == 0);
    EXPECT_EQ(Func.Blocks.front().Ops[4].Opcode, Read.Opcode);
    EXPECT_EQ(Func.Blocks.front().Ops[4].MemoryOrdering, Read.MemoryOrdering);
    EXPECT_EQ(Func.Blocks.front().Ops.size(), 8U);
  }
}

TEST(SourceStackSpills, DiscardedSpillInputsStillNeedImplicitCallProofs) {
  for (unsigned Mode = 0; Mode < 13; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.CC = CallingConv::Win64;
    Func.FrameSize = 40;
    Func.Params = {rcx(0)};
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Func.Entry;
    const auto Home = temp(10);
    Block.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
                 op(NdOp::COPY, rcx(0), {rcx(0)}),
                 op(NdOp::INT_ADD, Home, {sp(0), MedVar::makeConst(8, 8)}),
                 op(NdOp::STORE, none(), {Home, rcx(0)}),
                 op(NdOp::STORE, none(), {Home, MedVar::makeConst(0, 8)})};
    if (Mode == 3)
      Block.Ops.push_back(op(NdOp::COPY, reg(4, x86reg::RDX, 1), {rcx(0)}));
    if (Mode == 7)
      Block.Ops.push_back(op(NdOp::COPY, temp(11), {rcx(0)}));
    if (Mode != 2 && Mode < 8) {
      auto Overwrite = rcx(1);
      Overwrite.Size = Mode == 1 ? 1 : 8;
      Block.Ops.push_back(
          op(NdOp::COPY, Overwrite,
             {Mode == 7 ? temp(11) : MedVar::makeConst(42, Overwrite.Size)}));
    }
    auto Call = op(NdOp::CALL, temp(12), {MedVar::makeConst(0x3000, 8)});
    Call.Addr = 0x1010;
    Call.OriginSeq = 7;
    Call.CallSiteId = 1;
    if (Mode >= 8) {
      auto &Proof = Func.CallUnreadRegisters[1];
      Proof.Instruction = Call.Addr;
      Proof.OriginSeq = Call.OriginSeq;
      Proof.Target = Mode == 9 ? 0x4000 : 0x3000;
      Proof.Families = GPRFamilyMask{1} << (x86reg::RCX / 8);
    }
    if (Mode == 4) {
      MedCallInfo Info;
      Info.BlockId = 0;
      Info.OpIdx = Block.Ops.size();
      Info.TargetAddr = 0x3000;
      Info.Args = {rcx(0)};
      Func.CallInfos.push_back(Info);
    }
    if (Mode == 5)
      Block.ExceptionalSuccs.resize(1);
    if (Mode >= 10) {
      Func.ExceptionMetadata.emplace();
      Func.ExceptionMetadata->ParseStatus =
          Mode == 12 ? ExceptionParseStatus::Partial
                     : ExceptionParseStatus::Complete;
      Func.ExceptionMetadata->Personality = Mode == 11
                                                ? ExceptionPersonality::Unknown
                                                : ExceptionPersonality::None;
    }
    Block.Ops.push_back(Call);
    Block.Ops.push_back(op(NdOp::RETURN, none(),
                           {Mode == 6 ? rcx(0) : MedVar::makeConst(0, 8)}));
    Func.Blocks.push_back(std::move(Block));
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
    EXPECT_EQ(unobservedSourceSpillParameters(Func),
              Mode == 0 || Mode == 8 || Mode == 10 ? std::set<unsigned>{0}
                                                   : std::set<unsigned>{});
  }
}

TEST(SourceStackSpills, RegisterSuffixesNeedCurrentSourceCalleeProofs) {
  for (unsigned Mode = 0; Mode < 13; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.CC = CallingConv::Win64;
    Func.Params = {rcx(0)};
    MedVar Low = reg(4, x86reg::RCX, 0);
    Low.Size = 1;
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Func.Entry;
    Block.Ops = {op(NdOp::COPY, rcx(0), {rcx(0)}), op(NdOp::COPY, Low, {Low})};
    auto Call = op(NdOp::CALL, temp(10),
                   {MedVar::makeConst(Mode == 2 ? 0x4000 : 0x3000, 8), rcx(0)});
    Call.Addr = 0x1010;
    Call.OriginSeq = 7;
    Call.CallSiteId = 1;
    MedCallInfo Info;
    Info.BlockId = 0;
    Info.OpIdx = Block.Ops.size();
    Info.TargetAddr = 0x3000;
    Info.Args = {rcx(0)};
    Info.IsIndirect = Mode == 3;
    if (Mode != 9)
      Func.CallInfos.push_back(Info);
    Block.Ops.push_back(Call);
    if (Mode == 5)
      Block.Ops.push_back(op(NdOp::COPY, reg(5, x86reg::RDX, 1), {rcx(0)}));
    if (Mode == 6) {
      auto High = temp(11);
      High.Size = 1;
      Block.Ops.push_back(
          op(NdOp::SUBBYTES, High, {rcx(0), MedVar::makeConst(1, 1)}));
      Block.Ops.push_back(op(NdOp::RETURN, none(), {High}));
    } else {
      Block.Ops.push_back(op(NdOp::RETURN, none(), {Mode == 4 ? rcx(0) : Low}));
    }
    if (Mode == 7)
      Block.ExceptionalSuccs.resize(1);
    if (Mode == 8)
      Func.SourceParametersBound = true;
    if (Mode == 12)
      Func.ModuleAnalysisRoots = {Func.Entry, 0x1010};
    Func.Blocks.push_back(std::move(Block));
    SourceCalleeParameterReads Callees;
    if (Mode != 1)
      Callees[0x3000][0] = Mode == 10 ? 4 : 0;
    size_t Budget = Mode == 11 ? 1 : 4096;
    const auto Reads = sourceRegisterParameterReads(Func, Callees, Budget);
    const SourceRegisterParameterReads Expected =
        Mode == 0 || Mode == 9 ? SourceRegisterParameterReads{{0, 1}}
        : Mode == 10           ? SourceRegisterParameterReads{{0, 4}}
                               : SourceRegisterParameterReads{};
    EXPECT_EQ(Reads, Expected);
  }
}

TEST(SourceStackSpills, GSInputDestructionKeepsExceptionalFrameObservers) {
  for (unsigned Case = 0; Case != 25; ++Case) {
    SCOPED_TRACE(Case);
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.CC = CallingConv::Win64;
    Func.Params = {rcx(0)};
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Func.Entry;
    auto Call =
        op(NdOp::CALL, temp(20), {MedVar::makeConst(0x3000, 8), rcx(0)});
    Call.Addr = 0x1004;
    Call.OriginSeq = 0;
    Block.Ops = {op(NdOp::COPY, rcx(0), {rcx(0)}), Call,
                 op(NdOp::RETURN, none(), {temp(20)})};
    Func.Blocks.push_back(std::move(Block));
    ExceptionFunction EH;
    EH.CodeRange = {Func.Entry, 0x1020};
    EH.Encoding = Case == 1 ? ExceptionEncoding::X64UnwindV2
                            : ExceptionEncoding::X64UnwindV1;
    EH.UnwindVersion = Case == 1 ? 2 : 1;
    EH.UnwindFlags = 3;
    EH.Personality = ExceptionPersonality::GSHandlerCheck;
    EH.PersonalityVA = 0x4000;
    EH.HandlerDataVA = 0x5000;
    EH.GSCookie.emplace();
    auto &Cookie = *EH.GSCookie;
    Cookie.ParseStatus = ExceptionParseStatus::Complete;
    Cookie.CookieOffset = Case == 2 ? 24 : (Case == 21 ? 32 : 40);
    Cookie.Payload = {uint8_t(Cookie.CookieOffset), 0, 0, 0};
    if (Case == 3)
      Cookie.HasAlignment = true;
    if (Case == 4)
      EH.FrameRegister = 5;
    if (Case == 5)
      Cookie.Payload.pop_back();
    if (Case == 6)
      Cookie.Payload[0] ^= 8;
    if (Case == 7)
      EH.ParseStatus = ExceptionParseStatus::Partial;
    if (Case == 8)
      Cookie.ParseStatus = ExceptionParseStatus::Malformed;
    if (Case == 9)
      EH.Personality = ExceptionPersonality::GSHandlerCheckSEH;
    if (Case == 14)
      Cookie.HasUnwindHandler = true;
    if (Case == 15) {
      EH.DecodeProvenance.emplace();
      EH.DecodeProvenance->Structural.ParseStatus =
          ExceptionParseStatus::Partial;
    }
    if (Case == 18)
      Func.EntryKind = StackEntryKind::ProcessEntry;
    if (Case == 22) {
      UnwindOperation Machine;
      Machine.Kind = UnwindOperationKind::PushMachineFrame;
      EH.UnwindOperations.push_back(Machine);
    }
    if (Case == 23)
      EH.GSCookie.reset();
    if (Case == 24) {
      UnwindOperation Allocation;
      Allocation.Kind = UnwindOperationKind::AllocateSmall;
      EH.UnwindOperations.assign(64, Allocation);
    }
    Func.ExceptionMetadata = std::move(EH);
    const auto Low = [](va_t PC, NdOp Code, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
      LowOp Op;
      Op.Addr = PC;
      Op.Opcode = Code;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      return Op;
    };
    LowBlock Original;
    Original.Id = 0;
    Original.StartAddr = Func.Entry;
    Original.EndAddr = 0x1010;
    const auto SP = NdVar::reg(x86reg::RSP, 8);
    const auto Input = NdVar::reg(x86reg::RCX, 8);
    Original.Ops = {
        Low(Func.Entry, NdOp::INT_SUB, SP, {SP, NdVar::cst(56, 8)}),
        Low(Case == 16 ? 0x2000 : Call.Addr, NdOp::CALL,
            NdVar::reg(x86reg::RAX, 8),
            {NdVar::cst(Case == 19 ? 0x3001 : 0x3000, 8)}),
        Low(0x1009, NdOp::INT_ADD, SP, {SP, NdVar::cst(56, 8)}),
        Low(0x100D, NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 8)})};
    std::set<const MedOp *> Overwritten;
    if (Case == 11 || Case == 12) {
      // Even a transient store removed from MedIR may be the cookie's value
      // during an exception. A source-dead store cannot authorize this input.
      Original.Ops.insert(Original.Ops.begin() + 1,
                          Low(0x1002, NdOp::STORE, {}, {SP, Input}));
      if (Case == 12) {
        auto Save = op(NdOp::STORE, none(), {sp(0), rcx(0)});
        Save.Addr = 0x1002;
        Save.OriginSeq = 0;
        Save.PrologueSave = true;
        auto &Ops = Func.Blocks.front().Ops;
        Ops.insert(Ops.begin() + 1, Save);
        Overwritten.insert(&Ops[1]);
      }
    }
    if (Case == 13)
      Original.Ops.insert(
          Original.Ops.begin() + 1,
          Low(0x1002, NdOp::COPY, NdVar::reg(x86reg::RDX, 8), {Input}));
    SourceCalleeParameterOverwrites Callees;
    if (Case != 10 && Case != 20)
      Callees[0x3000].insert(0);
    size_t Budget = Case == 17 ? 1 : (Case == 24 ? 32 : 4096);
    EXPECT_EQ(sourceRegisterInputOverwrites(Func, {Original}, {}, Callees,
                                            Overwritten, Budget),
              Case == 0 || Case == 1 || Case == 21 ? std::set<unsigned>{0}
                                                   : std::set<unsigned>{});
    // The ordinary MedIR spill proof alone never grants this GS fact.
    Budget = 4096;
    EXPECT_TRUE(sourceRegisterParameterReads(Func, {{0x3000, {{0, 0}}}}, Budget)
                    .empty());
  }
}

TEST(SourceStackSpills,
     InputDestructionNeedsOriginalEntryAndCompleteOverwrites) {
  for (unsigned Mode = 0; Mode < 29; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.CC = CallingConv::Win64;
    Func.FrameSize = 40;
    Func.Params = {rcx(0)};
    const auto Home = temp(10);
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Func.Entry;
    Block.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
                 op(NdOp::COPY, rcx(0), {rcx(0)}),
                 op(NdOp::INT_ADD, Home, {sp(0), MedVar::makeConst(8, 8)}),
                 op(NdOp::STORE, none(), {Home, rcx(0)}),
                 op(NdOp::STORE, none(),
                    {Home, MedVar::makeConst(0, Mode == 6 ? 4 : 8)}),
                 op(NdOp::COPY, rcx(1), {MedVar::makeConst(42, 8)}),
                 op(NdOp::RETURN, none(), {MedVar::makeConst(0, 8)})};
    Block.Ops[3].Addr = 0x1004;
    Block.Ops[3].OriginSeq = 0;
    if (Mode == 20)
      Block.Ops[4].Dead = true;
    if (Mode == 21)
      Block.Ops[3].Dead = true;
    if (Mode == 3 || Mode == 22 || Mode == 23) {
      auto Hidden =
          Mode == 23 ? op(NdOp::CALL, temp(11), {MedVar::makeConst(0x3000, 8)})
                     : op(NdOp::LOAD, temp(11), {Home});
      Hidden.Dead = Mode >= 22;
      Block.Ops.insert(Block.Ops.begin() + 4, Hidden);
    }
    Func.Blocks.push_back(std::move(Block));
    if (Mode == 8)
      Func.ModuleAnalysisRoots = {Func.Entry, 0x1004};
    std::set<const MedOp *> Overwritten;
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF,
                            StackEntryKind::Call, nullptr, &Overwritten);
    const auto RCX = NdVar::reg(x86reg::RCX, 8);
    const auto Address = NdVar::tmp(Mode == 28 ? 10 : TmpBase, 8);
    auto Low = [&](va_t PC, NdOp Code, NdVar Out,
                   std::initializer_list<NdVar> Inputs) {
      LowOp O;
      O.Addr = PC;
      O.Opcode = Code;
      O.Output = Out;
      for (const auto &V : Inputs)
        O.addInput(V);
      return O;
    };
    std::vector<LowOp> Original = {
        Low(Func.Entry, NdOp::INT_ADD, Address,
            {NdVar::reg(x86reg::RSP, 8), NdVar::cst(8, 8)}),
        Low(0x1004, NdOp::STORE, {}, {Address, RCX}),
        Low(0x1008, NdOp::STORE, {}, {Address, NdVar::cst(0, 8)}),
        Low(0x100C, NdOp::COPY,
            NdVar::reg(x86reg::RCX, Mode == 1    ? 1
                                    : Mode == 13 ? 4
                                                 : 8),
            {NdVar::cst(42, 8)})};
    SourceCalleeParameterOverwrites Callees;
    if (Mode == 2 || Mode == 12)
      Original.insert(
          Original.begin() + 2,
          Low(0x1006, NdOp::COPY,
              Mode == 2 ? NdVar::reg(x86reg::RDX, 8) : NdVar::tmp(11, 8),
              {RCX}));
    if (Mode == 4)
      Original[1].Seq = 1;
    if (Mode == 7)
      Original.front().Addr += 1;
    if (Mode == 5 || Mode == 11) {
      auto Call = op(NdOp::CALL, temp(12), {MedVar::makeConst(0x3000, 8)});
      Call.Addr = 0x100A;
      Call.OriginSeq = 0;
      Func.Blocks.front().Ops.insert(Func.Blocks.front().Ops.end() - 1, Call);
      Original.back() = Low(0x100A, NdOp::CALL, {},
                            {NdVar::cst(Mode == 5 ? 0x4000 : 0x3000, 8)});
      Callees[0x3000].insert(0);
      // Vector insertion changes operation addresses: obtain current receipts.
      markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF,
                              StackEntryKind::Call, nullptr, &Overwritten);
    }
    if (Mode == 10)
      Original.insert(Original.begin() + 2,
                      Low(0x1006, NdOp::BRANCH, {}, {NdVar::cst(0x2000, 8)}));
    if ((Mode >= 14 && Mode < 20) || Mode == 28) {
      auto Read = Low(0x1006, NdOp::LOAD, NdVar::tmp(12, 8), {Address});
      auto Mask = Low(0x1006, NdOp::INT_AND, NdVar::tmp(13, 8),
                      {Read.Output, NdVar::scalar(Mode == 16 ? 1 : 0, 8)});
      Mask.Seq = 1;
      auto Save = Low(0x1006, NdOp::STORE, {}, {Address, Mask.Output});
      Save.Seq = 2;
      if (Mode == 15)
        Mask =
            Low(0x1006, NdOp::COPY, NdVar::reg(x86reg::RDX, 8), {Read.Output});
      if (Mode == 17)
        Read.MemoryOrdering = NdMemoryOrdering::Acquire;
      if (Mode == 18) {
        Mask = Low(0x1006, NdOp::COPY, NdVar::tmp(13, 8), {Read.Output});
        Save = Low(0x1006, NdOp::INT_AND, NdVar::tmp(13, 8),
                   {Mask.Output, NdVar::scalar(0, 8)});
      }
      if (Mode == 19) {
        Mask =
            Low(0x1006, NdOp::COPY, NdVar::tmp(12, 1), {NdVar::scalar(0, 1)});
        Save =
            Low(0x1006, NdOp::COPY, NdVar::reg(x86reg::RDX, 8), {Read.Output});
      }
      Original.insert(Original.begin() + 2, {Read, Mask, Save});
    }
    if (Mode >= 24 && Mode < 28) {
      auto Read = Low(0x1006, NdOp::LOAD, NdVar::tmp(32, 8), {Address});
      auto Reset =
          Low(0x1006, NdOp::COPY, NdVar::tmp(34, 2), {NdVar::scalar(0, 2)});
      Reset.Seq = 1;
      // The middle bytes were reset; either side still carries the load.
      auto Observe = Low(0x1006, NdOp::COPY, NdVar::reg(x86reg::RDX, 1),
                         {NdVar::tmp(Mode == 24   ? 33
                                     : Mode == 25 ? 36
                                                  : 34,
                                     1)});
      Observe.Seq = 2;
      if (Mode == 27) {
        Reset.Output = NdVar::tmp(48, 2);
        Reset.Inputs[0] = NdVar::tmp(33, 2);
        Observe.Inputs[0] = NdVar::tmp(49, 1);
      }
      Original.insert(Original.begin() + 2, {Read, Reset, Observe});
    }
    size_t Budget = Mode == 9 ? 1 : 4096;
    LowBlock OriginalBlock;
    OriginalBlock.Id = 0;
    OriginalBlock.StartAddr = Func.Entry;
    OriginalBlock.Ops = std::move(Original);
    const auto Killed = sourceRegisterInputOverwrites(
        Func, {OriginalBlock}, {{0, 0}}, Callees, Overwritten, Budget);
    EXPECT_EQ(Killed, Mode == 0 || Mode == 11 || Mode == 13 || Mode == 14 ||
                              Mode == 18 || Mode == 26
                          ? std::set<unsigned>{0}
                          : std::set<unsigned>{});
  }
}

TEST(SourceStackSpills, InputDestructionCoversEveryOriginalBranch) {
  for (unsigned Mode = 0; Mode < 25; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.CC = CallingConv::Win64;
    Func.Params = {rcx(0)};
    MedBlock Current;
    Current.Id = 0;
    Current.StartAddr = Func.Entry;
    Current.Ops = {op(NdOp::COPY, rcx(0), {rcx(0)}),
                   op(NdOp::RETURN, none(), {MedVar::makeConst(0, 8)})};
    const auto Low = [](va_t PC, NdOp Code, NdVar Out,
                        std::initializer_list<NdVar> Inputs) {
      LowOp O;
      O.Addr = PC;
      O.Opcode = Code;
      O.Output = Out;
      for (const auto &V : Inputs)
        O.addInput(V);
      return O;
    };
    const auto RCX = NdVar::reg(x86reg::RCX, 8);
    const auto RAX = NdVar::reg(x86reg::RAX, 8);
    std::vector<LowBlock> Original(4);
    for (size_t I = 0; I < Original.size(); ++I) {
      Original[I].Id = I;
      Original[I].StartAddr = Func.Entry + I * 0x10;
      Original[I].EndAddr = Original[I].StartAddr + 8;
    }
    Original[0].EndAddr = Original[1].StartAddr;
    Original[0].Succs = {1, 2};
    Original[0].Ops = {Low(0x1000, NdOp::COND_BR, {},
                           {NdVar::cst(0x1020, 8), NdVar::reg(x86reg::ZF, 1)})};
    for (size_t I : {1u, 2u}) {
      auto &Block = Original[I];
      Block.Preds = {0};
      Block.Succs = {3};
      Block.Ops = {
          Low(Block.StartAddr, NdOp::COPY,
              NdVar::reg(x86reg::RCX, I == 1 ? 8 : 4), {NdVar::cst(I, 8)}),
          Low(Block.StartAddr + 4, NdOp::BRANCH, {}, {NdVar::cst(0x1030, 8)})};
    }
    Original[3].Preds = {1, 2};
    Original[3].Ops = {Low(0x1030, NdOp::RETURN, {}, {RAX})};
    if (Mode == 1 || Mode == 7)
      Original[1].Ops.front() = Low(0x1010, NdOp::RETURN, {}, {RAX});
    if (Mode == 2)
      Original[1].Ops.front().Output.Size = 2;
    if (Mode == 3)
      Original[1].Ops.front() = Low(0x1010, NdOp::COPY, RAX, {RCX});
    if (Mode == 4) {
      Original[1].Ops = {
          Low(0x1010, NdOp::BRANCH, {}, {NdVar::cst(0x1010, 8)})};
      Original[1].Succs = {1};
      Original[1].Preds.push_back(1);
    }
    if (Mode == 5)
      Original[0].Ops.back().Inputs[0] = NdVar::cst(0x2000, 8);
    if (Mode == 6)
      Original[0].EndAddr = 0x1011;
    if (Mode == 7)
      Original[0].Ops.front().Opcode = NdOp::RETURN;
    if (Mode == 8)
      Original[1].Ops.front() = Low(0x1010, NdOp::LOAD, RAX, {RCX});
    if (Mode == 9)
      Original[0].Ops.front().Opcode = NdOp::INDIR_BR;
    if (Mode == 11)
      Original[1].ExceptionalSuccs.resize(1);
    if (Mode == 12)
      Original[0].Preds = {1};
    if (Mode == 13) {
      // Control flow after destruction cannot revive an earlier input that
      // left no copy in a register or live storage.
      Original[1].Ops.back().Inputs[0] = NdVar::cst(0x1010, 8);
      Original[1].Succs = {1};
      Original[1].Preds.push_back(1);
    }
    if (Mode == 14) {
      Original[1].Ops.front().Output = RAX;
      Original[2].Ops.front().Output = RAX;
      Original[3].Ops.insert(Original[3].Ops.begin(),
                             Low(0x1030, NdOp::COPY, RCX, {NdVar::cst(0, 8)}));
    }
    if (Mode == 15) {
      Original[0].Ops.front() =
          Low(0x1000, NdOp::COPY, RAX, {NdVar::cst(0, 8)});
      Original[0].Succs = {1};
    }
    if (Mode == 16)
      Original[0].Succs.push_back(3);
    SourceCalleeParameterOverwrites Callees;
    if ((Mode >= 17 && Mode <= 20) || Mode == 22) {
      const auto Call = Low(0x1010, NdOp::CALL, RAX, {NdVar::cst(0x3000, 8)});
      if (Mode == 19)
        Original[1].Ops.insert(Original[1].Ops.begin(), Call);
      else
        Original[1].Ops.front() = Call;
      auto Bound = op(NdOp::CALL, temp(10),
                      {MedVar::makeConst(Mode == 18 ? 0x4000 : 0x3000, 8)});
      Bound.Addr = 0x1010;
      Bound.OriginSeq = Mode == 22 ? 1 : 0;
      Bound.CallSiteId = 1;
      Current.Ops.insert(Current.Ops.begin() + 1, Bound);
      if (Mode == 19) {
        auto &Proof = Func.CallUnreadRegisters[1];
        Proof.Instruction = 0x1010;
        Proof.OriginSeq = 0;
        Proof.Target = 0x3000;
        Proof.Families = GPRFamilyMask{1} << (x86reg::RCX / 8);
      } else if (Mode != 20)
        Callees[0x3000].insert(0);
    }
    if (Mode == 21)
      Original[0].Ops.front().Inputs[1] = RCX;
    if (Mode == 23)
      Original[1].Ops.clear();
    if (Mode == 24)
      Original[2].StartAddr = Original[1].StartAddr;
    Func.Blocks.push_back(std::move(Current));
    size_t Budget = Mode == 10 ? 1 : 4096;
    const auto Killed = sourceRegisterInputOverwrites(Func, Original, {{0, 0}},
                                                      Callees, {}, Budget);
    EXPECT_EQ(Killed, Mode == 0 || Mode == 13 || Mode == 14 || Mode == 15 ||
                              Mode == 17 || Mode == 19
                          ? std::set<unsigned>{0}
                          : std::set<unsigned>{});
  }
}

TEST(SourceStackSpills, InputDestructionChecksPromotedStackReads) {
  for (unsigned Mode = 0; Mode < 13; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.CC = CallingConv::Win64;
    Func.Params.resize(5);
    Func.Params[0] = rcx(0);
    auto &Parameter = Func.Params[4];
    Parameter.Kind = MedVar::Param;
    Parameter.Id = 4;
    Parameter.Size = 8;
    Parameter.RegOff = kNoParamReg;
    MedBlock Current;
    Current.Id = 0;
    Current.StartAddr = Func.Entry;
    Current.Ops = {op(NdOp::COPY, rcx(0), {rcx(0)}),
                   op(NdOp::RETURN, none(), {MedVar::makeConst(0, 8)})};
    const auto Low = [](va_t PC, NdOp Code, NdVar Out,
                        std::initializer_list<NdVar> Inputs) {
      LowOp O;
      O.Addr = PC;
      O.Opcode = Code;
      O.Output = Out;
      for (const auto &V : Inputs)
        O.addInput(V);
      return O;
    };
    const auto SP = NdVar::reg(x86reg::RSP, 8);
    const auto FP = NdVar::reg(x86reg::RBP, 8);
    const auto RCX = NdVar::reg(x86reg::RCX, 8);
    const auto Address = NdVar::tmp(TmpBase, 8);
    const auto Value = NdVar::tmp(TmpBase + 8, 8);
    LowBlock Original;
    Original.Id = 0;
    Original.StartAddr = Func.Entry;
    Original.Ops = {
        Low(0x1000, NdOp::COPY, FP, {SP}),
        Low(0x1004, NdOp::INT_ADD, Address,
            {FP, NdVar::scalar(Mode == 1 ? 32 : 40, 8)}),
        Low(0x1004, NdOp::LOAD, Value, {Address}),
        Low(0x1004, NdOp::COPY, NdVar::reg(x86reg::RAX, 8), {Value}),
        Low(0x1008, NdOp::COPY, RCX, {NdVar::scalar(0, 8)}),
        Low(0x100C, NdOp::RETURN, {}, {})};
    Original.Ops[2].Seq = 1;
    Original.Ops[3].Seq = 2;
    if (Mode == 2)
      Original.Ops[0].Output.Size = 4;
    if (Mode == 3)
      Original.Ops[2].Output.Size = 16;
    if (Mode == 4)
      Parameter.Size = 4;
    if (Mode == 5)
      Parameter.Id = 5;
    if (Mode == 6) {
      Original.Ops.insert(
          Original.Ops.begin() + 1,
          Low(0x1002, NdOp::CALL, {}, {NdVar::scalar(0x2000, 8)}));
      auto Bound = op(NdOp::CALL, temp(10), {MedVar::makeConst(0x2000, 8)});
      Bound.Addr = 0x1002;
      Bound.OriginSeq = 0;
      Bound.CallSiteId = 1;
      Current.Ops.insert(Current.Ops.begin() + 1, Bound);
      auto &Proof = Func.CallUnreadRegisters[1];
      Proof.Instruction = Bound.Addr;
      Proof.OriginSeq = 0;
      Proof.Target = 0x2000;
      Proof.Families = GPRFamilyMask{1} << (x86reg::RCX / 8);
    }
    if (Mode == 7)
      Original.Ops[0].Inputs[0] = RCX;
    if (Mode == 9)
      Func.Params.resize(4);
    if (Mode == 10)
      Parameter.RegOff = x86reg::RAX;
    if (Mode == 11)
      Original.Ops[1].Opcode = NdOp::INT_OR;
    if (Mode == 12)
      Original.Ops.insert(Original.Ops.begin() + 3, Original.Ops[2]);
    Func.Blocks.push_back(std::move(Current));
    size_t Remaining = Mode == 8 ? 1 : 2048;
    const auto Killed = sourceRegisterInputOverwrites(
        Func, {Original}, {{0, 0}}, {}, {}, Remaining);
    EXPECT_EQ(Killed, Mode == 0 ? std::set<unsigned>{0} : std::set<unsigned>{});
  }
}

TEST(SourceStackSpills, StackParameterHolesKeepOriginalReadsAndEscapes) {
  for (unsigned Mode = 0; Mode < 29; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.CC = CallingConv::Win64;
    Func.Params.resize(7);
    for (unsigned P = 4; P < Func.Params.size(); ++P) {
      auto &Parameter = Func.Params[P];
      Parameter.Kind = MedVar::Param;
      Parameter.Id = P;
      Parameter.RegOff = kNoParamReg;
      Parameter.Size = 8;
      Parameter.TheArch = Arch::X64;
    }
    MedBlock Block;
    Block.Id = 0;
    Block.StartAddr = Func.Entry;
    auto Call = op(NdOp::CALL, temp(10), {MedVar::makeConst(0x3000, 8)});
    Call.Addr = 0x100C;
    Call.OriginSeq = 0;
    Call.CalleeRegisterArgs = Call.CalleeVectorArgs = Call.CalleeStackArgs = 0;
    Block.Ops = {Call, op(NdOp::RETURN, none(), {temp(10)})};
    Func.Blocks.push_back(std::move(Block));
    const auto SP = NdVar::reg(x86reg::RSP, 8);
    const auto RAX = NdVar::reg(x86reg::RAX, 8);
    const auto Address = NdVar::tmp(0x10000, 8);
    const auto Low = [](va_t PC, NdOp Opcode, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
      LowOp Result;
      Result.Addr = PC;
      Result.Opcode = Opcode;
      Result.Output = Output;
      for (const auto &V : Inputs)
        Result.addInput(V);
      return Result;
    };
    std::vector<LowOp> Original = {
        Low(0x1000, NdOp::INT_SUB, SP, {SP, NdVar::cst(40, 8)}),
        Low(0x1004, NdOp::INT_ADD, Address,
            {SP, NdVar::cst(Mode == 1   ? 80
                            : Mode == 2 ? 79
                                        : 88,
                            8)}),
        Low(0x1004, NdOp::LOAD, NdVar::reg(x86reg::RAX, Mode == 2 ? 2 : 8),
            {Address}),
        Low(0x1008, NdOp::INT_ADD, Address, {SP, NdVar::cst(96, 8)}),
        Low(0x1008, NdOp::LOAD, RAX, {Address}),
        Low(0x100C, NdOp::CALL, RAX, {NdVar::cst(0x3000, 8)}),
        Low(0x1010, NdOp::INT_ADD, SP, {SP, NdVar::cst(40, 8)}),
        Low(0x1014, NdOp::RETURN, {}, {RAX})};
    std::set<va_t> Declared{0x3000};
    if (Mode == 3)
      Func.Blocks[0].Ops[0].CalleeStackArgs = 15;
    if (Mode == 4)
      Declared.clear();
    if (Mode == 5)
      Func.Blocks[0].Ops[0].Inputs[0].ConstVal += 8;
    if (Mode == 6)
      Func.Blocks[0].Ops[0].OriginSeq = 1;
    if (Mode == 7)
      Original[1] = Low(0x1004, NdOp::COPY, NdVar::reg(x86reg::RCX, 8), {SP});
    if (Mode == 8)
      Original[2] = Low(0x1004, NdOp::STORE, {}, {Address, SP});
    if (Mode == 9)
      Original[1].Opcode = NdOp::INT_AND;
    if (Mode == 10)
      Original.insert(Original.begin() + 2,
                      Low(0x1004, NdOp::COPY, NdVar::tmp(Address.Offset, 1),
                          {NdVar::scalar(0, 1)}));
    if (Mode == 12) {
      Func.ExceptionMetadata.emplace();
      Func.ExceptionMetadata->Personality = ExceptionPersonality::Unknown;
    }
    if (Mode == 13)
      Func.Blocks.push_back(MedBlock{});
    if (Mode == 14)
      Original[2].MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Mode == 15)
      Original[2].Opcode = NdOp::INTRINSIC;
    if (Mode == 16)
      Original[0].Addr += 1;
    if (Mode == 17)
      Original[6].Inputs[1].Offset += 8;
    if (Mode == 18)
      Func.Blocks[0].Ops[1].Inputs[0] = Func.Params[4];
    if (Mode == 19)
      Func.Blocks[0].Ops[0].IsTailCall = true;
    if (Mode == 20) {
      const auto Flag = NdVar::reg(x86reg::CF, 1);
      Original.insert(
          Original.begin() + 1,
          {Low(0x1002, NdOp::INT_EQUAL, Flag, {SP, NdVar::cst(0, 8)}),
           Low(0x1003, NdOp::COPY, NdVar::reg(x86reg::RCX, 1), {Flag})});
    }
    if (Mode == 21)
      Func.CC = CallingConv::SysV_AMD64;
    if (Mode == 22)
      Original[0].Inputs[0].Size = 4;
    if (Mode == 23) {
      Call.Addr += 4;
      Func.Blocks[0].Ops.insert(Func.Blocks[0].Ops.begin(), Call);
    }
    if (Mode == 24)
      Original[2].Inputs[0] = NdVar::reg(x86reg::RCX, 8);
    if (Mode == 25)
      Original[2].Addr += 1;
    if (Mode == 26 || Mode == 28) {
      const auto Flag = NdVar::reg(x86reg::CF, 1);
      Original.insert(
          Original.begin() + 6,
          Low(0x100D, NdOp::COPY, NdVar::reg(x86reg::RCX, 1), {Flag}));
      if (Mode == 28)
        Original.insert(Original.begin() + 6,
                        Low(0x100D, NdOp::COPY, Flag, {NdVar::scalar(0, 1)}));
      Original.insert(Original.begin() + 1, Low(0x1002, NdOp::INT_EQUAL, Flag,
                                                {SP, NdVar::cst(0, 8)}));
    }
    if (Mode == 27)
      Func.Blocks[0].Ops[0].CalleeStackArgs = 7;
    size_t Budget = Mode == 11 ? 1 : 4096;
    LowBlock OriginalBlock;
    OriginalBlock.Id = 0;
    OriginalBlock.StartAddr = Func.Entry;
    OriginalBlock.EndAddr = 0x1015;
    OriginalBlock.Ops = std::move(Original);
    EXPECT_EQ(unobservedSourceStackParameters(Func, {OriginalBlock}, Declared,
                                              Budget),
              Mode == 0 || Mode == 27 || Mode == 28 ? std::set<unsigned>{4}
              : Mode == 1 || Mode == 2              ? std::set<unsigned>{5}
                                                    : std::set<unsigned>{});
  }
}

TEST(SourceStackSpills, StackParametersTrackEntryBytesAcrossOverwrites) {
  enum Case {
    FullThenRead,
    PartialThenFullRead,
    PartialThenPartialRead,
    ReadThenWrite,
    UnknownReadBefore,
    UnknownReadAfter,
    CallBefore,
    CallAfter,
    SplitWrites,
    IncompleteSplit,
    AliasedWrite,
    OriginalParameterUse,
    ExhaustedWork,
    ExceptionalObserver,
    OrderedWrite,
    ForeignAddressSpace,
    TruncatedAddress,
    MissingEdge,
    DisconnectedCycle
  };
  for (unsigned Mode = FullThenRead; Mode <= DisconnectedCycle; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.CC = CallingConv::Win64;
    Func.Params.resize(5);
    auto &Parameter = Func.Params[4];
    Parameter.Kind = MedVar::Param;
    Parameter.Id = 4;
    Parameter.RegOff = kNoParamReg;
    Parameter.Size = 8;
    Parameter.TheArch = Arch::X64;
    MedBlock Source;
    Source.Id = 0;
    Source.StartAddr = Func.Entry;
    Source.Ops = {op(NdOp::RETURN, none(), {MedVar::makeConst(7, 8)})};
    if (Mode == OriginalParameterUse)
      Source.Ops[0].Inputs[0] = Parameter;
    Func.Blocks.push_back(std::move(Source));
    if (Mode == ExceptionalObserver) {
      Func.ExceptionMetadata.emplace();
      Func.ExceptionMetadata->Personality = ExceptionPersonality::Unknown;
    }
    const auto SP = NdVar::reg(x86reg::RSP, 8);
    const auto AX = NdVar::reg(x86reg::RAX, 8);
    const auto CX = NdVar::reg(x86reg::RCX, 8);
    const auto Address = NdVar::tmp(0x10000, 8);
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Func.Entry;
    va_t PC = Func.Entry;
    const auto Add = [&](NdOp Opcode, NdVar Output,
                         std::initializer_list<NdVar> Inputs) {
      LowOp Op;
      Op.Addr = PC;
      Op.Seq = Block.Ops.empty() || Block.Ops.back().Addr != PC
                   ? 0
                   : Block.Ops.back().Seq + 1;
      Op.Opcode = Opcode;
      Op.Output = Output;
      for (const auto &Input : Inputs)
        Op.addInput(Input);
      Block.Ops.push_back(Op);
    };
    const auto Slot = [&](int64_t Offset = 40) {
      Add(NdOp::INT_ADD, Address, {SP, NdVar::cst(Offset, 8)});
    };
    const auto UnknownRead = [&] {
      Add(NdOp::LOAD, AX, {CX});
      ++PC;
    };
    const auto Call = [&] {
      Add(NdOp::CALL, AX, {NdVar::cst(0x3000, 8)});
      ++PC;
    };
    Slot();
    if (Mode == ReadThenWrite) {
      Add(NdOp::LOAD, AX, {Address});
      ++PC;
      Slot();
    }
    if (Mode == UnknownReadBefore) {
      UnknownRead();
      Slot();
    }
    if (Mode == CallBefore) {
      Call();
      Slot();
    }
    const bool Partial = Mode == PartialThenFullRead ||
                         Mode == PartialThenPartialRead ||
                         Mode == SplitWrites || Mode == IncompleteSplit;
    if (Mode == AliasedWrite)
      Add(NdOp::COPY, CX, {Address});
    if (Mode == TruncatedAddress)
      Add(NdOp::COPY, NdVar::tmp(Address.Offset, 4), {NdVar::scalar(0, 4)});
    Add(NdOp::STORE, {},
        {Mode == AliasedWrite ? CX : Address,
         NdVar::scalar(7, Partial ? 4 : 8)});
    if (Mode == OrderedWrite)
      Block.Ops.back().MemoryOrdering = NdMemoryOrdering::Release;
    if (Mode == ForeignAddressSpace)
      Block.Ops.back().MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    ++PC;
    if (Mode == SplitWrites || Mode == IncompleteSplit) {
      Slot(44);
      Add(NdOp::STORE, {},
          {Address, NdVar::scalar(9, Mode == SplitWrites ? 4 : 2)});
      ++PC;
    }
    if (Mode == UnknownReadAfter || Mode == DisconnectedCycle)
      UnknownRead();
    if (Mode == CallAfter)
      Call();
    Slot();
    Add(NdOp::LOAD,
        NdVar::reg(x86reg::RAX, Mode == PartialThenPartialRead ? 4 : 8),
        {Address});
    ++PC;
    if (Mode == AliasedWrite) {
      Add(NdOp::COPY, CX, {NdVar::scalar(0, 8)});
      ++PC;
    }
    Add(NdOp::RETURN, {}, {AX});
    Block.EndAddr = PC + 1;
    if (Mode == MissingEdge)
      Block.Succs = {1};
    std::vector<LowBlock> Original{Block};
    if (Mode == DisconnectedCycle) {
      LowBlock Other;
      Other.Id = 1;
      Other.StartAddr = 0x1100;
      Other.EndAddr = 0x1101;
      Other.Preds = Other.Succs = {1};
      LowOp Branch;
      Branch.Addr = Other.StartAddr;
      Branch.Opcode = NdOp::BRANCH;
      Branch.addInput(NdVar::cst(Other.StartAddr, 8));
      Other.Ops.push_back(Branch);
      Original.push_back(std::move(Other));
    }
    size_t Budget = Mode == ExhaustedWork ? 1 : 4096;
    const bool Unread = Mode == FullThenRead ||
                        Mode == PartialThenPartialRead ||
                        Mode == UnknownReadAfter || Mode == CallAfter ||
                        Mode == SplitWrites || Mode == AliasedWrite;
    EXPECT_EQ(unobservedSourceStackParameters(Func, Original, {}, Budget),
              Unread ? std::set<unsigned>{4} : std::set<unsigned>{});
  }
}

TEST(SourceStackSpills, StackParameterOverwritesMeetAcrossEveryPath) {
  for (unsigned Mode = 0; Mode < 9; ++Mode)
    for (bool Reversed : {false, true}) {
      SCOPED_TRACE(Mode);
      SCOPED_TRACE(Reversed);
      MedFunc Func;
      Func.Entry = 0x1000;
      Func.CC = CallingConv::Win64;
      Func.Params.resize(5);
      auto &Parameter = Func.Params[4];
      Parameter.Kind = MedVar::Param;
      Parameter.Id = 4;
      Parameter.RegOff = kNoParamReg;
      Parameter.Size = 8;
      Parameter.TheArch = Arch::X64;
      MedBlock Source;
      Source.Id = 0;
      Source.StartAddr = Func.Entry;
      Source.Ops = {op(NdOp::RETURN, none(), {MedVar::makeConst(7, 8)})};
      Func.Blocks.push_back(std::move(Source));
      const auto SP = NdVar::reg(x86reg::RSP, 8);
      const auto AX = NdVar::reg(x86reg::RAX, 8);
      const auto Address = NdVar::tmp(0x10000, 8);
      const auto Low = [](va_t PC, NdOp Opcode, NdVar Output,
                          std::initializer_list<NdVar> Inputs) {
        LowOp Op;
        Op.Addr = PC;
        Op.Opcode = Opcode;
        Op.Output = Output;
        for (const auto &Input : Inputs)
          Op.addInput(Input);
        return Op;
      };
      std::vector<LowBlock> Blocks(4);
      for (unsigned I = 0; I < Blocks.size(); ++I) {
        Blocks[I].Id = I;
        Blocks[I].StartAddr = 0x1000 + I * 16;
        Blocks[I].EndAddr = Blocks[I].StartAddr + 16;
      }
      Blocks[0].Succs = {1, 2};
      Blocks[0].Ops = {Low(0x1000, NdOp::COND_BR, {},
                           {NdVar::cst(0x1020, 8), NdVar::reg(x86reg::ZF, 1)})};
      for (unsigned I : {1u, 2u}) {
        auto &B = Blocks[I];
        B.Preds = {0};
        B.Succs = {3};
        const bool Half = Mode == 1 ? I == 2 : Mode == 2 || Mode == 3;
        const unsigned Offset = Mode == 3 && I == 2 ? 44 : 40;
        B.Ops = {Low(B.StartAddr, NdOp::INT_ADD, Address,
                     {SP, NdVar::cst(Offset, 8)}),
                 Low(B.StartAddr, NdOp::STORE, {},
                     {Address, NdVar::scalar(7, Half ? 4 : 8)})};
        if (Mode == 4 && I == 2)
          B.Ops.insert(B.Ops.begin() + 1,
                       Low(B.StartAddr, NdOp::LOAD, AX, {Address}));
        if (Mode == 5 || Mode == 6) {
          if (I == 1)
            B.Ops.push_back(Low(B.StartAddr + 4, NdOp::LOAD, AX,
                                {NdVar::reg(x86reg::RCX, 8)}));
          else if (Mode == 6)
            B.Ops.erase(B.Ops.begin() + 1);
        }
        B.Ops.push_back(
            Low(B.StartAddr + 8, NdOp::BRANCH, {}, {NdVar::cst(0x1030, 8)}));
        if (Mode == 7)
          B.Ops.erase(B.Ops.begin() + 1);
      }
      Blocks[3].Preds = {1, 2};
      Blocks[3].Ops = {
          Low(0x1030, NdOp::INT_ADD, Address, {SP, NdVar::cst(40, 8)}),
          Low(0x1030, NdOp::LOAD,
              NdVar::reg(x86reg::RAX, Mode == 2 || Mode == 3 ? 4 : 8),
              {Address}),
          Low(0x1038, NdOp::RETURN, {}, {AX})};
      if (Mode >= 7) {
        // A backedge overwrite cannot erase the first iteration's read.
        Blocks[3].Preds.push_back(3);
        Blocks[3].Succs = {3, 4};
        Blocks[3].Ops.pop_back();
        Blocks[3].Ops.push_back(
            Low(0x1034, NdOp::INT_ADD, Address, {SP, NdVar::cst(40, 8)}));
        Blocks[3].Ops.push_back(
            Low(0x1034, NdOp::STORE, {}, {Address, NdVar::scalar(7, 8)}));
        Blocks[3].Ops.push_back(
            Low(0x1038, NdOp::COND_BR, {},
                {NdVar::cst(0x1030, 8), NdVar::reg(x86reg::ZF, 1)}));
        auto &Exit = Blocks.emplace_back();
        Exit.Id = 4;
        Exit.StartAddr = 0x1040;
        Exit.EndAddr = 0x1041;
        Exit.Preds = {3};
        Exit.Ops = {Low(0x1040, NdOp::RETURN, {}, {AX})};
      }
      if (Reversed)
        std::swap(Blocks[1], Blocks[2]);
      size_t Budget = 4096;
      EXPECT_EQ(unobservedSourceStackParameters(Func, Blocks, {}, Budget),
                Mode == 0 || Mode == 2 || Mode == 5 || Mode == 8
                    ? std::set<unsigned>{4}
                    : std::set<unsigned>{});
    }
}

TEST(SourceStackSpills, StackParameterHolesRequireEveryOriginalPath) {
  for (unsigned Mode = 0; Mode < 49; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.CC = CallingConv::Win64;
    Func.Params.resize(7);
    for (unsigned P = 4; P < Func.Params.size(); ++P) {
      auto &Parameter = Func.Params[P];
      Parameter.Kind = MedVar::Param;
      Parameter.Id = P;
      Parameter.RegOff = kNoParamReg;
      Parameter.Size = 8;
      Parameter.TheArch = Arch::X64;
    }
    Func.Blocks.resize(4);
    std::vector<LowBlock> Original(4);
    for (unsigned I = 0; I < 4; ++I) {
      Func.Blocks[I].Id = Original[I].Id = I;
      Func.Blocks[I].StartAddr = Original[I].StartAddr = 0x1000 + 16 * I;
      Original[I].EndAddr = 0x1010 + 16 * I;
    }
    Original[0].Succs = {1, 2};
    Original[1].Preds = Original[2].Preds = {0};
    Original[1].Succs = Original[2].Succs = {3};
    Original[3].Preds = {1, 2};
    for (unsigned I = 0; I < 4; ++I) {
      Func.Blocks[I].Preds = Original[I].Preds;
      Func.Blocks[I].Succs = Original[I].Succs;
    }
    auto Call = op(NdOp::CALL, temp(10), {MedVar::makeConst(0x3000, 8)});
    Call.Addr = 0x1030;
    Call.OriginSeq = 0;
    Call.CalleeRegisterArgs = Call.CalleeVectorArgs = Call.CalleeStackArgs = 0;
    Func.Blocks[1].Ops = {op(NdOp::COPY, temp(11), {Func.Params[5]})};
    Func.Blocks[2].Ops = {op(NdOp::COPY, temp(12), {Func.Params[6]})};
    Func.Blocks[3].Ops = {Call, op(NdOp::RETURN, none(), {temp(10)})};
    const auto SP = NdVar::reg(x86reg::RSP, 8);
    const auto RAX = NdVar::reg(x86reg::RAX, 8);
    const auto RCX = NdVar::reg(x86reg::RCX, 8);
    const auto CF = NdVar::reg(x86reg::CF, 1);
    const auto Address = NdVar::tmp(0x10000, 8);
    const auto Low = [](va_t PC, NdOp Opcode, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
      LowOp Result;
      Result.Addr = PC;
      Result.Opcode = Opcode;
      Result.Output = Output;
      for (const auto &V : Inputs)
        Result.addInput(V);
      return Result;
    };
    Original[0].Ops = {
        Low(0x1000, NdOp::INT_SUB, SP, {SP, NdVar::cst(40, 8)}),
        Low(0x1004, NdOp::COND_BR, {}, {NdVar::cst(0x1020, 8), RCX})};
    for (unsigned I : {1u, 2u}) {
      const va_t PC = Original[I].StartAddr;
      Original[I].Ops = {
          Low(PC, NdOp::INT_ADD, Address, {SP, NdVar::cst(80 + I * 8, 8)}),
          Low(PC, NdOp::LOAD, RAX, {Address}),
          Low(PC + 4, NdOp::BRANCH, {}, {NdVar::cst(0x1030, 8)})};
    }
    Original[3].Ops = {Low(0x1030, NdOp::CALL, RAX, {NdVar::cst(0x3000, 8)}),
                       Low(0x1034, NdOp::INT_ADD, SP, {SP, NdVar::cst(40, 8)}),
                       Low(0x1038, NdOp::RETURN, {}, {RAX})};
    if (Mode == 1)
      Original[2].Ops[0].Inputs[1].Offset = 80;
    if (Mode == 2) {
      Original[2].Ops[0].Inputs[1].Offset = 79;
      Original[2].Ops[1].Output.Size = 2;
    }
    if (Mode == 3)
      std::swap(Original[1], Original[2]);
    if (Mode == 4)
      Original[0].Succs = {1};
    if (Mode == 5)
      Original[2].Preds.clear();
    if (Mode == 6)
      Original[3].Preds.push_back(1);
    if (Mode == 7)
      Original[2].Id = 1;
    if (Mode == 8)
      Original[2].StartAddr = 0x1010;
    if (Mode == 9)
      Original[0].Ops.back().Inputs[0] = RCX;
    if (Mode == 10)
      Original[0].Ops.back().NumInputs = 1;
    if (Mode == 11)
      Func.Blocks[3].Ops[0].Inputs[0].ConstVal += 8;
    if (Mode == 12)
      Func.Blocks[3].Ops[0].OriginSeq = 1;
    if (Mode == 13)
      Func.Blocks[3].Ops[0].CalleeStackArgs = 15;
    if (Mode == 14)
      Func.Blocks[3].Ops[0].IsTailCall = true;
    if (Mode == 15)
      Original[1].Ops.insert(Original[1].Ops.begin(),
                             Low(0x1010, NdOp::COPY, RCX, {SP}));
    if (Mode == 16 || Mode == 17 || Mode == 18) {
      // The untainted arm reaches the join first. The later predecessor must
      // revisit it; clearing the flag on only one arm is insufficient.
      Original[1].Ops.insert(
          Original[1].Ops.begin(),
          Low(0x1010, NdOp::INT_EQUAL, CF, {SP, NdVar::scalar(0, 8)}));
      Original[2].Ops.insert(
          Original[2].Ops.begin(),
          Low(0x1020, NdOp::COPY, CF, {NdVar::scalar(0, 1)}));
      if (Mode == 17)
        Original[1].Ops.insert(
            Original[1].Ops.begin() + 1,
            Low(0x1010, NdOp::COPY, CF, {NdVar::scalar(0, 1)}));
      const auto Read =
          Low(0x1030, NdOp::COPY, NdVar::reg(x86reg::RCX, 1), {CF});
      Original[3].Ops.insert(Original[3].Ops.begin() + (Mode == 18 ? 1 : 0),
                             Read);
    }
    if (Mode == 19)
      Original[1].Ops.insert(
          Original[1].Ops.begin(),
          Low(0x1010, NdOp::INT_SUB, SP, {SP, NdVar::scalar(8, 8)}));
    if (Mode == 20 || Mode == 21) {
      Original[2].Ops.back() =
          Low(0x1024, NdOp::COND_BR, {}, {NdVar::cst(0x1020, 8), RCX});
      Original[2].Succs = {2, 3};
      Original[2].Preds.push_back(2);
      if (Mode == 21)
        Original[2].Ops.insert(
            Original[2].Ops.begin(),
            Low(0x1020, NdOp::INT_SUB, SP, {SP, NdVar::scalar(8, 8)}));
    }
    if (Mode == 22)
      Func.Blocks[3].Ops[0].CalleeStackArgs = -1;
    if (Mode == 23)
      Original[3].Ops.pop_back();
    if (Mode == 24)
      Original[2].ExceptionalSuccs.resize(1);
    if (Mode == 25)
      Func.Blocks[2].ExceptionalPreds.resize(1);
    if (Mode == 26) {
      PhiNode Phi;
      Phi.Output = temp(20);
      Phi.Args = {{1, Func.Params[4]}, {2, Func.Params[4]}};
      Func.Blocks[3].Phis.push_back(Phi);
    }
    if (Mode == 27) {
      MedCallInfo Info;
      Info.BlockId = 3;
      Info.OpIdx = 0;
      Info.Args = {Func.Params[4]};
      Func.CallInfos.push_back(Info);
    }
    if (Mode == 28) {
      MedCallClobber Clobber;
      Clobber.PreservedInput = Func.Params[4];
      Func.CallClobbers.push_back(Clobber);
    }
    if (Mode == 30)
      Original[2].Ops[1].Inputs[0] = RCX;
    if (Mode == 31)
      Original[2].Ops[1] = Low(0x1020, NdOp::STORE, {}, {Address, SP});
    if (Mode == 32)
      Original[0].Ops[0].Output.Size = 4;
    if (Mode == 33)
      Func.Blocks[3].Ops.erase(Func.Blocks[3].Ops.begin());
    if (Mode == 34)
      Original[1].Ops.insert(Original[1].Ops.begin(),
                             Low(0x1010, NdOp::RETURN, {}, {RAX}));
    if (Mode == 35)
      Original[2].Ops[1].Addr = 0x1040;
    if (Mode == 36) {
      auto &Unreachable = Original.emplace_back();
      Unreachable.Id = 4;
      Unreachable.StartAddr = 0x1040;
      Unreachable.EndAddr = 0x1050;
      Unreachable.Preds = Unreachable.Succs = {4};
      Unreachable.Ops = {
          Low(0x1040, NdOp::BRANCH, {}, {NdVar::cst(0x1040, 8)})};
    }
    if (Mode == 37)
      Original[2].Ops.back().Opcode = NdOp::INDIR_BR;
    if (Mode == 38)
      Func.ModuleAnalysisRoots = {0x1000, 0x1020};
    if (Mode == 39)
      Original[2].Ops[1].MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Mode == 40)
      Original[3].Ops.insert(Original[3].Ops.begin(), Original[3].Ops.front());
    if (Mode == 41)
      Original[2].Ops.insert(Original[2].Ops.begin() + 1,
                             Low(0x1020, NdOp::COPY,
                                 NdVar::tmp(Address.Offset + 1, 1),
                                 {NdVar::scalar(0, 1)}));
    if (Mode == 42)
      Original[2].Ops[1].Addr += 1;
    if (Mode == 43)
      Original[2].Ops.insert(Original[2].Ops.begin() + 1,
                             Low(0x1020, NdOp::COPY, NdVar::reg(x86reg::RCX, 1),
                                 {NdVar::tmp(Address.Offset + 1, 1)}));
    if (Mode == 44)
      Original[2].Ops.insert(Original[2].Ops.begin(),
                             Low(0x1020, NdOp::COPY,
                                 NdVar::tmp(Address.Offset + 1, 1),
                                 {NdVar::scalar(0, 1)}));
    if (Mode == 45)
      Original[1].EndAddr = 0x1021;
    if (Mode == 46)
      Original[0].Ops.back().Inputs[0].Offset = 0x1030;
    if (Mode == 47)
      Original[0].EndAddr = 0x1011;
    if (Mode == 48)
      Func.Blocks[2].Ops[0].Inputs[0] = Func.Params[4];
    size_t Budget = Mode == 29 ? 1 : 4096;
    const auto Expected =
        Mode == 0 || Mode == 3 || Mode == 17 || Mode == 20 || Mode == 44
            ? std::set<unsigned>{4}
            : std::set<unsigned>{};
    EXPECT_EQ(unobservedSourceStackParameters(Func, Original, {0x3000}, Budget),
              Expected);
    if (Mode == 0) {
      const size_t Used = 4096 - Budget;
      Budget = Used;
      EXPECT_EQ(
          unobservedSourceStackParameters(Func, Original, {0x3000}, Budget),
          Expected);
      EXPECT_EQ(Budget, 0u);
      Budget = Used - 1;
      EXPECT_TRUE(
          unobservedSourceStackParameters(Func, Original, {0x3000}, Budget)
              .empty());
    }
  }
}

TEST(SourceStackSpills, StackParameterHolesKeepExactImageReadProvenance) {
  for (unsigned Mode = 0; Mode != 32; ++Mode) {
    SCOPED_TRACE(Mode);
    BinaryImage Image;
    Image.Arch = Arch::X64;
    Image.Format = BinaryFormat::COFF;
    Image.Bits = Bitness::Bits64;
    Segment Storage;
    Storage.VA = 0x2000;
    Storage.Size = Storage.FileSz = 16;
    Storage.Data.resize(16);
    Storage.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Segments.push_back(Storage);
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.CC = CallingConv::Win64;
    Func.Params.resize(6);
    for (unsigned P = 4; P != 6; ++P) {
      auto &Parameter = Func.Params[P];
      Parameter.Kind = MedVar::Param;
      Parameter.Id = P;
      Parameter.RegOff = kNoParamReg;
      Parameter.Size = 8;
      Parameter.TheArch = Arch::X64;
    }
    Func.Blocks.resize(1);
    Func.Blocks[0].StartAddr = Func.Entry;
    Func.Blocks[0].Ops = {op(NdOp::COPY, temp(10), {Func.Params[5]})};
    const auto SP = NdVar::reg(x86reg::RSP, 8);
    const auto RAX = NdVar::reg(x86reg::RAX, 8);
    const auto Address = NdVar::tmp(0x10000, 8);
    const auto Low = [](va_t PC, NdOp Opcode, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
      LowOp Result;
      Result.Addr = PC;
      Result.Opcode = Opcode;
      Result.Output = Output;
      for (const auto &V : Inputs)
        Result.addInput(V);
      return Result;
    };
    LowBlock Original;
    Original.StartAddr = Func.Entry;
    Original.EndAddr = 0x1010;
    Original.Ops = {
        Low(0x1000, NdOp::INT_ADD, Address, {SP, NdVar::scalar(48, 8)}),
        Low(0x1000, NdOp::LOAD, NdVar::reg(x86reg::R8, 8), {Address}),
        Low(0x1004, NdOp::COPY, Address, {NdVar::address(0x1FF8, 8)}),
        Low(0x1004, NdOp::INT_ADD, Address, {Address, NdVar::scalar(8, 8)}),
        Low(0x1004, NdOp::LOAD, RAX, {Address}),
        Low(0x100F, NdOp::RETURN, {}, {RAX})};
    if (Mode == 1)
      Original.Ops[4].Inputs[0] = NdVar::dataAddress(0x2000, 8, 0x2000);
    if (Mode == 2) {
      const auto Copy = NdVar::tmp(0x10008, 8);
      Original.Ops[4].Inputs[0] = Copy;
      Original.Ops.insert(Original.Ops.begin() + 4,
                          Low(0x1004, NdOp::COPY, Copy, {Address}));
    }
    if (Mode == 3 || Mode == 4) {
      Original.Ops[2].Inputs[0].Offset = 0x2008;
      if (Mode == 3)
        Original.Ops[3].Opcode = NdOp::INT_SUB;
      else
        Original.Ops[3].Inputs[1].Offset = uint64_t(-8);
    }
    if (Mode == 5)
      Original.Ops[2].Inputs[0].Provenance = ConstantAddressProvenance::Scalar;
    if (Mode == 6)
      Original.Ops[2].Inputs[0].Provenance = ConstantAddressProvenance::Unknown;
    if (Mode == 7)
      Original.Ops[2].Inputs[0].Provenance =
          ConstantAddressProvenance::AddressFragment;
    if (Mode == 8)
      Original.Ops[2].Inputs[0].Provenance =
          ConstantAddressProvenance::CodeAddress;
    if (Mode == 10)
      Original.Ops[2].Inputs[0].AddressOwnerVA = 0x3000;
    if (Mode == 11)
      Original.Ops[3].Inputs[1].Offset = 17;
    if (Mode == 12)
      Image.Segments[0].Data.resize(7);
    if (Mode == 13)
      Original.Ops[4].MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Mode == 14)
      Original.Ops[4].MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
    if (Mode == 15 || Mode == 16)
      Original.Ops[4] = Low(0x1004, NdOp::STORE, {},
                            {Address, Mode == 15 ? NdVar::scalar(0, 8) : SP});
    if (Mode == 17)
      Original.Ops.insert(Original.Ops.begin() + 4,
                          Low(0x1004, NdOp::COPY,
                              NdVar::tmp(Address.Offset + 2, 1),
                              {NdVar::scalar(0, 1)}));
    if (Mode == 18)
      Original.Ops[4].Inputs[0] = NdVar::tmp(Address.Offset + 1, 7);
    if (Mode == 19)
      Original.Ops[4].Addr = 0x1005;
    if (Mode == 20)
      Original.Ops[2].Inputs[0].Size = Original.Ops[2].Output.Size = 4;
    if (Mode == 21)
      Original.Ops.insert(Original.Ops.begin() + 5,
                          Low(0x1006, NdOp::LOAD, RAX, {RAX}));
    if (Mode == 22) {
      Original.Ops[3].Output = RAX;
      Original.Ops[4].Inputs[0] = RAX;
    }
    if (Mode == 23)
      Original.Ops.insert(
          Original.Ops.begin() + 4,
          Low(0x1004, NdOp::COPY, Address, {NdVar::scalar(0x2000, 8)}));
    if (Mode == 24) {
      Storage.VA = 0x2010;
      Image.Segments.push_back(Storage);
      Original.Ops[2].Inputs[0] = NdVar::dataAddress(0x2000, 8, 0x2000);
      Original.Ops[3].Inputs[1].Offset = 16;
    }
    if (Mode == 25) {
      Image.Segments[0].VA = uint64_t{1} << 63;
      Original.Ops[2].Inputs[0].Offset = INT64_MAX;
      Original.Ops[3].Inputs[1].Offset = 1;
    }
    if (Mode == 26)
      Original.Ops[4].Output.Size = 0;
    if (Mode == 28) {
      Original.Ops[2].Inputs[0] = NdVar::dataAddress(0x2000, 8, 0x2000);
    }
    if (Mode == 29)
      Original.Ops[3].Inputs[1].Provenance = ConstantAddressProvenance::Unknown;
    if (Mode == 30)
      Original.Ops[0].Inputs[1].Offset = 40;
    if (Mode == 31)
      Image.Segments[0].Flags =
          SegmentFlags::Readable | SegmentFlags::Executable;
    const auto Before = Original.Ops;
    const auto Expected =
        Mode <= 4 || Mode == 28 ? std::set<unsigned>{4} : std::set<unsigned>{};
    size_t Budget = Mode == 27 ? 1 : 4096;
    const BinaryImage *Current = Mode == 9 ? nullptr : &Image;
    EXPECT_EQ(
        unobservedSourceStackParameters(Func, {Original}, {}, Budget, Current),
        Expected);
    EXPECT_EQ(Original.Ops.size(), Before.size());
    for (size_t I = 0; I < Before.size(); ++I) {
      EXPECT_EQ(Original.Ops[I].Opcode, Before[I].Opcode);
      EXPECT_EQ(Original.Ops[I].Output, Before[I].Output);
    }
    if (Mode == 0) {
      const auto Used = 4096 - Budget;
      Budget = Used;
      EXPECT_EQ(unobservedSourceStackParameters(Func, {Original}, {}, Budget,
                                                Current),
                Expected);
      EXPECT_EQ(Budget, 0u);
      Budget = Used - 1;
      EXPECT_TRUE(
          unobservedSourceStackParameters(Func, {Original}, {}, Budget, Current)
              .empty());
    }
  }
}

TEST(SourceStackSpills, StackParameterHolesTrackPhysicalFrameAliases) {
  for (unsigned Mode = 0; Mode != 31; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func;
    Func.Entry = 0x1000;
    Func.CC = CallingConv::Win64;
    Func.Params.resize(7);
    for (unsigned P = 4; P < Func.Params.size(); ++P) {
      auto &Parameter = Func.Params[P];
      Parameter.Kind = MedVar::Param;
      Parameter.Id = P;
      Parameter.RegOff = kNoParamReg;
      Parameter.Size = 8;
      Parameter.TheArch = Arch::X64;
    }
    Func.Blocks.resize(4);
    std::vector<LowBlock> Original(4);
    for (unsigned I = 0; I != 4; ++I) {
      Func.Blocks[I].Id = Original[I].Id = I;
      Func.Blocks[I].StartAddr = Original[I].StartAddr = 0x1000 + 16 * I;
      Original[I].EndAddr = 0x1010 + 16 * I;
    }
    Original[0].Succs = {1, 2};
    Original[1].Preds = Original[2].Preds = {0};
    Original[1].Succs = Original[2].Succs = {3};
    Original[3].Preds = {1, 2};
    for (unsigned I = 0; I != 4; ++I) {
      Func.Blocks[I].Preds = Original[I].Preds;
      Func.Blocks[I].Succs = Original[I].Succs;
    }
    Func.Blocks[1].Ops = {op(NdOp::COPY, temp(11), {Func.Params[5]})};
    Func.Blocks[2].Ops = {op(NdOp::COPY, temp(12), {Func.Params[6]})};
    const auto SP = NdVar::reg(x86reg::RSP, 8);
    const auto RAX = NdVar::reg(x86reg::RAX, 8);
    const auto RCX = NdVar::reg(x86reg::RCX, 8);
    const auto Alias = NdVar::reg(Mode == 25 ? x86reg::RAX : x86reg::R11, 8);
    const auto Address = NdVar::tmp(0x10000, 8);
    const auto Low = [](va_t PC, NdOp Opcode, NdVar Output,
                        std::initializer_list<NdVar> Inputs) {
      LowOp Result;
      Result.Addr = PC;
      Result.Opcode = Opcode;
      Result.Output = Output;
      for (const auto &V : Inputs)
        Result.addInput(V);
      return Result;
    };
    Original[0].Ops = {
        Low(0x1000, NdOp::COPY, Alias, {SP}),
        Low(0x1003, NdOp::INT_SUB, SP, {SP, NdVar::scalar(40, 8)}),
        Low(0x1007, NdOp::COND_BR, {}, {NdVar::cst(0x1020, 8), RCX})};
    for (unsigned I : {1u, 2u}) {
      const va_t PC = Original[I].StartAddr;
      Original[I].Ops = {
          Low(PC, NdOp::INT_ADD, Alias, {Alias, NdVar::scalar(8, 8)}),
          Low(PC + 4, NdOp::BRANCH, {}, {NdVar::cst(0x1030, 8)})};
    }
    Original[3].Ops = {
        Low(0x1034, NdOp::INT_ADD, Address, {Alias, NdVar::scalar(40, 8)}),
        Low(0x1034, NdOp::LOAD, RAX, {Address}),
        Low(0x1038, NdOp::COPY, Alias, {NdVar::scalar(0, 8)}),
        Low(0x103A, NdOp::INT_ADD, SP, {SP, NdVar::scalar(40, 8)}),
        Low(0x103E, NdOp::RETURN, {}, {RAX})};
    Original[3].StartAddr = Func.Blocks[3].StartAddr = 0x1030;
    // The first original operation must also own the join's start address.
    Original[3].Ops.insert(Original[3].Ops.begin(),
                           Low(0x1030, NdOp::COPY, RCX, {NdVar::scalar(0, 8)}));
    if (Mode == 1)
      Original[3].Ops[1].Inputs[1].Offset = 32;
    if (Mode == 2) {
      Original[3].Ops[1].Inputs[1].Offset = 31;
      Original[3].Ops[2].Output.Size = 2;
    }
    if (Mode == 3)
      Original[2].Ops[0].Inputs[1].Offset = 16;
    if (Mode == 4)
      Original[2].Ops[0] =
          Low(0x1020, NdOp::COPY, Alias, {NdVar::scalar(0, 8)});
    if (Mode == 5)
      Original[2].Ops.insert(Original[2].Ops.begin() + 1,
                             Low(0x1023, NdOp::STORE, {}, {SP, Alias}));
    if (Mode == 6)
      Original[3].Ops[3] = Low(0x1038, NdOp::COPY, RAX, {Alias});
    if (Mode == 7)
      Original[3].Ops[3] = Low(0x1038, NdOp::COPY, RCX, {Alias});
    if (Mode == 6 || Mode == 7)
      Original[3].Ops.insert(
          Original[3].Ops.begin() + 4,
          Low(0x1039, NdOp::COPY, Alias, {NdVar::scalar(0, 8)}));
    if (Mode == 8) {
      Original[2].Ops[0].Opcode = NdOp::INT_SUB;
      Original[1].Ops[0].Opcode = NdOp::INT_SUB;
      Original[3].Ops[1].Inputs[1].Offset = 56;
    }
    if (Mode == 9)
      std::swap(Original[1], Original[2]);
    if (Mode == 10 || Mode == 11 || Mode == 12) {
      auto Narrow = Alias;
      Narrow.Size = Mode == 12 ? 1 : 4;
      if (Mode == 12)
        ++Narrow.Offset;
      Original[3].Ops.insert(
          Original[3].Ops.begin() + 1,
          Low(0x1031, NdOp::COPY, Narrow, {NdVar::scalar(0, Narrow.Size)}));
      if (Mode == 11)
        Original[3].Ops.insert(Original[3].Ops.begin() + 2,
                               Low(0x1032, NdOp::COPY, Alias, {SP}));
      if (Mode == 11)
        Original[3].Ops[3].Inputs[1].Offset = 88;
    }
    if (Mode == 13 || Mode == 14) {
      auto Narrow = Alias;
      Narrow.Size = 4;
      Original[3].Ops[3] =
          Low(0x1038, NdOp::COPY, Narrow, {NdVar::scalar(0, 4)});
      if (Mode == 14)
        Original[3].Ops.insert(Original[3].Ops.begin() + 4,
                               Low(0x1038, NdOp::INT_ZEXT, Alias, {Narrow}));
    }
    if (Mode == 15) {
      Original[2].Ops[0] = Low(0x1020, NdOp::COPY, RCX, {NdVar::scalar(0, 8)});
      Original[2].Ops.back() =
          Low(0x1024, NdOp::COND_BR, {}, {NdVar::cst(0x1020, 8), RCX});
      Original[2].Succs = {2, 3};
      Original[2].Preds.push_back(2);
      Original[1].Ops[0].Inputs[1].Offset = 0;
      Original[3].Ops[1].Inputs[1].Offset = 48;
    }
    if (Mode == 16) {
      Original[2].Ops.back() =
          Low(0x1024, NdOp::COND_BR, {}, {NdVar::cst(0x1020, 8), RCX});
      Original[2].Succs = {2, 3};
      Original[2].Preds.push_back(2);
    }
    if (Mode >= 17 && Mode <= 26) {
      Original[3].Ops[0] =
          Low(0x1030, NdOp::CALL, RAX, {NdVar::cst(0x3000, 8)});
      auto Call = op(NdOp::CALL, temp(10), {MedVar::makeConst(0x3000, 8)});
      Call.Addr = 0x1030;
      Call.OriginSeq = 0;
      Call.CallSiteId = 1;
      Call.CalleeRegisterArgs = Call.CalleeVectorArgs = Call.CalleeStackArgs =
          0;
      Call.CallPreservedGPRs = GPRFamilyMask{1}
                               << *gprFamilyOf(Arch::X64, Alias.Offset);
      MedCallUnreadRegisters Proof;
      Proof.Target = 0x3000;
      Proof.Instruction = Call.Addr;
      Proof.OriginSeq = Call.OriginSeq;
      Proof.Families = Call.CallPreservedGPRs;
      if (Mode == 18)
        Proof.Families = 0;
      if (Mode == 19)
        Proof.Target += 8;
      if (Mode == 20)
        ++Proof.Instruction;
      if (Mode == 21)
        ++Proof.OriginSeq;
      if (Mode == 22)
        Call.CallSiteId = 0;
      if (Mode == 23 || Mode == 24)
        Call.CallPreservedGPRs = 0;
      if (Mode == 24) {
        Original[3].Ops[1].Inputs[0] = SP;
        Original[3].Ops[1].Inputs[1].Offset = 88;
      }
      if (Mode == 25) {
        Original[3].Ops[0].Output = Alias;
        Original[3].Ops[1].Inputs[0] = SP;
        Original[3].Ops[1].Inputs[1].Offset = 88;
        Call.CallPreservedGPRs = 0;
      }
      Func.Blocks[3].Ops = {Call};
      if (Mode != 26)
        Func.CallUnreadRegisters.emplace(1, Proof);
    }
    if (Mode == 27)
      Original[3].Ops[2].MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Mode == 28)
      Original[3].Ops[1].Inputs[1].Offset = INT64_MAX;
    if (Mode == 29) {
      Original[3].Ops.erase(Original[3].Ops.begin() + 3);
      for (unsigned I = 0; I < 8; ++I)
        Original[3].Ops.insert(Original[3].Ops.begin() + 3 + I,
                               Low(0x1038, NdOp::COPY,
                                   NdVar::reg(Alias.Offset + I, 1),
                                   {NdVar::scalar(0, 1)}));
    }
    size_t Budget = Mode == 30 ? 1 : 8192;
    const auto Expected = Mode == 0 || Mode == 8 || Mode == 9 || Mode == 11 ||
                                  Mode == 14 || Mode == 15 || Mode == 17 ||
                                  Mode == 24 || Mode == 29
                              ? std::set<unsigned>{4}
                              : std::set<unsigned>{};
    EXPECT_EQ(unobservedSourceStackParameters(Func, Original, {0x3000}, Budget),
              Expected);
    if (Mode == 0) {
      const size_t Used = 8192 - Budget;
      Budget = Used;
      EXPECT_EQ(
          unobservedSourceStackParameters(Func, Original, {0x3000}, Budget),
          Expected);
      EXPECT_EQ(Budget, 0u);
      Budget = Used - 1;
      EXPECT_TRUE(
          unobservedSourceStackParameters(Func, Original, {0x3000}, Budget)
              .empty());
    }
  }
}

TEST(SourceStackSpills, AReintroducedRegisterInputStillReachesImplicitCalls) {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.CC = CallingConv::Win64;
  Func.FrameSize = 40;
  Func.Params = {rcx(0)};
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  const auto Home = temp(10);
  Block.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
               op(NdOp::COPY, rcx(0), {rcx(0)}),
               op(NdOp::INT_ADD, Home, {sp(0), MedVar::makeConst(8, 8)}),
               op(NdOp::STORE, none(), {Home, rcx(0)}),
               op(NdOp::STORE, none(), {Home, MedVar::makeConst(0, 8)}),
               op(NdOp::COPY, rcx(1), {MedVar::makeConst(42, 8)}),
               op(NdOp::COPY, rcx(2), {rcx(0)}),
               op(NdOp::CALL, temp(11), {MedVar::makeConst(0x3000, 8)}),
               op(NdOp::RETURN, none(), {MedVar::makeConst(0, 8)})};
  Func.Blocks.push_back(std::move(Block));
  markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
  EXPECT_TRUE(unobservedSourceSpillParameters(Func).empty());
}

TEST(SourceStackSpills, ADiscardedSpillDoesNotEraseADynamicCallTarget) {
  MedFunc Func;
  Func.Entry = 0x1000;
  Func.CC = CallingConv::Win64;
  Func.FrameSize = 40;
  Func.Params = {rcx(0)};
  MedBlock Block;
  Block.Id = 0;
  Block.StartAddr = Func.Entry;
  const auto Home = temp(10), Target = temp(11);
  Block.Ops = {op(NdOp::COPY, sp(0), {sp(0)}),
               op(NdOp::COPY, rcx(0), {rcx(0)}),
               op(NdOp::INT_ADD, Home, {sp(0), MedVar::makeConst(8, 8)}),
               op(NdOp::STORE, none(), {Home, rcx(0)}),
               op(NdOp::STORE, none(), {Home, MedVar::makeConst(0, 8)}),
               op(NdOp::COPY, Target, {rcx(0)}),
               op(NdOp::COPY, rcx(1), {MedVar::makeConst(0, 8)}),
               op(NdOp::CALL, temp(12), {Target}),
               op(NdOp::RETURN, none(), {MedVar::makeConst(0, 8)})};
  Func.Blocks.push_back(std::move(Block));
  markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(Func.Blocks[0].Ops[3].PrologueSave);
  EXPECT_TRUE(unobservedSourceSpillParameters(Func).empty());
}

TEST(SourceStackSpills, ConstantPrefixesNeverEvaluateUnavailableNeighbors) {
  for (unsigned Mode = 0; Mode < 10; ++Mode) {
    SCOPED_TRACE(Mode);
    MedFunc Func;
    MedBlock Block;
    Block.Id = 0;
    MedVar High = temp(1), Low = temp(2), Joined = temp(3), Copied = temp(4);
    High.Size = 7;
    Low.Size = 1;
    auto Literal = MedVar::makeConst(0x5A, 1);
    if (Mode == 3)
      Literal.Provenance = ConstantAddressProvenance::DataAddress;
    // This effect remains even when its loaded bytes are not call inputs.
    Block.Ops = {
        op(NdOp::LOAD, High, {MedVar::makeConst(0x2000, 8)}),
        op(NdOp::COPY, Low, {Literal}),
        op(NdOp::CONCAT, Joined,
           {High, Mode == 1 ? MedVar::makeUnspecified(1, Arch::X64) : Low}),
        op(NdOp::COPY, Copied, {Joined})};
    if (Mode == 4)
      Block.Ops[1].MemoryOrdering = NdMemoryOrdering::Acquire;
    if (Mode == 5)
      Block.Ops[3].Inputs[0] = Copied;
    auto Call = op(NdOp::CALL, temp(5),
                   {MedVar::makeConst(Mode == 6 ? 0x4000 : 0x3000, 8), Copied});
    MedCallInfo Info;
    Info.BlockId = 0;
    Info.OpIdx = Block.Ops.size();
    Info.TargetAddr = 0x3000;
    Info.Args = {Copied, MedVar::makeUnspecified(8, Arch::X64)};
    Info.IsIndirect = Mode == 7;
    Info.ArgumentsInCalleeRegisterOrder = Mode == 8;
    if (Mode == 9)
      Info.OpIdx = 0;
    Func.CallInfos.push_back(Info);
    Block.Ops.push_back(Call);
    Func.Blocks.push_back(std::move(Block));
    const auto Before = Func.CallInfos[0].Args;
    replaceKnownSourceCallArguments(Func, {{0x3000, {{0, Mode == 2 ? 2 : 1}}}});
    if (Mode == 0) {
      ASSERT_TRUE(Func.CallInfos[0].Args[0].isConst());
      EXPECT_EQ(Func.CallInfos[0].Args[0].ConstVal, 0x5Au);
      EXPECT_EQ(Func.CallInfos[0].Args[0].Size, 8u);
    } else {
      EXPECT_EQ(Func.CallInfos[0].Args[0], Before[0]);
    }
    EXPECT_EQ(Func.CallInfos[0].Args.size(), 2u);
    EXPECT_EQ(Func.CallInfos[0].Args[1], Before[1]);
    EXPECT_EQ(Func.Blocks[0].Ops.front().Opcode, NdOp::LOAD);
    EXPECT_EQ(Func.Blocks[0].Ops.back().Inputs[1], Copied);
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

TEST(SourceStackSpills, TemporaryRestoreChainsKeepEveryRegisterObserver) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (unsigned Mode = 0; Mode != 8; ++Mode) {
      SCOPED_TRACE(static_cast<unsigned>(Format));
      SCOPED_TRACE(Mode);
      MedFunc Func = pushedRbx(CallingConv::Win64);
      auto &Ops = Func.Blocks[0].Ops;
      Ops[4].Inputs[1] = MedVar::makeUnspecified(8, Arch::X64);
      Ops[7] = op(NdOp::COPY, temp(20), {temp(10)});
      Ops[8] = op(NdOp::STORE, none(), {sp(1), MedVar::makeConst(19, 8)});
      Ops[9] = op(NdOp::RETURN, none(), {MedVar::makeConst(19, 8)});
      if (Mode <= 1) {
        Ops.insert(Ops.begin() + 8,
                   op(NdOp::COPY, Mode ? reg(30, x86reg::R10, 1) : temp(21),
                      {temp(20)}));
      } else if (Mode <= 4) {
        // A dead temporary copy cannot hide the LOAD's physical observer.
        auto Register = Mode == 2   ? rbx(2)
                        : Mode == 3 ? reg(30, x86reg::RAX, 1)
                                    : sp(2);
        Ops[6].Output = Register;
        Ops[7].Inputs[0] = Register;
      } else if (Mode == 5) {
        Ops.back().Inputs[0] = temp(20);
      } else if (Mode == 6) {
        PhiNode Phi;
        Phi.Output = temp(21);
        Phi.Args = {{0, temp(20)}};
        Func.Blocks[0].Phis.push_back(std::move(Phi));
      } else {
        MedCallClobber Clobber;
        Clobber.Value = temp(21);
        Clobber.PreservedInput = temp(20);
        Clobber.PreservedPrefixSize = 4;
        Func.CallClobbers.push_back(Clobber);
      }
      markSourceStackAccesses(Func, Arch::X64, Format);
      EXPECT_EQ(save(Func).PrologueSave, Mode <= 1);
    }
}

TEST(SourceStackSpills, TemporaryRestoreChainsRespectTheTargetWidth) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(static_cast<unsigned>(Format));
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto Width = TRI.PointerSize;
      auto Func = pushedRbx();
      Func.FrameSize = Width;
      auto &Ops = Func.Blocks[0].Ops;
      Ops[4].Inputs[1] = MedVar::makeUnspecified(Width, Architecture);
      Ops[7] = op(NdOp::COPY, temp(20), {temp(10)});
      Ops[8] = op(NdOp::STORE, none(), {sp(1), MedVar::makeConst(19, Width)});
      Ops[9] = op(NdOp::RETURN, none(), {MedVar::makeConst(19, Width)});
      Ops.insert(Ops.begin() + 8, op(NdOp::COPY, temp(21), {temp(20)}));
      Ops[3].Inputs[1].ConstVal = Width;
      const auto Adapt = [&](MedVar &Value) {
        Value.TheArch = Architecture;
        Value.Size = Width;
        if (Value.Kind == MedVar::Reg) {
          if (Value.Id == kSP)
            Value.RegOff = TRI.StackPointer;
          else if (Value.Id == kRBX)
            Value.RegOff = TRI.CalleeSaveRegs.front();
          else if (Value.Id == kRCX)
            Value.RegOff = TRI.IntParamRegs.front();
        }
      };
      for (auto &Operation : Ops) {
        Adapt(Operation.Output);
        for (unsigned I = 0; I < Operation.NumInputs; ++I)
          Adapt(Operation.Inputs[I]);
      }
      markSourceStackAccesses(Func, Architecture, Format);
      EXPECT_TRUE(save(Func).PrologueSave);
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

MedFunc neutralReturnSlot(unsigned Width = 1, int64_t Offset = 0,
                          Arch Architecture = Arch::X64) {
  auto Func = readsReturnAddress();
  const auto PointerSize = getTargetRegInfo(Architecture).PointerSize;
  auto Stack = sp(0);
  Stack.TheArch = Architecture;
  Stack.Size = PointerSize;
  auto Address = temp(21);
  Address.TheArch = Architecture;
  Address.Size = PointerSize;
  auto Old = temp(20);
  Old.TheArch = Architecture;
  Old.Size = Width;
  Func.Blocks[0].Ops = {
      op(NdOp::COPY, Stack, {Stack}),
      op(NdOp::INT_ADD, Address,
         {Stack, MedVar::makeConst(uint64_t(Offset), PointerSize)}),
      op(NdOp::ATOMIC_ADD, Old, {Address, MedVar::makeConst(0, Width)}),
      op(NdOp::RETURN, none(), {MedVar::makeConst(7, PointerSize)})};
  Func.Blocks[0].Ops[2].MemoryOrdering =
      NdMemoryOrdering::SequentiallyConsistent;
  return Func;
}

TEST(MedNeutralReturnSlot, OnlyCompleteUnobservedSlotTransactionsAreProjected) {
  for (auto Architecture : {Arch::X86, Arch::X64})
    for (auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (unsigned Width : {1u, 2u, 4u, 8u})
        for (int64_t Offset = -1; Offset <= 8; ++Offset) {
          SCOPED_TRACE(testing::Message()
                       << unsigned(Architecture) << '/' << unsigned(Format)
                       << '/' << Width << '/' << Offset);
          auto Func = neutralReturnSlot(Width, Offset, Architecture);
          markSourceStackAccesses(Func, Architecture, Format);
          const auto &Atomic = Func.Blocks[0].Ops[2];
          const int64_t End = getTargetRegInfo(Architecture).PointerSize;
          const bool Fits =
              Offset >= 0 && Offset < End && Width <= End - Offset;
          EXPECT_EQ(Atomic.SourceReturnSlotOffset.has_value(), Fits);
          if (Fits && Atomic.SourceReturnSlotOffset)
            EXPECT_EQ(*Atomic.SourceReturnSlotOffset, Offset);
          EXPECT_EQ(Atomic.Opcode, NdOp::ATOMIC_ADD);
          EXPECT_FALSE(Atomic.PrologueSave);
          EXPECT_EQ(Func.Blocks[0].Ops.size(), 4u);
        }
}

TEST(MedNeutralReturnSlot, ChangedBodiesCannotRetainTheAddressProof) {
  for (unsigned Case = 0; Case < 14; ++Case) {
    SCOPED_TRACE(Case);
    auto Func = neutralReturnSlot();
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
    ASSERT_EQ(Func.Blocks[0].Ops[2].SourceReturnSlotOffset, 0);
    auto &Ops = Func.Blocks[0].Ops;
    auto &Atomic = Ops[2];
    auto Entry = StackEntryKind::Call;
    switch (Case) {
    case 0:
      Atomic.Inputs[1].ConstVal = 1;
      break;
    case 1:
      Atomic.Opcode = NdOp::ATOMIC_XCHG;
      break;
    case 2:
      Atomic.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
      break;
    case 3:
      Atomic.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 4:
      Atomic.MemoryOrdering = NdMemoryOrdering::Relaxed;
      break;
    case 5:
      Atomic.Inputs[0].Size = 4;
      break;
    case 6:
      Atomic.Inputs[1].Size = 2;
      break;
    case 7:
      Ops[3].Inputs[0] = Atomic.Output;
      break;
    case 8:
      Ops[1].Inputs[0] = rcx(0);
      break;
    case 9:
      Entry = StackEntryKind::ProcessEntry;
      break;
    case 10:
      Func.Blocks[0].Preds.push_back(0);
      break;
    case 11:
      Func.SkippedSSA = true;
      break;
    case 12:
      Ops[1].Inputs[1].ConstVal = 8;
      break;
    case 13:
      Func.AnalystStackFrame = true;
      break;
    }
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF, Entry);
    EXPECT_FALSE(Atomic.SourceReturnSlotOffset);
    EXPECT_FALSE(Atomic.PrologueSave);
  }
}

TEST(MedNeutralReturnSlot, FlagAndPhiReadersKeepOriginalReturnSlotBytes) {
  for (bool PhiRead : {false, true}) {
    auto Func = neutralReturnSlot();
    auto &Block = Func.Blocks[0];
    const auto Old = Block.Ops[2].Output;
    if (PhiRead) {
      PhiNode Phi;
      Phi.Output = temp(22);
      Phi.Args.emplace_back(0, Old);
      Block.Phis.push_back(std::move(Phi));
    } else {
      Block.Ops.insert(
          Block.Ops.end() - 1,
          op(NdOp::INT_EQUAL, temp(22), {Old, MedVar::makeConst(0, Old.Size)}));
      Block.Ops.back().Inputs[0] = temp(22);
    }
    markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
    EXPECT_FALSE(Block.Ops[2].SourceReturnSlotOffset);
  }
}

TEST(MedNeutralReturnSlot, AnotherEntryDoesNotInheritTheCallersSlot) {
  auto Func = neutralReturnSlot();
  MedBlock Other;
  Other.Id = 1;
  Other.StartAddr = Func.Entry + 16;
  Other.Ops = {Func.Blocks[0].Ops[2], Func.Blocks[0].Ops.back()};
  Other.Ops.front().Output.Id = 30;
  Func.Blocks.push_back(std::move(Other));
  markSourceStackAccesses(Func, Arch::X64, BinaryFormat::COFF);
  EXPECT_FALSE(Func.Blocks[0].Ops[2].SourceReturnSlotOffset);
  EXPECT_FALSE(Func.Blocks[1].Ops[0].SourceReturnSlotOffset);
}

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

TEST(SourceCallReturnReads, PhysicalPushOverwritesAnUnavailablePrivateSpill) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(static_cast<unsigned>(Format));
      auto Func = observedCallSlot();
      auto Image = callSlotImage();
      Image.Arch = Architecture;
      Image.Format = Format;
      Image.Bits =
          Architecture == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
      const auto Width = getTargetRegInfo(Architecture).PointerSize;
      auto &Ops = Func.Blocks[0].Ops;
      Ops.erase(Ops.begin() + 5);
      Ops.back().Inputs[0] = MedVar::makeConst(7, Width);
      Ops[3].Inputs[1] = MedVar::makeUnspecified(Width, Architecture);
      Ops[2].Inputs[1].ConstVal = Width;
      for (auto &Op : Ops) {
        Op.Output.TheArch = Architecture;
        Op.Output.Size = Width;
        for (unsigned I = 0; I < Op.NumInputs; ++I) {
          Op.Inputs[I].TheArch = Architecture;
          Op.Inputs[I].Size = Width;
        }
      }
      std::string Error;
      ASSERT_TRUE(preserveSourceCallReturnWrites(Func, Image, Error)) << Error;
      ASSERT_EQ(Ops.size(), 8u);
      EXPECT_EQ(Ops[5].Opcode, NdOp::STORE);
      EXPECT_EQ(Ops[5].Inputs[1].ConstVal, 0x100du);
      markSourceStackAccesses(Func, Architecture, Format, StackEntryKind::Call,
                              &Image);
      EXPECT_TRUE(Ops[3].PrologueSave);
      EXPECT_FALSE(Ops[5].PrologueSave);
      EXPECT_TRUE(validateSourceCallReturnReads(Func, Image, Error)) << Error;
    }
}

TEST(SourceCallReturnReads, AReadBeforeThePhysicalPushStillObservesTheSpill) {
  auto Func = observedCallSlot();
  auto Image = callSlotImage();
  Image.Bits = Bitness::Bits64;
  auto &Ops = Func.Blocks[0].Ops;
  Ops[3].Inputs[1] = MedVar::makeUnspecified(8, Arch::X64);
  std::swap(Ops[4], Ops[5]);
  std::string Error;
  ASSERT_TRUE(preserveSourceCallReturnWrites(Func, Image, Error)) << Error;
  EXPECT_EQ(Ops.size(), 9u);
  markSourceStackAccesses(Func, Arch::X64, BinaryFormat::ELF,
                          StackEntryKind::Call, &Image);
  EXPECT_FALSE(Ops[3].PrologueSave);
  EXPECT_EQ(Ops[4].Opcode, NdOp::LOAD);
}

TEST(SourceCallReturnReads, ScratchProjectionNeedsAnExactCallAndPrivateFrame) {
  for (unsigned Mode = 0; Mode < 8; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Func = observedCallSlot();
    auto Image = callSlotImage();
    Image.Bits = Bitness::Bits64;
    auto &Ops = Func.Blocks[0].Ops;
    Ops.erase(Ops.begin() + 5);
    Ops[3].Inputs[1] = MedVar::makeUnspecified(8, Arch::X64);
    Ops.back().Inputs[0] = MedVar::makeConst(7, 8);
    switch (Mode) {
    case 0:
      Image.Segments[0].Data[8] = 0xe9; // Synthetic tail, no physical push.
      break;
    case 1:
      Image.Segments[0].Data.resize(10); // Truncated near CALL.
      Image.Segments[0].Size = 10;
      break;
    case 2:
      Func.FrameSize = 32;
      break;
    case 3:
      Func.AnalystStackFrame = true;
      break;
    case 4:
      Func.SkippedSSA = true;
      break;
    case 5:
      Ops.insert(Ops.begin() + 4, op(NdOp::COPY, sp(2),
                                     {MedVar::makeUnspecified(8, Arch::X64)}));
      break;
    case 6:
      Func.Blocks[0].Preds = {4};
      break;
    case 7:
      Func.EntryKind = StackEntryKind::ProcessEntry;
      break;
    }
    const auto Count = Ops.size();
    std::string Error;
    EXPECT_TRUE(preserveSourceCallReturnWrites(Func, Image, Error)) << Error;
    EXPECT_EQ(Ops.size(), Count);
    EXPECT_FALSE(Ops[3].PrologueSave);
    EXPECT_EQ(Ops[3].Inputs[1].Kind, MedVar::Unspecified);
  }
}

TEST(SourceCallReturnReads, IndirectTargetRolesSurviveThePhysicalPush) {
  for (unsigned Mode = 0; Mode < 3; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Func = observedCallSlot();
    auto Image = callSlotImage();
    Image.Bits = Bitness::Bits64;
    auto &Bytes = Image.Segments[0].Data;
    auto &Ops = Func.Blocks[0].Ops;
    Bytes[8] = 0xff;
    MedVar Target = rcx(0);
    va_t PC = 0x100a;
    if (Mode == 0) {
      // A literal INDIR_CALL operand is pointer storage, such as an IAT slot.
      Bytes[9] = 0x15;
      Target = MedVar::makeConst(0x3000, 8);
      PC = 0x100e;
    } else if (Mode == 1) {
      Bytes[9] = 0xd1; // call rcx: a variable is the callable value.
    } else {
      // call [rsp - 8] already loads its target before the implicit push.
      Bytes[9] = 0x54;
      Bytes[10] = 0x24;
      Bytes[11] = 0xf8;
      Target = temp(23);
      PC = 0x100c;
    }
    Ops[4].Opcode = NdOp::INDIR_CALL;
    Ops[4].Inputs[0] = Target;
    if (Mode == 2)
      Ops.insert(Ops.begin() + 4, op(NdOp::LOAD, Target, {Ops[2].Output}));
    const size_t Store = Mode == 2 ? 6 : 5;
    std::string Error;
    ASSERT_TRUE(preserveSourceCallReturnWrites(Func, Image, Error)) << Error;
    ASSERT_EQ(Ops.size(), Mode == 2 ? 10u : 9u);
    if (Mode == 2) {
      EXPECT_EQ(Ops[4].Opcode, NdOp::LOAD);
      EXPECT_EQ(Ops[4].Inputs[0], Ops[2].Output);
      EXPECT_EQ(Ops[4].Output, Target);
    }
    EXPECT_EQ(Ops[Store].Opcode, NdOp::STORE);
    EXPECT_EQ(Ops[Store].Inputs[1].ConstVal, PC);
    EXPECT_EQ(Ops[Store + 1].Opcode, NdOp::INDIR_CALL);
    EXPECT_EQ(Ops[Store + 1].Inputs[0], Target);
    EXPECT_TRUE(validateSourceCallReturnReads(Func, Image, Error)) << Error;
  }
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

TEST(SourceCallReturnReads, MaterializesTheOriginalPCAndRevalidatesTheStore) {
  auto Func = observedCallSlot();
  auto Image = callSlotImage();
  Image.Bits = Bitness::Bits64;
  std::string Error;
  ASSERT_TRUE(preserveSourceCallReturnWrites(Func, Image, Error)) << Error;
  ASSERT_EQ(Func.Blocks[0].Ops.size(), 9u);
  const auto &Write = Func.Blocks[0].Ops[5];
  EXPECT_EQ(Write.Opcode, NdOp::STORE);
  EXPECT_EQ(Write.Inputs[1].ConstVal, 0x100du);
  EXPECT_EQ(Write.Inputs[1].Size, 8u);
  EXPECT_FALSE(Write.PrologueSave);
  EXPECT_TRUE(validateSourceCallReturnReads(Func, Image, Error)) << Error;
  ASSERT_TRUE(preserveSourceCallReturnWrites(Func, Image, Error)) << Error;
  EXPECT_EQ(Func.Blocks[0].Ops.size(), 9u);
  Func.Blocks[0].Ops[5].Inputs[1].ConstVal ^= 1;
  EXPECT_FALSE(validateSourceCallReturnReads(Func, Image, Error));
  Func.Blocks[0].Ops[5].Inputs[1].ConstVal ^= 1;
  // A changed call length cannot reuse a saved return-PC assertion.
  Image.Segments[0].Data[8] = 0xff;
  Image.Segments[0].Data[9] = 0xd0; // call rax: next PC is 0x100a
  EXPECT_FALSE(validateSourceCallReturnReads(Func, Image, Error));
}

TEST(SourceCallReturnReads, MaterializationRetainsPartialOutputAndFrameExtent) {
  auto Func = observedCallSlot();
  Func.FrameSize = 32;
  auto &Ops = Func.Blocks[0].Ops;
  Ops.insert(Ops.begin() + 5,
             op(NdOp::STORE, none(), {temp(20), MedVar::makeConst(29, 4)}));
  auto Image = callSlotImage();
  Image.Bits = Bitness::Bits64;
  std::string Error;
  ASSERT_TRUE(preserveSourceCallReturnWrites(Func, Image, Error)) << Error;
  EXPECT_EQ(Func.FrameSize, 40);
  EXPECT_EQ(Func.Blocks[0].Ops[5].Inputs[1].ConstVal, 0x100du);
  EXPECT_EQ(Func.Blocks[0].Ops[7].Inputs[1].ConstVal, 29u);
  EXPECT_EQ(Func.Blocks[0].Ops[7].Inputs[1].Size, 4u);
  EXPECT_TRUE(validateSourceCallReturnReads(Func, Image, Error)) << Error;
}

TEST(SourceCallReturnReads, MaterializationNeverTurnsTailsIntoPushes) {
  auto Func = observedCallSlot();
  auto Image = callSlotImage(0xe9);
  Image.Bits = Bitness::Bits64;
  std::string Error;
  EXPECT_TRUE(preserveSourceCallReturnWrites(Func, Image, Error)) << Error;
  EXPECT_EQ(Func.Blocks[0].Ops.size(), 7u);
}

TEST(SourceCallReturnReads, MaterializationIsTransactionalForUnknownStacks) {
  for (bool MissingStack : {false, true}) {
    auto Func = observedCallSlot();
    if (MissingStack)
      Func.Blocks[0].Ops.insert(
          Func.Blocks[0].Ops.begin() + 4,
          op(NdOp::COPY, sp(2), {MedVar::makeUnspecified(8, Arch::X64)}));
    const auto Before = Func.Blocks[0].Ops.size();
    auto Image = callSlotImage();
    Image.Bits = Bitness::Bits64;
    std::string Error;
    EXPECT_FALSE(preserveSourceCallReturnWrites(Func, Image, Error,
                                                MissingStack ? 262144 : 0));
    EXPECT_EQ(Func.Blocks[0].Ops.size(), Before);
    EXPECT_EQ(Func.Blocks[0].Ops[3].Inputs[1].ConstVal, 19u);
  }
}

TEST(SourceCallReturnReads, MaterializationRequiresAnActualCompleteNearCall) {
  for (unsigned Size : {9u, 10u, 11u, 12u}) {
    auto Func = observedCallSlot();
    auto Image = callSlotImage();
    Image.Bits = Bitness::Bits64;
    Image.Segments[0].Size = Size;
    Image.Segments[0].Data.resize(Size);
    std::string Error;
    EXPECT_FALSE(preserveSourceCallReturnWrites(Func, Image, Error));
    EXPECT_EQ(Func.Blocks[0].Ops.size(), 7u);
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
  for (unsigned Case = 0; Case != 52; ++Case) {
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
    std::vector<MedFunc> Relays;
    std::map<va_t, const MedFunc *> Callees;
    if (Case >= 22) {
      const unsigned Depth =
          Case >= 50 ? limits::ir::kSourceFrameBorrowCallDepth + (Case - 50)
                     : (Case == 24 ? 3 : 2);
      Relays.reserve(Depth - 1);
      for (unsigned I = 0; I + 1 < Depth; ++I) {
        auto &Relay = Relays.emplace_back();
        Relay.Entry = 0x2000 + I * 0x100;
        Relay.CC = CallingConv::Win64;
        Relay.Params = {rcx(0)};
        Relay.FrameSize = 0x28;
        MedBlock Forward;
        Forward.Id = 0;
        Forward.StartAddr = Relay.Entry;
        Forward.Ops = {
            op(NdOp::COPY, sp(0), {sp(0)}),
            op(NdOp::COPY, rcx(0), {rcx(0)}),
            op(NdOp::INT_SUB, sp(1), {sp(0), MedVar::makeConst(0x28, 8)}),
            op(NdOp::CALL, Word,
               {MedVar::makeConst(Relay.Entry + 0x100, 8), rcx(0)}),
            op(NdOp::INT_ADD, sp(2), {sp(1), MedVar::makeConst(0x28, 8)}),
            op(NdOp::RETURN, none(), {Word})};
        Forward.Ops[3].CalleeRegisterArgs = 1;
        Forward.Ops[3].CalleeStackArgs = 0;
        Forward.Ops[3].CalleeVectorArgs = 0;
        MedCallInfo Next;
        Next.BlockId = 0;
        Next.OpIdx = 3;
        Next.TargetAddr = Relay.Entry + 0x100;
        Next.Args = {rcx(0)};
        Relay.CallInfos = {Next};
        Relay.Blocks = {Forward};
      }
      Callee.Entry = 0x2000 + (Depth - 1) * 0x100;
      Callee.Blocks[0].StartAddr = Callee.Entry;
      auto &Leaf = Callee.Blocks[0];
      auto &First = Relays.front();
      if (Case == 23) {
        Leaf.Ops.insert(
            Leaf.Ops.begin() + 1,
            op(NdOp::INT_SUB, temp(30), {rcx(0), MedVar::makeConst(4, 8)}));
        Leaf.Ops[2].Inputs[0] = temp(30);
      }
      if (Case == 25 || Case == 47)
        Leaf.Ops[1].Inputs[0] =
            reg(30, Case == 25 ? x86reg::RAX : x86reg::RDX, 0);
      if (Case == 26) {
        Leaf.Ops.insert(
            Leaf.Ops.begin() + 1,
            op(NdOp::INT_ADD, temp(30), {sp(0), MedVar::makeConst(0x28, 8)}));
        Leaf.Ops[2].Inputs[0] = temp(30);
      }
      if (Case == 27 || Case == 28) {
        const va_t Target = Case == 27 ? 0x4000 : First.Entry;
        First.Blocks[0].Ops[3].Inputs[0] = MedVar::makeConst(Target, 8);
        First.CallInfos[0].TargetAddr = Target;
      }
      if (Case == 29)
        First.CallInfos[0].TargetAddr += 0x100;
      if (Case == 30 || Case == 42) {
        ExceptionFunction EH;
        EH.ParseStatus = ExceptionParseStatus::Complete;
        EH.Personality = Case == 30 ? ExceptionPersonality::GSHandlerCheck
                                    : ExceptionPersonality::None;
        Callee.ExceptionMetadata = EH;
      }
      if (Case == 31)
        First.Blocks[0].Ops[2].Inputs[0] = rcx(0);
      if (Case == 32)
        Callee.Blocks.clear();
      if (Case == 33)
        Callee.IsVariadic = true;
      if (Case == 34)
        Callee.IntegerArgumentRegisters = {x86reg::RDX};
      if (Case == 35)
        Callee.SourceParametersBound = true;
      if (Case == 36 || Case == 37) {
        First.CallInfos.clear();
        if (Case == 37)
          First.Blocks[0].Ops[3].NumInputs = 1;
      }
      if (Case == 38)
        Callee.Params.resize(5);
      if (Case == 39)
        First.CallInfos[0].OpIdx = 99;
      if (Case == 40)
        Leaf.Ops.insert(
            Leaf.Ops.begin() + 1,
            op(NdOp::STORE, none(), {MedVar::makeConst(0x4000, 8), rcx(0)}));
      if (Case == 41)
        Leaf.ExceptionalPreds.emplace_back();
      if (Case == 43)
        First.Blocks[0].Ops[3].IsTailCall = true;
      if (Case == 44)
        First.Blocks[0].Ops[3].CalleeVectorArgs = 1;
      if (Case == 45)
        First.CallInfos[0].OpIdx = 2;
      if (Case == 46)
        First.Blocks[0].Ops.back().Inputs[0] = rcx(0);
      if (Case == 48)
        Caller.Blocks[0].Ops[6].Opcode = NdOp::INDIR_CALL;
      if (Case == 49)
        Callee.Entry += 0x100;
      for (const auto &Relay : Relays)
        Callees.emplace(Relay.Entry, &Relay);
    }
    Callees.emplace(Callee.Entry, &Callee);
    markUnobservedBorrowedFrameStores(Caller, Arch::X64, BinaryFormat::COFF,
                                      Callees);
    EXPECT_EQ(Caller.Blocks[0].Ops[4].PrologueSave,
              Case == 0 || Case == 13 || Case == 22 || Case == 24 ||
                  Case == 36 || Case == 42 || Case == 50);
    EXPECT_FALSE(Caller.Blocks[0].Ops[5].PrologueSave);
    EXPECT_EQ(Caller.Blocks[0].Ops[6].Opcode,
              Case == 20 || Case == 21 || Case == 48 ? NdOp::INDIR_CALL
                                                     : NdOp::CALL);
  }
}

TEST(SourceFrameBorrows, PartialOverwritesMustPrecedeEveryBorrowRead) {
  for (bool Home : {false, true})
    for (unsigned Case = 0; Case != 8; ++Case) {
      SCOPED_TRACE(Home);
      SCOPED_TRACE(Case);
      MedFunc Caller;
      Caller.Entry = 0x1000;
      Caller.CC = CallingConv::Win64;
      Caller.FrameSize = 0x38;
      auto Byte = temp(20);
      Byte.Size = 1;
      const int64_t Offset = Home ? 8 : -16;
      MedBlock Entry;
      Entry.Id = 0;
      Entry.StartAddr = Caller.Entry;
      Entry.Succs = Case == 3 ? std::vector<int>{1, 2} : std::vector<int>{1};
      Entry.Ops = {
          op(NdOp::COPY, sp(0), {sp(0)}),
          op(NdOp::INT_SUB, sp(1), {sp(0), MedVar::makeConst(0x38, 8)}),
          op(NdOp::INT_ADD, temp(10),
             {sp(0), MedVar::makeConst(uint64_t(Offset), 8)}),
          op(NdOp::STORE, none(), {temp(10), rcx(0)})};
      MedBlock Overwrite;
      Overwrite.Id = 1;
      Overwrite.StartAddr = 0x1010;
      Overwrite.Preds = {0};
      Overwrite.Succs = {2};
      Overwrite.Ops = {
          op(NdOp::STORE, none(), {temp(10), MedVar::makeConst(37, 1)})};
      Overwrite.Ops[0].Dead = Case == 4;
      Overwrite.Ops[0].PrologueSave = Case == 5;
      MedBlock Read;
      Read.Id = 2;
      Read.StartAddr = 0x1020;
      Read.Preds = Case == 3 ? std::vector<int>{0, 1} : std::vector<int>{1};
      Read.Ops = {
          op(NdOp::CALL, Byte, {MedVar::makeConst(0x2000, 8), temp(10)}),
          op(NdOp::INT_ADD, sp(2), {sp(1), MedVar::makeConst(0x38, 8)}),
          op(NdOp::RETURN, none(), {Byte})};
      Read.Ops[0].CalleeRegisterArgs = 1;
      Read.Ops[0].CalleeStackArgs = 0;
      Read.Ops[0].CalleeVectorArgs = 0;
      if (Case == 1) {
        Read.Ops.insert(Read.Ops.begin() + 1, Overwrite.Ops.front());
        Overwrite.Ops.clear();
      }
      Caller.Blocks = {Entry, Overwrite, Read};
      MedCallInfo Call;
      Call.BlockId = 2;
      Call.OpIdx = 0;
      Call.TargetAddr = 0x2000;
      Call.Args = {temp(10)};
      Call.IsIndirect = Case == 6;
      Caller.CallInfos = {Call};
      MedFunc Callee;
      Callee.Entry = Call.TargetAddr;
      Callee.CC = CallingConv::Win64;
      Callee.Params = {rcx(0)};
      MedBlock Body;
      Body.Id = 0;
      Body.StartAddr = Callee.Entry;
      Body.Ops = {op(NdOp::COPY, rcx(0), {rcx(0)}),
                  op(NdOp::LOAD, Byte, {rcx(0)}),
                  op(NdOp::RETURN, none(), {Byte})};
      if (Case == 2)
        Body.Ops[1].Output.Size = 8;
      if (Case == 7)
        Body.Ops.insert(
            Body.Ops.begin() + 1,
            op(NdOp::STORE, none(), {MedVar::makeConst(0x3000, 8), rcx(0)}));
      Callee.Blocks = {Body};
      markUnobservedBorrowedFrameStores(Caller, Arch::X64, BinaryFormat::COFF,
                                        {{Callee.Entry, &Callee}});
      EXPECT_EQ(Caller.Blocks[0].Ops[3].PrologueSave, Case == 0);
    }
}

} // namespace
