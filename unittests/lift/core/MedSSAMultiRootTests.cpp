//===- MedSSAMultiRootTests.cpp - Multiple-root SSA tests ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/PrologueStackAllocation.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/LLVMSourceUnknownValue.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/MedLLVMEmitter.h"
#include "neverd/backend/llvm/WindowsEHMetadata.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/X86/X87Context.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/SEHFrameProof.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/LowToMedError.h"
#include "neverd/ir/med/MedCallReturnReads.h"
#include "neverd/ir/med/MedPrologueSaves.h"
#include "neverd/ir/med/MedStackAlignment.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"

#include <algorithm>
#include <regex>
#include <set>

namespace {

using namespace neverd;

TEST(MedSSAMultiRoot, SharedHandlersRetainEveryDistinctExceptionalPredecessor) {
  for (Arch Architecture : {Arch::X86, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    constexpr int Sources = 256;
    constexpr int Targets = 4;
    constexpr int Contexts = 8;
    constexpr int Duplicates = 3;
    const auto Width = getTargetRegInfo(Architecture).PointerSize;
    LowFunc Low;
    Low.Entry = 0x1000;
    Low.Name = "shared_exception_handlers";
    // Removing the empty interval between sources and handlers must remap
    // every destination while keeping the source state and instruction.
    Low.Blocks.resize(Sources + 1 + Targets);
    std::vector<std::vector<ExceptionalEdge>> Expected(Targets);
    for (int I = 0; I < static_cast<int>(Low.Blocks.size()); ++I) {
      auto &Block = Low.Blocks[I];
      Block.Id = I;
      Block.StartAddr = Low.Entry + 16 * I;
      Block.EndAddr = Block.StartAddr + 8;
      if (I == Sources)
        continue;
      LowOp Return;
      Return.Addr = Block.StartAddr;
      Return.Opcode = NdOp::RETURN;
      Return.addInput(NdVar::scalar(0, Width));
      Block.Ops.push_back(Return);
      if (I >= Sources)
        continue;
      for (int J = 0; J < Targets; ++J) {
        const va_t Target = Low.Entry + 16 * (Sources + 1 + J);
        std::vector<ExceptionalEdge> Edges = {
            {Sources + 1 + J, Target, ExceptionalEdgeKind::CxxCleanup, 0, 3},
            {Sources + 1 + J, Target, ExceptionalEdgeKind::CxxCleanup, 1, 3},
            {Sources + 1 + J, Target, ExceptionalEdgeKind::CxxCleanup, 0, 4},
            {Sources + 1 + J, Target, ExceptionalEdgeKind::CxxCleanup, 0, 3,
             Block.StartAddr + 1},
            {Sources + 1 + J, Target, ExceptionalEdgeKind::CxxCatch, 0, 3},
            {Sources + 1 + J, Target + 1, ExceptionalEdgeKind::CxxCleanup, 0,
             3}};
        // Identical sets of regions and states do not imply identical pairs.
        // Both domains must survive, and a separately owned equal domain must
        // deduplicate by value rather than by shared_ptr identity.
        for (bool Crossed : {false, true}) {
          auto Edge = Edges.front();
          Edge.SharedContexts =
              std::make_shared<const ExceptionalEdgeContextDomain>(
                  std::vector<ExceptionalEdgeContext>{{0, Crossed ? 4 : 3},
                                                      {1, Crossed ? 3 : 4}});
          Edges.push_back(std::move(Edge));
        }
        for (auto Edge : Edges) {
          Block.ExceptionalSuccs.push_back(Edge);
          Edge.BlockId = I;
          Low.Blocks[Sources + 1 + J].ExceptionalPreds.push_back(Edge);
          Expected[J].push_back(Edge);
        }
        // Duplicate metadata does not create another incoming occurrence.
        Block.ExceptionalSuccs.push_back(Edges[0]);
        Block.ExceptionalSuccs.push_back(Edges[2]);
        auto Duplicate = Edges[6];
        Duplicate.SharedContexts =
            std::make_shared<const ExceptionalEdgeContextDomain>(
                std::vector<ExceptionalEdgeContext>{{1, 4}, {0, 3}});
        Block.ExceptionalSuccs.push_back(std::move(Duplicate));
      }
      Block.ExceptionalSuccs.push_back(
          {-1, 0x80000, ExceptionalEdgeKind::CxxCleanup, 0, 3});
    }
    auto Med = LowToMedConverter().convert(Low, Architecture);
    ASSERT_EQ(Med.Blocks.size(), Sources + Targets);
    for (int J = 0; J < Targets; ++J) {
      const auto &Handler = Med.Blocks[Sources + J];
      EXPECT_EQ(Handler.ExceptionalPreds.size(), Sources * Contexts);
      EXPECT_EQ(Handler.ExceptionalPreds, Expected[J]);
    }
    for (int I = 0; I < Sources; ++I) {
      const auto &Edges = Med.Blocks[I].ExceptionalSuccs;
      ASSERT_EQ(Edges.size(), Targets * (Contexts + Duplicates) + 1);
      for (int J = 0; J < Targets; ++J)
        for (int K = 0; K < Contexts + Duplicates; ++K)
          EXPECT_EQ(Edges[J * (Contexts + Duplicates) + K].BlockId,
                    Sources + J);
      EXPECT_EQ(Edges.back().BlockId, -1);
    }
    EXPECT_TRUE(verifyMedFunc(Med, "shared-exception-handlers"));
  }
}

TEST(MedSSAMultiRoot, MergesDefinitionsFromIndependentSourcesAtAJoin) {
  constexpr va_t EntryVA = 0x1000;
  constexpr va_t ResumeVA = 0x1100;
  constexpr va_t JoinVA = 0x1200;
  constexpr uint64_t EntryValue = 0x11;
  constexpr uint64_t ResumeValue = 0x22;
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);

  LowFunc Low;
  Low.Entry = EntryVA;
  Low.Name = "two_source_join";
  Low.Blocks.resize(3);

  auto BuildSource = [&](LowBlock &Block, int Id, va_t Address,
                         uint64_t Value) {
    Block.Id = Id;
    Block.StartAddr = Address;
    Block.EndAddr = Address + 1;
    Block.Succs = {2};

    LowOp Define;
    Define.Opcode = NdOp::COPY;
    Define.Addr = Address;
    Define.Output = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
    Define.addInput(NdVar::cst(Value, TRI.PointerSize));
    Block.Ops.push_back(Define);

    LowOp Branch;
    Branch.Opcode = NdOp::BRANCH;
    Branch.Addr = Address;
    Branch.addInput(NdVar::cst(JoinVA, TRI.PointerSize));
    Block.Ops.push_back(Branch);
  };

  BuildSource(Low.Blocks[0], 0, EntryVA, EntryValue);
  BuildSource(Low.Blocks[1], 1, ResumeVA, ResumeValue);

  LowBlock &Join = Low.Blocks[2];
  Join.Id = 2;
  Join.StartAddr = JoinVA;
  Join.EndAddr = JoinVA + 1;
  Join.Preds = {0, 1};
  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = JoinVA;
  Return.addInput(NdVar::reg(TRI.IntReturnReg, TRI.PointerSize));
  Join.Ops.push_back(Return);

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X64);
  ASSERT_EQ(Med.Blocks.size(), 3u);
  const MedBlock &MedJoin = Med.Blocks[2];
  ASSERT_EQ(MedJoin.Phis.size(), 1u);

  const PhiNode &Phi = MedJoin.Phis.front();
  ASSERT_EQ(Phi.Output.Kind, MedVar::Reg);
  EXPECT_EQ(Phi.Output.RegOff, TRI.IntReturnReg);
  ASSERT_EQ(Phi.Args.size(), 2u);
  EXPECT_EQ(Phi.Args[0].first, 0);
  EXPECT_EQ(Phi.Args[1].first, 1);
  EXPECT_EQ(Phi.Args[0].second, MedVar::makeConst(EntryValue, TRI.PointerSize));
  EXPECT_EQ(Phi.Args[1].second,
            MedVar::makeConst(ResumeValue, TRI.PointerSize));

  ASSERT_FALSE(MedJoin.Ops.empty());
  const MedOp &MedReturn = MedJoin.Ops.back();
  ASSERT_EQ(MedReturn.Opcode, NdOp::RETURN);
  ASSERT_EQ(MedReturn.NumInputs, 1u);
  EXPECT_EQ(MedReturn.Inputs[0].Id, Phi.Output.Id);
  EXPECT_EQ(MedReturn.Inputs[0].SSAVer, Phi.Output.SSAVer);
  EXPECT_TRUE(verifyMedFunc(Med, "multi-root-join"));
}

TEST(MedSSAMultiRoot, UnwrittenPhiEdgeCannotBorrowASiblingDefinition) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
    SCOPED_TRACE(static_cast<int>(Architecture));
    const auto &TRI = getTargetRegInfo(Architecture);
    const uint16_t Width = TRI.PointerSize;
    LowFunc Low;
    Low.Entry = 0x1000;
    Low.Name = "partial_high_word";
    Low.Blocks.resize(3);
    for (int I = 0; I < 3; ++I) {
      Low.Blocks[I].Id = I;
      Low.Blocks[I].StartAddr = Low.Entry + 16 * I;
      Low.Blocks[I].EndAddr = Low.Entry + 16 * I + 1;
    }
    Low.Blocks[0].Succs = {1, 2};
    Low.Blocks[1].Preds = {0};
    Low.Blocks[1].Succs = {2};
    Low.Blocks[2].Preds = {0, 1};
    LowOp Branch;
    Branch.Opcode = NdOp::COND_BR;
    Branch.Addr = Low.Entry;
    Branch.addInput(NdVar::cst(0x1020, Width));
    Branch.addInput(NdVar::reg(TRI.IntReturnReg, Width));
    Low.Blocks[0].Ops.push_back(Branch);
    LowOp High;
    High.Opcode = NdOp::COPY;
    High.Addr = 0x1010;
    High.Output = NdVar::reg(TRI.IntReturnReg2, Width);
    High.addInput(NdVar::cst(1, Width));
    Low.Blocks[1].Ops.push_back(High);
    LowOp Value = High;
    Value.Addr = 0x1020;
    Value.Output = NdVar::reg(TRI.IntReturnReg, Width);
    Low.Blocks[2].Ops.push_back(Value);
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.Addr = 0x1020;
    Return.addInput(Value.Output);
    Low.Blocks[2].Ops.push_back(Return);
    const MedFunc Med = LowToMedConverter().convert(Low, Architecture);
    ASSERT_TRUE(verifyMedFunc(Med, "unwritten-phi-edge"));
    bool Found = false;
    for (const PhiNode &Phi : Med.Blocks[2].Phis) {
      if (Phi.Output.RegOff != TRI.IntReturnReg2)
        continue;
      ASSERT_EQ(Phi.Args.size(), 2u);
      const MedVar &Incoming = Phi.Args[0].second;
      // AArch64 may already bind the high register as an incoming argument.
      // Either that entry value or an explicit unknown is valid; the sibling
      // block's constant is not.
      if (Incoming.Kind == MedVar::Reg) {
        EXPECT_EQ(Incoming.RegOff, TRI.IntReturnReg2);
        EXPECT_EQ(Incoming.SSAVer, 0);
      } else {
        EXPECT_EQ(Incoming.Kind, MedVar::Unspecified);
      }
      EXPECT_EQ(Phi.Args[1].second, MedVar::makeConst(1, Width));
      Found = true;
    }
    EXPECT_TRUE(Found);
  }
}

TEST(MedSSAMultiRoot, DoesNotInventADownstreamRootFromBlockOrder) {
  constexpr va_t EntryVA = 0x2000;
  constexpr va_t DownstreamVA = 0x2100;
  constexpr va_t SourceVA = 0x2200;
  constexpr uint64_t SourceValue = 0x33;
  const TargetRegInfo &TRI = getTargetRegInfo(Arch::X64);

  LowFunc Low;
  Low.Entry = EntryVA;
  Low.Name = "source_scc_order";
  Low.Blocks.resize(3);

  LowBlock &Entry = Low.Blocks[0];
  Entry.Id = 0;
  Entry.StartAddr = EntryVA;
  Entry.EndAddr = EntryVA + 1;
  LowOp EntryReturn;
  EntryReturn.Opcode = NdOp::RETURN;
  EntryReturn.Addr = EntryVA;
  Entry.Ops.push_back(EntryReturn);

  // This sink deliberately has a lower block id than its disconnected source.
  // Root discovery must follow the condensation graph, not vector order.
  LowBlock &Downstream = Low.Blocks[1];
  Downstream.Id = 1;
  Downstream.StartAddr = DownstreamVA;
  Downstream.EndAddr = DownstreamVA + 1;
  Downstream.Preds = {2};
  LowOp DownstreamReturn;
  DownstreamReturn.Opcode = NdOp::RETURN;
  DownstreamReturn.Addr = DownstreamVA;
  DownstreamReturn.addInput(NdVar::reg(TRI.IntReturnReg, TRI.PointerSize));
  Downstream.Ops.push_back(DownstreamReturn);

  LowBlock &Source = Low.Blocks[2];
  Source.Id = 2;
  Source.StartAddr = SourceVA;
  Source.EndAddr = SourceVA + 1;
  Source.Succs = {1};
  LowOp Define;
  Define.Opcode = NdOp::COPY;
  Define.Addr = SourceVA;
  Define.Output = NdVar::reg(TRI.IntReturnReg, TRI.PointerSize);
  Define.addInput(NdVar::cst(SourceValue, TRI.PointerSize));
  Source.Ops.push_back(Define);
  LowOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.Addr = SourceVA;
  Branch.addInput(NdVar::cst(DownstreamVA, TRI.PointerSize));
  Source.Ops.push_back(Branch);

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X64);
  ASSERT_EQ(Med.Blocks.size(), 3u);
  const MedBlock &MedDownstream = Med.Blocks[1];
  ASSERT_TRUE(MedDownstream.Phis.empty());
  ASSERT_FALSE(MedDownstream.Ops.empty());
  const MedOp &MedReturn = MedDownstream.Ops.back();
  ASSERT_EQ(MedReturn.Opcode, NdOp::RETURN);
  ASSERT_EQ(MedReturn.NumInputs, 1u);
  ASSERT_TRUE(MedReturn.Inputs[0].isConst());
  EXPECT_EQ(MedReturn.Inputs[0].ConstVal, SourceValue);
  EXPECT_TRUE(verifyMedFunc(Med, "source-scc-order"));
}

TEST(MedSSAMultiRoot, LoopLiveInsRespectDefinitionsAndUseOrder) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool DefineAtEntry : {false, true})
        for (bool DefineBeforeUse : {false, true}) {
          SCOPED_TRACE(static_cast<int>(Architecture));
          SCOPED_TRACE(static_cast<int>(Format));
          SCOPED_TRACE(DefineAtEntry);
          SCOPED_TRACE(DefineBeforeUse);
          const auto &TRI = getTargetRegInfo(Architecture);
          const auto Arguments = TRI.integerArgumentLayout(Format).Registers;
          ASSERT_GE(Arguments.size(), 2u);
          const uint64_t ValueReg = Arguments[1];
          const uint16_t Width = TRI.PointerSize;
          LowFunc Low;
          Low.Entry = 0x1000;
          Low.Name = "loop_live_in";
          Low.Blocks.resize(4);
          for (int B = 0; B < 4; ++B) {
            Low.Blocks[B].Id = B;
            Low.Blocks[B].StartAddr = Low.Entry + 16 * B;
            Low.Blocks[B].EndAddr = Low.Entry + 16 * B + 12;
          }
          Low.Blocks[0].Succs = {1};
          Low.Blocks[1].Preds = {0, 2};
          Low.Blocks[1].Succs = {2, 3};
          Low.Blocks[2].Preds = {1};
          Low.Blocks[2].Succs = {1};
          Low.Blocks[3].Preds = {1};

          auto Define = [&](int B) {
            LowOp Op;
            Op.Opcode = NdOp::COPY;
            Op.Addr = Low.Blocks[B].StartAddr + 4 * Low.Blocks[B].Ops.size();
            Op.Output = NdVar::reg(ValueReg, Width);
            Op.addInput(NdVar::scalar(0x33, Width));
            Low.Blocks[B].Ops.push_back(Op);
          };
          if (DefineAtEntry)
            Define(0);
          LowOp Branch;
          Branch.Opcode = NdOp::BRANCH;
          Branch.Addr = Low.Entry + 8;
          Branch.addInput(NdVar::cst(0x1010, Width));
          Low.Blocks[0].Ops.push_back(Branch);
          Branch.Addr = 0x1028;

          LowOp Condition;
          Condition.Opcode = NdOp::COND_BR;
          Condition.Addr = 0x1010;
          Condition.addInput(NdVar::cst(0x1030, Width));
          Condition.addInput(NdVar::reg(Arguments[0], Width));
          Low.Blocks[1].Ops.push_back(Condition);

          // Keep the store observable. A definition after its read does not
          // kill the first iteration's incoming value; one before it does.
          if (DefineBeforeUse)
            Define(2);
          LowOp Store;
          Store.Opcode = NdOp::STORE;
          Store.Addr = 0x1020 + 4 * Low.Blocks[2].Ops.size();
          Store.addInput(NdVar::cst(0x4000, Width));
          Store.addInput(NdVar::reg(ValueReg, Width));
          Low.Blocks[2].Ops.push_back(Store);
          if (!DefineBeforeUse)
            Define(2);
          Low.Blocks[2].Ops.push_back(Branch);
          LowOp Return;
          Return.Opcode = NdOp::RETURN;
          Return.Addr = 0x1030;
          Return.addInput(NdVar::scalar(0, Width));
          Low.Blocks[3].Ops.push_back(Return);

          const auto Med =
              LowToMedConverter().convert(Low, Architecture, Format);
          ASSERT_FALSE(Med.SkippedSSA);
          ASSERT_TRUE(verifyMedFunc(Med, "loop-live-in"));
          EXPECT_EQ(std::any_of(Med.Params.begin(), Med.Params.end(),
                                [&](const MedVar &Param) {
                                  return Param.Kind == MedVar::Param &&
                                         Param.RegOff == ValueReg &&
                                         Param.Id >= 0;
                                }),
                    !DefineAtEntry && !DefineBeforeUse);
          unsigned Stores = 0;
          for (const auto &Block : Med.Blocks)
            for (const auto &Op : Block.Ops)
              Stores += Op.Opcode == NdOp::STORE;
          EXPECT_EQ(Stores, 1u);
        }
}

TEST(MedTempIdentity, SeparatesReusedLowTempSlotsByWidth) {
  constexpr va_t EntryVA = 0x3000;

  LowFunc Low;
  Low.Entry = EntryVA;
  Low.Name = "temp_width_identity";
  Low.Blocks.resize(1);

  LowBlock &Block = Low.Blocks.front();
  Block.Id = 0;
  Block.StartAddr = EntryVA;
  Block.EndAddr = EntryVA + 4;

  LowOp ByteDef;
  ByteDef.Opcode = NdOp::POPCOUNT;
  ByteDef.Addr = EntryVA;
  ByteDef.Output = NdVar::tmp(TmpBase, 1);
  ByteDef.addInput(NdVar::scalar(0x5a, 1));
  Block.Ops.push_back(ByteDef);
  LowOp StoreByte;
  StoreByte.Opcode = NdOp::STORE;
  StoreByte.Addr = EntryVA;
  StoreByte.addInput(NdVar::cst(0x4000, 8));
  StoreByte.addInput(ByteDef.Output);
  Block.Ops.push_back(StoreByte);

  LowOp WideDef;
  WideDef.Opcode = NdOp::POPCOUNT;
  WideDef.Addr = EntryVA + 1;
  WideDef.Output = NdVar::tmp(TmpBase, 2);
  WideDef.addInput(NdVar::scalar(0x1234, 2));
  Block.Ops.push_back(WideDef);
  LowOp StoreWideDef;
  StoreWideDef.Opcode = NdOp::STORE;
  StoreWideDef.Addr = EntryVA + 1;
  StoreWideDef.addInput(NdVar::cst(0x4002, 8));
  StoreWideDef.addInput(WideDef.Output);
  Block.Ops.push_back(StoreWideDef);

  LowOp WideUpdate;
  WideUpdate.Opcode = NdOp::POPCOUNT;
  WideUpdate.Addr = EntryVA + 2;
  WideUpdate.Output = NdVar::tmp(TmpBase, 2);
  WideUpdate.addInput(NdVar::scalar(0xabcd, 2));
  Block.Ops.push_back(WideUpdate);
  LowOp StoreWideUpdate = StoreWideDef;
  StoreWideUpdate.Addr = EntryVA + 2;
  StoreWideUpdate.Inputs[1] = WideUpdate.Output;
  Block.Ops.push_back(StoreWideUpdate);

  LowOp Return;
  Return.Opcode = NdOp::RETURN;
  Return.Addr = EntryVA + 3;
  Block.Ops.push_back(Return);

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X64);
  ASSERT_EQ(Med.Blocks.size(), 1u);
  auto FindOp = [&](va_t Addr, NdOp Opcode) -> const MedOp * {
    for (const MedOp &Op : Med.Blocks.front().Ops)
      if (Op.Addr == Addr && Op.Opcode == Opcode)
        return &Op;
    return nullptr;
  };
  const MedOp *MedByteDef = FindOp(EntryVA, NdOp::POPCOUNT);
  const MedOp *MedWideDef = FindOp(EntryVA + 1, NdOp::POPCOUNT);
  const MedOp *MedWideUpdate = FindOp(EntryVA + 2, NdOp::POPCOUNT);
  ASSERT_NE(MedByteDef, nullptr);
  ASSERT_NE(MedWideDef, nullptr);
  ASSERT_NE(MedWideUpdate, nullptr);
  EXPECT_NE(MedByteDef->Output.Id, MedWideDef->Output.Id)
      << "a Med SSA identity must have exactly one value width";
  EXPECT_EQ(MedWideDef->Output.Id, MedWideUpdate->Output.Id);
  EXPECT_NE(MedWideDef->Output.SSAVer, MedWideUpdate->Output.SSAVer);
}

BinaryImage makeFixedSEHFrameImage() {
  constexpr va_t Entry = 0x140001000;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = 0x3d;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  // push rdi; sub rsp,128; mov dword [rsp+36],1; nop; jmp join.
  const uint8_t Prologue[] = {0x57, 0x48, 0x81, 0xec, 0x80, 0, 0,
                              0,    0xc7, 0x44, 0x24, 0x24, 1, 0,
                              0,    0,    0x90, 0xeb, 0x1d};
  std::copy(std::begin(Prologue), std::end(Prologue), Text.Data.begin());
  // Handler: eax=[rsp+36]; eax+=20; [rsp+36]=eax; jmp join.
  const uint8_t Handler[] = {0x8b, 0x44, 0x24, 0x24, 0x83, 0xc0, 20,
                             0x89, 0x44, 0x24, 0x24, 0xeb, 3};
  std::copy(std::begin(Handler), std::end(Handler), Text.Data.begin() + 0x20);
  const uint8_t Join[] = {0x8b, 0x44, 0x24, 0x24, 0x48, 0x81, 0xc4,
                          0x80, 0,    0,    0,    0x5f, 0xc3};
  std::copy(std::begin(Join), std::end(Join), Text.Data.begin() + 0x30);
  Img.Segments.push_back(std::move(Text));
  Section Section;
  Section.Name = ".text";
  Section.VA = Entry;
  Section.Size = 0x3d;
  Section.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Section));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, 0x3d));
  ExceptionFunction EH;
  EH.CodeRange = {Entry, Entry + 0x3d};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.UnwindVersion = 1;
  EH.UnwindFlags = 1;
  EH.PrologueSize = 8;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  UnwindOperation Alloc;
  Alloc.Kind = UnwindOperationKind::AllocateLarge;
  Alloc.CodeOffset = 8;
  Alloc.StackOffset = 128;
  EH.UnwindOperations.push_back(Alloc);
  UnwindOperation Push;
  Push.Kind = UnwindOperationKind::PushNonVolatile;
  Push.CodeOffset = 1;
  Push.Register = 7;
  EH.UnwindOperations.push_back(Push);
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Entry + 0x10, Entry + 0x11};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Entry + 0x20;
  EH.SEH->Scopes.push_back(Scope);
  Img.ExceptionMetadata.Functions.push_back(EH);
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

LowFunc decodeFixedSEHFrame(const BinaryImage &Img) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::X64));
  return CFGBuilder().build(Img, Dec, Img.Entry, "fixed_seh_frame");
}

// Resolve only copies, constant SP arithmetic and equal-input PHIs. This
// asserts the actual memory operand's entry-SP-relative identity on both
// paths, rather than looking for a particular initializer spelling.
std::optional<int64_t> entrySPOffset(const MedFunc &F, const MedVar &V,
                                     std::set<std::pair<int, int>> Seen = {}) {
  if (V.Kind == MedVar::Reg &&
      V.RegOff == getTargetRegInfo(Arch::X64).StackPointer && V.SSAVer == 0)
    return 0;
  if (!Seen.emplace(V.Id, V.SSAVer).second)
    return std::nullopt;
  for (const MedBlock &B : F.Blocks) {
    for (const PhiNode &Phi : B.Phis) {
      if (Phi.Output.Id != V.Id || Phi.Output.SSAVer != V.SSAVer)
        continue;
      std::optional<int64_t> Result;
      for (const auto &[Pred, Arg] : Phi.Args) {
        auto Offset = entrySPOffset(F, Arg, Seen);
        if (!Offset || (Result && Result != Offset))
          return std::nullopt;
        Result = Offset;
      }
      return Result;
    }
    for (const MedOp &Op : B.Ops) {
      if (Op.Output.Id != V.Id || Op.Output.SSAVer != V.SSAVer)
        continue;
      if (Op.Opcode == NdOp::COPY && Op.NumInputs == 1)
        return entrySPOffset(F, Op.Inputs[0], Seen);
      if ((Op.Opcode == NdOp::INT_ADD || Op.Opcode == NdOp::INT_SUB) &&
          Op.NumInputs == 2 && Op.Inputs[1].isConst()) {
        auto Base = entrySPOffset(F, Op.Inputs[0], Seen);
        if (Base)
          return *Base + (Op.Opcode == NdOp::INT_ADD ? 1 : -1) *
                             static_cast<int64_t>(Op.Inputs[1].ConstVal);
      }
    }
  }
  return std::nullopt;
}

TEST(MedSEHEstablisherFrame, NormalHandlerAndJoinUseTheSameLocalSlot) {
  auto Img = makeFixedSEHFrameImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-establisher"));
  std::set<va_t> Seen;
  const std::set<va_t> Expected = {Img.Entry + 8, Img.Entry + 0x20,
                                   Img.Entry + 0x27, Img.Entry + 0x30};
  for (const MedBlock &B : Med.Blocks)
    for (const MedOp &Op : B.Ops)
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Expected.count(Op.Addr)) {
        ASSERT_GT(Op.NumInputs, 0u);
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -100) << Op.Addr;
        Seen.insert(Op.Addr);
      }
  EXPECT_EQ(Seen, Expected);
}

TEST(MedSEHEstablisherFrame, CallBeforeStackRestoreKeepsHandlerFrameLive) {
  for (bool Indirect : {false, true}) {
    SCOPED_TRACE(Indirect);
    auto Img = makeFixedSEHFrameImage();
    auto &Code = Img.Segments.front().Data;
    Code.resize(0x45, 0xcc);
    Img.Segments.front().Size = Code.size();
    Img.Sections.front().Size = Code.size();
    Img.Symbols.front().Size = Code.size();
    Img.ExceptionMetadata.Functions.front().CodeRange.End =
        Img.Entry + Code.size();
    // Remember the allocated SP in a preserved register. Neither the handler
    // nor the join explicitly reads SP before the join restores it from RDI.
    const uint8_t Save[] = {0x48, 0x89, 0xe7, 0x90, 0x90, 0x90, 0x90, 0x90};
    std::copy(std::begin(Save), std::end(Save), Code.begin() + 8);
    const uint8_t Handler[] = {0x31, 0xc0, 0xeb, 0x0c};
    std::copy(std::begin(Handler), std::end(Handler), Code.begin() + 0x20);
    const uint8_t Join[] = {0xe8, 0xcb, 0,    0,    0,    0x48, 0x89,
                            0xfc, 0x8b, 0x44, 0x24, 0x24, 0x48, 0x81,
                            0xc4, 0x80, 0,    0,    0,    0x5f, 0xc3};
    std::copy(std::begin(Join), std::end(Join), Code.begin() + 0x30);
    if (Indirect) {
      Code[0x30] = 0xff;
      Code[0x31] = 0xd2; // call rdx
      std::fill(Code.begin() + 0x32, Code.begin() + 0x35, 0x90);
    }
    Img.ExceptionMetadata.rebuildIndex();
    auto Low = decodeFixedSEHFrame(Img);
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Img);
    auto Med = Converter.convert(Low, Arch::X64, BinaryFormat::COFF);
    ASSERT_TRUE(verifyMedFunc(Med, "seh-call-before-stack-restore"));
    std::string Error;
    EXPECT_TRUE(validateSourceCallReturnReads(
        Med, Img, Error, limits::ir::kSourceCallReturnReadWork, &Low))
        << Error;
    unsigned Seen = 0;
    for (const auto &Block : Med.Blocks)
      if (Block.StartAddr == Img.Entry + 0x20)
        for (const auto &Op : Block.Ops)
          if (Op.Output.Kind == MedVar::Reg &&
              Op.Output.RegOff == getTargetRegInfo(Arch::X64).StackPointer) {
            EXPECT_EQ(entrySPOffset(Med, Op.Output), -136);
            ++Seen;
          }
    EXPECT_EQ(Seen, 1u);
  }
}

TEST(MedSEHEstablisherFrame, AllocationFactsCannotEraseAddressProvenance) {
  for (auto Provenance :
       {ConstantAddressProvenance::Unknown, ConstantAddressProvenance::Scalar,
        ConstantAddressProvenance::DataAddress,
        ConstantAddressProvenance::CodeAddress}) {
    SCOPED_TRACE(static_cast<unsigned>(Provenance));
    for (bool Mapped : {false, true}) {
      SCOPED_TRACE(Mapped);
      BinaryImage Image;
      Image.Arch = Arch::X64;
      Image.Bits = Bitness::Bits64;
      Image.Format = BinaryFormat::COFF;
      if (Mapped) {
        Segment Data;
        Data.VA = 0x80;
        Data.Size = Data.FileSz = 8;
        Data.Flags = SegmentFlags::Readable;
        Data.Data.resize(8);
        Image.Segments.push_back(std::move(Data));
      }
      LowFunc Function;
      Function.Entry = 0x1000;
      Function.ExceptionMetadata.emplace();
      Function.ExceptionMetadata->PrologueSize = 2;
      Function.Blocks.resize(1);
      auto &Block = Function.Blocks.front();
      Block.Id = 0;
      Block.StartAddr = Function.Entry;
      Block.EndAddr = Function.Entry + 2;
      const NdVar SP = NdVar::reg(x86reg::RSP, 8);
      const NdVar Amount = NdVar::reg(x86reg::RAX, 8);
      LowOp Copy;
      Copy.Addr = Function.Entry;
      Copy.Opcode = NdOp::COPY;
      Copy.Output = Amount;
      auto Constant = NdVar::cst(0x80, 8);
      Constant.Provenance = Provenance;
      Copy.addInput(Constant);
      LowOp Allocate;
      Allocate.Addr = Function.Entry + 1;
      Allocate.Opcode = NdOp::INT_SUB;
      Allocate.Output = SP;
      Allocate.addInput(SP);
      Allocate.addInput(Amount);
      Block.Ops = {Copy, Allocate};
      for (unsigned I = 0; I < 2; ++I) {
        LowInstructionBoundary Boundary;
        Boundary.Address = Function.Entry + I;
        Boundary.Size = Boundary.OpCount = 1;
        Boundary.FirstOp = I;
        Block.InstructionBoundaries.push_back(Boundary);
      }
      canonicalizePrologueStackAllocation(Image, Function);
      const bool Scalar =
          Provenance == ConstantAddressProvenance::Scalar ||
          (Provenance == ConstantAddressProvenance::Unknown && !Mapped);
      EXPECT_EQ(Block.Ops[1].Inputs[1].isConst(), Scalar);
      if (Scalar)
        EXPECT_EQ(Block.Ops[1].Inputs[1].Offset, 0x80u);
      else
        EXPECT_EQ(Block.Ops[1].Inputs[1], Amount);
    }
  }
}

TEST(MedSEHEstablisherFrame, StackProbeKeepsTheDecodedAllocationConstant) {
  for (unsigned Variant = 0; Variant != 4; ++Variant) {
    SCOPED_TRACE(Variant);
    auto Img = makeFixedSEHFrameImage();
    auto &Text = Img.Segments.front();
    Text.Size = Text.FileSz = 0x80;
    Text.Data.resize(Text.Size, 0xcc);
    Img.Sections.front().Size = Text.Size;
    std::fill_n(Text.Data.begin(), 0x20, 0xcc);
    const uint8_t Prefix[] = {
        0x57,                      // push rdi
        0xb8, 0x80, 0,    0,    0, // mov eax,128
        0xe8, 0x65, 0,    0,    0, // call helper at +70h
        0x48, 0x2b, 0xe0,          // sub rsp,rax
        0xc7, 0x44, 0x24, 0x24, 1,   0,
        0,    0,    0x90, 0xeb, 0x17}; // protected nop; jmp join
    std::copy(std::begin(Prefix), std::end(Prefix), Text.Data.begin());
    Text.Data[0x70] = 0xc3;
    Symbol Probe = Symbol::makeFunc(Img.Entry + 0x70, 1);
    Probe.Name = Variant == 1 ? "ordinary_callee" : "__chkstk";
    Probe.Origin = Variant == 2 ? NameOrigin::Analysis : NameOrigin::Stated;
    Img.Symbols.push_back(Probe);
    auto &EH = Img.ExceptionMetadata.Functions.front();
    EH.PrologueSize = 14;
    EH.UnwindOperations.front().CodeOffset = 14;
    EH.SEH->Scopes.front().GuardedRange = {Img.Entry + 22, Img.Entry + 23};
    // A decoded constant still has to agree with the independently supplied
    // unwind allocation. The helper's name cannot repair this mismatch.
    if (Variant == 3)
      Text.Data[2] = 0x40;
    Img.ExceptionMetadata.rebuildIndex();
    auto Low = decodeFixedSEHFrame(Img);
    unsigned Calls = 0;
    for (const auto &Block : Low.Blocks)
      for (const auto &Op : Block.Ops)
        Calls += Op.Opcode == NdOp::CALL;
    EXPECT_EQ(Calls,
              1u); // stack probes and their fault behavior remain visible
    size_t Work = limits::kMaxSEHEstablisherFrameWork;
    auto Frame = proveLowSEHEstablisherFrame(
        Low, Img.Entry + 0x20, getTargetRegInfo(Arch::X64), false, Work);
    const bool Proven = bool(Frame);
    const std::string Error = Proven ? "" : llvm::toString(Frame.takeError());
    EXPECT_EQ(Proven, Variant == 0) << Error;
    if (Proven) {
      EXPECT_EQ(Frame->FrameBytes, 136u);
      const auto Med =
          LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
      EXPECT_TRUE(verifyMedFunc(Med, "stack-probe-frame"));
    }
  }
}

TEST(MedSEHEstablisherFrame, ResumableBreakpointKeepsTheFollowingFramePath) {
  auto Img = makeFixedSEHFrameImage();
  Img.Segments.front().Data[0x10] = 0xcc;
  Img.ExceptionMetadata.Functions.front().SEH->Scopes.front().GuardedRange.End =
      Img.Entry + 0x13;
  Img.ExceptionMetadata.rebuildIndex();
  auto Low = decodeFixedSEHFrame(Img);
  ASSERT_TRUE(Low.hasCompleteLiftCoverage());
  bool MiddleBreakpoint = false;
  for (size_t I = 0; I < Low.Blocks.size(); ++I) {
    auto &Block = Low.Blocks[I];
    for (const auto &Insn : Block.InstructionBoundaries)
      if (Insn.Address == Img.Entry + 0x10) {
        EXPECT_EQ(Insn.Control, LowInstructionControl::Terminator);
        EXPECT_TRUE(hasLowInstructionControlFlag(
            Insn.ControlFlags, LowInstructionControlFlag::Resumable));
        MiddleBreakpoint = &Insn != &Block.InstructionBoundaries.back();
      }
  }
  ASSERT_TRUE(MiddleBreakpoint);
  for (bool Source : {false, true}) {
    size_t Work = limits::kMaxSEHEstablisherFrameWork;
    auto Proof = proveLowSEHEstablisherFrame(
        Low, Img.Entry + 0x20, getTargetRegInfo(Arch::X64), false, Work, {},
        false,
        Source ? LowSEHFrameUse::SourceProjection
               : LowSEHFrameUse::InvocationIdentity);
    ASSERT_TRUE(bool(Proof)) << llvm::toString(Proof.takeError());
    auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
    EXPECT_TRUE(verifyMedFunc(Med, "resumable-seh-frame"));
  }
  // A terminal transfer in the middle of a block still invalidates the
  // frame proof; only the decoder's explicit resumption contract admits it.
  for (auto &Block : Low.Blocks)
    for (auto &Insn : Block.InstructionBoundaries)
      if (Insn.Address == Img.Entry + 0x10)
        Insn.ControlFlags = LowInstructionControlFlag::None;
  size_t Work = limits::kMaxSEHEstablisherFrameWork;
  auto Proof = proveLowSEHEstablisherFrame(
      Low, Img.Entry + 0x20, getTargetRegInfo(Arch::X64), false, Work, {},
      false, LowSEHFrameUse::SourceProjection);
  EXPECT_FALSE(bool(Proof));
  llvm::consumeError(Proof.takeError());
}

TEST(MedSEHEstablisherFrame, SourceIndependentEntriesKeepUnknownStackValues) {
  for (bool SharedHandler : {false, true}) {
    SCOPED_TRACE(SharedHandler);
    auto Img = makeFixedSEHFrameImage();
    if (SharedHandler)
      // The ordinary branch also reaches the exception handler.
      Img.Segments.front().Data[0x12] = 0x0d;
    auto Low = decodeFixedSEHFrame(Img);
    Low.ModuleAnalysisRoots.insert(Img.Entry + 0x10);
    Low.OrdinaryModuleAnalysisRoots.insert(Img.Entry + 0x10);
    EXPECT_THROW(
        LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF),
        LowToMedConversionError);
    size_t Work = limits::kMaxSEHEstablisherFrameWork;
    auto Strict = proveLowSEHEstablisherFrame(Low, Img.Entry + 0x20,
                                              getTargetRegInfo(Arch::X64),
                                              SharedHandler, Work);
    EXPECT_FALSE(Strict);
    llvm::consumeError(Strict.takeError());

    LowToMedConverter Converter;
    Converter.setSEHSourceProjectionEnabled(true);
    auto Med = Converter.convert(Low, Arch::X64, BinaryFormat::COFF);
    EXPECT_TRUE(Low.SEHLocalUnwindContinuations.empty());
    EXPECT_TRUE(verifyMedFunc(Med, "seh-source-independent-entry"));
    EXPECT_TRUE(Med.HasUnresolvedSEHFrame);
    bool SawHandler = false;
    for (const auto &Block : Med.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::LOAD && Op.Addr == Img.Entry + 0x20) {
          SawHandler = true;
          // The entry store uses SP-100; the runtime entry cannot reuse it.
          EXPECT_FALSE(entrySPOffset(Med, Op.Inputs[0]));
        }
    EXPECT_TRUE(SawHandler);
    auto High = MedToHighConverter().convert(Med, Arch::X64);
    EXPECT_TRUE(High.HasUnresolvedSEHFrame);
    std::string Source;
    llvm::raw_string_ostream Stream(Source);
    CEmitterOptions Options;
    Options.TheArch = Arch::X64;
    EXPECT_TRUE(HighCEmitter().emit({High}, Stream, Options));
    EXPECT_NE(Source.find("unresolved runtime stack"), std::string::npos)
        << Source;
    EXPECT_NE(Source.find("unknown value"), std::string::npos) << Source;
    if (SharedHandler) {
      // The handler's entry copies stay on the runtime's entry into it.
      const std::string Handler = llvm::utohexstr(Img.Entry + 0x20);
      const size_t EntryAt =
          Source.find("Native exception entry to the handler @ 0x" + Handler);
      ASSERT_NE(EntryAt, std::string::npos) << Source;
      EXPECT_NE(Source.find("goto L_" + Handler + ";", EntryAt),
                std::string::npos)
          << Source;
      // Any other clause code would leave its native position: refuse it.
      auto Moved = High;
      for (auto &Stmt : Moved.Body)
        if (Stmt.Kind == StmtKind::SEHTry)
          for (auto &Body : Stmt.EHClauseBodies)
            if (!Body.empty())
              Body.pop_back();
      std::string Refused;
      llvm::raw_string_ostream RefusedStream(Refused);
      EXPECT_THROW(HighCEmitter().emit({Moved}, RefusedStream, Options),
                   std::invalid_argument);
    }
    llvm::LLVMContext Context;
    EXPECT_THROW((void)MedLLVMEmitter().emit({Med}, Context),
                 std::runtime_error);
    MedLLVMEmitter SourceEmitter;
    SourceEmitter.setPreserveUnknownSourceValues(true);
    auto Module = SourceEmitter.emit({Med}, Context, "source-seh-frame",
                                     Arch::X64, {}, &Img, BinaryFormat::COFF);
    ASSERT_TRUE(Module);
    ASSERT_FALSE(llvm::verifyModule(*Module, &llvm::errs()));
    bool ExplicitFrame = false, UnknownValue = false;
    for (const auto &Function : *Module) {
      ExplicitFrame |=
          Function.hasFnAttribute(windows_eh_md::UnresolvedSourceFrame);
      for (const auto &Block : Function)
        for (const auto &Instruction : Block)
          UnknownValue |= llvm_source_unknown::isValue(&Instruction);
    }
    EXPECT_TRUE(ExplicitFrame);
    EXPECT_TRUE(UnknownValue);
    std::string LLVMSource;
    llvm::raw_string_ostream LLVMStream(LLVMSource);
    EXPECT_TRUE(LLVMCEmitter().emit(*Module, LLVMStream, Options));
    EXPECT_NE(LLVMSource.find("unresolved runtime stack"), std::string::npos)
        << LLVMSource;
    EXPECT_NE(LLVMSource.find("unknown machine state"), std::string::npos)
        << LLVMSource;
    // Reusing an emitter for native output never inherits source admission.
    SourceEmitter.setPreserveUnknownSourceValues(false);
    EXPECT_THROW((void)SourceEmitter.emit({Med}, Context, "native-seh-frame",
                                          Arch::X64, {}, &Img,
                                          BinaryFormat::COFF),
                 std::runtime_error);
  }
}

TEST(MedSEHEstablisherFrame, SourceSwitchRequiresEveryResolvedTarget) {
  auto Img = makeFixedSEHFrameImage();
  auto Low = decodeFixedSEHFrame(Img);
  int Dispatch = -1, Join = -1;
  for (const auto &Block : Low.Blocks) {
    if (!Block.Ops.empty() && Block.Ops.back().Addr == Img.Entry + 0x11)
      Dispatch = Block.Id;
    if (Block.StartAddr == Img.Entry + 0x30)
      Join = Block.Id;
  }
  ASSERT_GE(Dispatch, 0);
  ASSERT_GE(Join, 0);
  // Model the resolver's published target set, independently of this frame
  // consumer. An unresolved jmp rax is instead decoded as a tail call, so
  // start with the complete branch CFG and replace the dispatch operation.
  // Unknown or changed dispatches must not borrow that closure.
  auto &Branch = Low.Blocks[Dispatch].Ops.back();
  Branch.Opcode = NdOp::INDIR_BR;
  Branch.Inputs[0] = NdVar::reg(x86reg::RAX, 8);
  auto &Boundary = Low.Blocks[Dispatch].InstructionBoundaries.back();
  Boundary.ControlFlags |= LowInstructionControlFlag::Indirect;
  Boundary.Immediate.reset();
  JumpTable Table;
  Table.InsnAddr = Img.Entry + 0x11;
  Table.Targets = {Img.Entry + 0x30};
  Low.JumpTables = {Table};
  Low.UnsafeIndirectBranchAddresses.erase(Table.InsnAddr);
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Low;
    if (Mutation == 1)
      Changed.JumpTables.clear();
    if (Mutation == 2)
      Changed.JumpTables.front().Targets.push_back(Img.Entry + 0x20);
    if (Mutation == 3)
      Changed.JumpTables.front().MutatedUnsafe = true;
    if (Mutation == 4)
      Changed.UnsafeIndirectBranchAddresses.insert(Table.InsnAddr);
    if (Mutation == 5)
      Changed.JumpTables.push_back(Table);
    size_t Work = limits::kMaxSEHEstablisherFrameWork;
    auto Proof = proveLowSEHEstablisherFrame(
        Changed, Img.Entry + 0x20, getTargetRegInfo(Arch::X64), false, Work, {},
        false, LowSEHFrameUse::SourceProjection);
    const bool Proved = bool(Proof);
    std::string Failure = Proved ? "" : llvm::toString(Proof.takeError());
    EXPECT_EQ(Proved, Mutation == 0) << Failure;
    if (Proved) {
      EXPECT_FALSE(Proof->DynamicSP);
      EXPECT_FALSE(Proof->HasIndependentEntries);
      EXPECT_EQ(Proof->FrameBytes, 136u);
    }
  }
}

TEST(MedSEHEstablisherFrame, SourceRequiresExplicitIndependentEntry) {
  auto Img = makeFixedSEHFrameImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto *Protected = Low.blockFor(Img.Entry + 0x10);
  auto *Join = Low.blockFor(Img.Entry + 0x30);
  ASSERT_NE(Protected, nullptr);
  ASSERT_NE(Join, nullptr);
  // Redirect the post-prologue instruction to the epilogue. The protected
  // scope is now disconnected, so it needs its own published entry state.
  auto &Prefix = Low.Blocks.front();
  auto &Boundary = Prefix.InstructionBoundaries.back();
  Prefix.Ops.resize(Boundary.FirstOp);
  LowOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.Addr = Boundary.Address;
  Branch.addInput(NdVar::cst(Join->StartAddr, 8));
  Prefix.Ops.push_back(Branch);
  Boundary.OpCount = 1;
  Boundary.Control = LowInstructionControl::Branch;
  Boundary.ControlFlags = LowInstructionControlFlag::Branch;
  Boundary.Immediate = Join->StartAddr;
  Prefix.Succs = {Join->Id};
  Protected->Preds.clear();
  Join->Preds.push_back(Prefix.Id);
  Low.OrdinaryModuleAnalysisRoots.clear();
  for (bool Published : {false, true}) {
    SCOPED_TRACE(Published);
    if (Published)
      Low.OrdinaryModuleAnalysisRoots.insert(Protected->StartAddr);
    size_t Work = limits::kMaxSEHEstablisherFrameWork;
    auto Proof = proveLowSEHEstablisherFrame(
        Low, Img.Entry + 0x20, getTargetRegInfo(Arch::X64), false, Work, {},
        false, LowSEHFrameUse::SourceProjection);
    const bool Proved = bool(Proof);
    std::string Failure = Proved ? "" : llvm::toString(Proof.takeError());
    EXPECT_EQ(Proved, Published) << Failure;
    if (Proved)
      EXPECT_TRUE(Proof->HasIndependentEntries);
  }
}

// clang-cl ends a guarded range one byte past the return address of its
// last call, so the instruction there is guarded: here the first one of the
// join the handler resumes at.  The epilogue after it is guarded by no
// scope, so its stack release cannot unsettle a handler's frame; inside the
// range it would.
TEST(MedSEHEstablisherFrame, EpilogueAfterTheGuardedRangeIsUnchecked) {
  for (va_t RangeEnd : {0x16, 0x1a}) {
    SCOPED_TRACE(RangeEnd);
    auto Img = makeFixedSEHFrameImage();
    auto &Code = Img.Segments.front().Data;
    std::fill(Code.begin() + 0x10, Code.end(), 0xcc);
    // 0x10: call 0x38; 0x15: mov eax,[rsp+36]; add rsp,128; pop rdi; ret.
    const uint8_t Join[] = {0xe8, 0x23, 0,    0,    0, 0x8b, 0x44, 0x24, 0x24,
                            0x48, 0x81, 0xc4, 0x80, 0, 0,    0,    0x5f, 0xc3};
    std::copy(std::begin(Join), std::end(Join), Code.begin() + 0x10);
    // 0x28: eax=[rsp+36]; eax+=20; [rsp+36]=eax; jmp 0x15.
    const uint8_t Handler[] = {0x8b, 0x44, 0x24, 0x24, 0x83, 0xc0, 20,
                               0x89, 0x44, 0x24, 0x24, 0xeb, 0xe0};
    std::copy(std::begin(Handler), std::end(Handler), Code.begin() + 0x28);
    Code[0x38] = 0xc3;
    auto &EH = Img.ExceptionMetadata.Functions.front();
    EH.CodeRange.End = Img.Entry + 0x35;
    Img.Symbols.front().Size = 0x35;
    auto &Scope = EH.SEH->Scopes.front();
    Scope.GuardedRange = {Img.Entry + 0x10, Img.Entry + RangeEnd};
    Scope.HandlerVA = Img.Entry + 0x28;
    Img.ExceptionMetadata.rebuildIndex();
    auto Low = decodeFixedSEHFrame(Img);
    ASSERT_NE(Low.blockFor(Img.Entry + 0x15), nullptr);
    size_t Work = limits::kMaxSEHEstablisherFrameWork;
    auto Frame = proveLowSEHEstablisherFrame(
        Low, Img.Entry + 0x28, getTargetRegInfo(Arch::X64), false, Work);
    const bool Proven = bool(Frame);
    const std::string Error = Proven ? "" : llvm::toString(Frame.takeError());
    EXPECT_EQ(Proven, RangeEnd == 0x16) << Error;
    if (!Proven)
      EXPECT_NE(Error.find("SP is not stable"), std::string::npos) << Error;
    else
      EXPECT_EQ(Frame->FrameBytes, 136u);
  }
}

LowFunc makeFixedCxxContinuationFrame(
    const BinaryImage &Image = makeFixedSEHFrameImage()) {
  auto Low = decodeFixedSEHFrame(Image);
  auto &EH = *Low.ExceptionMetadata;
  EH.SEH.reset();
  EH.Cxx.emplace();
  auto &Cxx = *EH.Cxx;
  Cxx.NativeFuncInfoVA = Low.Entry + 0x1000;
  Cxx.MaxState = 2;
  Cxx.UnwindMap.resize(2);
  Cxx.UnwindMap[0].ToState = -1;
  Cxx.UnwindMap[1].ToState = -1;
  Cxx.TryBlocks.push_back({0, 0, 1, {}});
  CxxCatchHandler Catch;
  Catch.HandlerVA = Low.Entry + 0x100;
  Cxx.TryBlocks.front().Handlers.push_back(Catch);
  Cxx.IPMap = {{Low.Entry, -1}, {Low.Entry + 0x10, 0}, {Low.Entry + 0x11, -1}};
  for (auto &B : Low.Blocks) {
    B.ExceptionalPreds.clear();
    for (auto &E : B.ExceptionalSuccs) {
      E.Kind = ExceptionalEdgeKind::CxxCatch;
      E.BlockId = -1;
      E.TargetVA = Catch.HandlerVA;
      E.ExplicitContext = {0, 0};
    }
  }
  Low.CxxContinuationEntries.push_back(
      {Low.Entry + 0x20, Catch.HandlerVA, Catch.HandlerVA + 0x10, 7});
  return Low;
}

TEST(MedCxxContinuationFrame, CatchResumeAndNormalPathAddressTheSameLocal) {
  auto Low = makeFixedCxxContinuationFrame();
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "cxx-continuation-frame"));
  unsigned Loads = 0;
  for (const auto &B : Med.Blocks)
    for (const auto &Op : B.Ops)
      if (Op.Opcode == NdOp::LOAD &&
          (Op.Addr == Low.Entry + 0x20 || Op.Addr == Low.Entry + 0x30)) {
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -100);
        ++Loads;
      }
  EXPECT_EQ(Loads, 2u);
}

TEST(MedCxxContinuationFrame, ProtectedSourcesFollowCompactedBlockIdentities) {
  for (bool Synchronous : {false, true}) {
    SCOPED_TRACE(Synchronous);
    auto Image = makeFixedSEHFrameImage();
    auto &Code = Image.Segments.front().Data;
    Code.resize(0x101, 0xcc);
    Code[0x100] = 0xc3;
    // RDI changes before the throwing call, through two removable jump blocks.
    const uint8_t SetRDI[] = {0x48, 0xc7, 0xc7, 0x55, 0, 0, 0};
    std::copy(std::begin(SetRDI), std::end(SetRDI), Code.begin() + 8);
    Code[0xf] = 0x90;
    const uint8_t Path[] = {0xeb, 2,    0xcc, 0xcc, 0xeb, 2,    0xcc, 0xcc,
                            0xe8, 0xe3, 0,    0,    0,    0xeb, 0x11};
    std::copy(std::begin(Path), std::end(Path), Code.begin() + 0x10);
    // The resumed catch observes the nonvolatile value at the throwing call.
    const uint8_t Handler[] = {0x48, 0x89, 0x7c, 0x24, 0x24, 0xeb, 9};
    std::copy(std::begin(Handler), std::end(Handler), Code.begin() + 0x20);
    Image.ExceptionMetadata.Functions.front().SEH->Scopes.front().GuardedRange =
        {Image.Entry + 0x18, Image.Entry + 0x1d};
    auto Low = makeFixedCxxContinuationFrame(Image);
    auto &Cxx = *Low.ExceptionMetadata->Cxx;
    Cxx.IsSynchronous = Synchronous;
    Cxx.IPMap = {
        {Low.Entry, -1}, {Low.Entry + 0x18, 0}, {Low.Entry + 0x1d, -1}};
    if (Synchronous)
      for (auto &Block : Low.Blocks)
        for (auto &Edge : Block.ExceptionalSuccs)
          Edge.SourceVA = Low.Entry + 0x18;
    auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
    ASSERT_TRUE(verifyMedFunc(Med, "compacted-cxx-frame"));
    EXPECT_LT(Med.Blocks.size(), Low.Blocks.size());
    bool Observed = false;
    for (const auto &Block : Med.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::STORE && Op.Addr == Low.Entry + 0x20) {
          Observed = true;
          EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -100);
          ASSERT_TRUE(Op.Inputs[1].isConst()) << Op.Inputs[1].display();
          EXPECT_EQ(Op.Inputs[1].ConstVal, 0x55u);
        }
    EXPECT_TRUE(Observed);
  }
}

TEST(MedCxxContinuationFrame, SynchronousFrameStillChecksTheThrowingPrefix) {
  for (unsigned Variant = 0; Variant < 3; ++Variant) {
    SCOPED_TRACE(Variant);
    auto Img = makeFixedSEHFrameImage();
    auto &Code = Img.Segments.front().Data;
    size_t CallOffset = 0x10;
    if (Variant == 2) {
      const uint8_t Adjust[] = {0x48, 0x83, 0xec, 8};
      std::copy(std::begin(Adjust), std::end(Adjust), Code.begin() + 0x10);
      CallOffset += sizeof(Adjust);
    }
    Code[CallOffset] = 0xe8;
    writeLE<int32_t>(Code.data() + CallOffset + 1,
                     int32_t(0x100 - CallOffset - 5));
    Code[CallOffset + 5] = 0xeb;
    Code[CallOffset + 6] = uint8_t(0x30 - CallOffset - 7);
    Img.ExceptionMetadata.Functions.front()
        .SEH->Scopes.front()
        .GuardedRange.End = Img.Entry + 0x3d;
    auto Low = makeFixedCxxContinuationFrame(Img);
    Low.ExceptionMetadata->Cxx->IsSynchronous = Variant != 1;
    if (Low.ExceptionMetadata->Cxx->IsSynchronous)
      for (auto &B : Low.Blocks) {
        const auto Call =
            llvm::find_if(B.InstructionBoundaries, [](const auto &I) {
              return I.Control == LowInstructionControl::Call;
            });
        if (Call == B.InstructionBoundaries.end())
          B.ExceptionalSuccs.clear();
        else
          for (auto &Edge : B.ExceptionalSuccs)
            Edge.SourceVA = Call->Address;
      }
    if (Variant == 0) {
      auto Med =
          LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
      EXPECT_TRUE(verifyMedFunc(Med, "synchronous-cxx-frame"));
    } else {
      // Async EH can enter from the restoring epilogue. Sync EH excludes that
      // suffix, but cannot excuse an SP change before a potentially throwing
      // call.
      EXPECT_THROW(
          LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF),
          LowToMedConversionError);
    }
  }
}

TEST(MedCxxContinuationFrame, AnOrdinaryRootCannotBorrowTheCatchFrame) {
  auto Low = makeFixedCxxContinuationFrame();
  Low.CxxContinuationEntries.clear();
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  bool Seen = false;
  for (const auto &B : Med.Blocks)
    for (const auto &Op : B.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.Addr == Low.Entry + 0x20) {
        EXPECT_NE(entrySPOffset(Med, Op.Inputs[0]), -100);
        Seen = true;
      }
  EXPECT_TRUE(Seen);
}

TEST(MedCxxContinuationFrame, RejectsUncertifiedParentFrame) {
  for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Low = makeFixedCxxContinuationFrame();
    auto &EH = *Low.ExceptionMetadata;
    switch (Mutation) {
    case 0:
      EH.Cxx->HasDynamicStackAlignment = true;
      break;
    case 1:
      EH.UnwindOperations.front().StackOffset += 8;
      break;
    case 2:
      Low.CxxContinuationEntries.front().SourceEntry += 1;
      break;
    case 3:
      EH.Cxx->UnwindMap[0].ToState = 0;
      break;
    }
    EXPECT_THROW(
        LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF),
        LowToMedConversionError);
    LowToMedConverter Source;
    Source.setSEHSourceProjectionEnabled(true);
    EXPECT_THROW(Source.convert(Low, Arch::X64, BinaryFormat::COFF),
                 LowToMedConversionError);
  }
}

TEST(MedCxxContinuationFrame, RequiresAuthenticatedSynchronousCallSources) {
  auto Low = makeFixedCxxContinuationFrame();
  Low.ExceptionMetadata->Cxx->IsSynchronous = true;
  // A range-level edge cannot authorize sampling arbitrary register state at
  // a guessed call, even when its try index and handler happen to match.
  EXPECT_THROW(LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF),
               LowToMedConversionError);
}

TEST(HighCxxContinuation, AttachesOnlyCertifiedReturnsToTheirParentEntries) {
  for (unsigned Variant = 0; Variant < 4; ++Variant)
    for (bool ReverseOrder : {false, true}) {
      SCOPED_TRACE(Variant);
      SCOPED_TRACE(ReverseOrder);
      constexpr va_t ParentEntry = 0x140001000;
      constexpr va_t Target = ParentEntry + 0x80;
      constexpr va_t CatchEntry = ParentEntry + 0x100;
      HighFunc Parent, Catch;
      Parent.Entry = ParentEntry;
      Parent.CxxContinuationTargets = {Target, Target + 16};
      Catch.Entry = CatchEntry;
      HighStmt Return;
      Return.Kind = StmtKind::Return;
      Return.RetVal = HighExpr::makeConst(Target, 8);
      if (Variant == 0)
        Return.CxxContinuationReturnTargets = {Target};
      if (Variant == 1)
        Return.CxxContinuationReturnTargets = {Target, Target + 16};
      if (Variant == 3)
        Return.CxxContinuationReturnTargets = {Target, Target + 32};
      Catch.Body.push_back(Return);
      HighStmt Try;
      Try.Kind = StmtKind::CxxTry;
      HighEHClause Clause;
      Clause.Kind = HighEHClauseKind::CxxCatch;
      Clause.HandlerVA = CatchEntry;
      Clause.ContinuationVAs = {Target, Target + 16};
      Try.EHClauses.push_back(Clause);
      Try.EHClauseBodies.resize(1);
      Parent.Body.push_back(Try);
      std::vector<HighFunc> Funcs{Parent, Catch};
      if (ReverseOrder)
        std::reverse(Funcs.begin(), Funcs.end());
      attachCxxFuncletBodies(Funcs);
      const auto &Result = Funcs[ReverseOrder ? 1 : 0];
      const auto &Unattached = Funcs[ReverseOrder ? 0 : 1];
      ASSERT_EQ(Unattached.Body.front().Kind, StmtKind::Return);
      EXPECT_EQ(Unattached.Body.front().CxxContinuationReturnTargets,
                Return.CxxContinuationReturnTargets);
      std::set<va_t> Jumps;
      unsigned Switches = 0;
      walkStmts(Result.Body, [&](const HighStmt &S) {
        if (S.Kind == StmtKind::Goto)
          Jumps.insert(S.GotoTarget);
        Switches += S.Kind == StmtKind::Switch;
      });
      const std::set<va_t> Expected = Variant == 0 ? std::set<va_t>{Target}
                                      : Variant == 1
                                          ? std::set<va_t>{Target, Target + 16}
                                          : std::set<va_t>{};
      EXPECT_EQ(Jumps, Expected);
      EXPECT_EQ(Switches, Variant == 1 ? 1u : 0u);
    }
}

// A catch funclet numbers its names from v0 as its parent does.  Attached to
// the parent's clause, its names move past the parent's, so the two v0 are
// not one C variable, and the funclet's own function keeps its names.
TEST(HighCxxContinuation, AnAttachedFuncletKeepsNamesOfItsOwn) {
  constexpr va_t ParentEntry = 0x140001000;
  constexpr va_t CatchEntry = ParentEntry + 0x100;
  auto Named = [](int Tag) {
    MedVar V;
    V.Kind = MedVar::Temp;
    V.Id = 7;
    V.Size = 8;
    V.RenameTag = static_cast<int16_t>(Tag);
    return HighExpr::makeVar(V);
  };
  auto Assign = [](ExprPtr Dst, ExprPtr Val) {
    HighStmt S;
    S.Kind = StmtKind::Assign;
    S.Dst = std::move(Dst);
    S.Val = std::move(Val);
    return S;
  };
  HighFunc Parent, Catch;
  Parent.Entry = ParentEntry;
  Catch.Entry = CatchEntry;
  Parent.Body.push_back(Assign(Named(0), HighExpr::makeConst(1, 8)));
  HighStmt Try;
  Try.Kind = StmtKind::CxxTry;
  HighEHClause Clause;
  Clause.Kind = HighEHClauseKind::CxxCatch;
  Clause.HandlerVA = CatchEntry;
  Try.EHClauses.push_back(Clause);
  Try.EHClauseBodies.resize(1);
  Parent.Body.push_back(Try);
  const ExprPtr CatchV0 = Named(0);
  Catch.Body.push_back(Assign(CatchV0, HighExpr::makeConst(2, 8)));
  Catch.Body.push_back(Assign(Named(1), CatchV0));
  std::vector<HighFunc> Funcs{Parent, Catch};
  attachCxxFuncletBodies(Funcs);
  const auto &Attached = Funcs[0].Body[1].EHClauseBodies[0];
  ASSERT_EQ(Attached.size(), 2u);
  EXPECT_EQ(Funcs[0].Body[0].Dst->Var.RenameTag, 0);
  EXPECT_EQ(Attached[0].Dst->Var.RenameTag, 1);
  EXPECT_EQ(Attached[1].Dst->Var.RenameTag, 2);
  EXPECT_EQ(Attached[1].Val->Var.RenameTag, 1);
  // A node the body shares stays one node.
  EXPECT_EQ(Attached[0].Dst, Attached[1].Val);
  EXPECT_EQ(Funcs[1].Body[0].Dst->Var.RenameTag, 0);
}

TEST(MedCxxContinuationFrame,
     PublicNestedCatchKeepsOneParameterAndResumeEdges) {
#ifndef NEVERD_BINARY_CORPUS_ROOT
  GTEST_SKIP() << "windows-eh corpus root is not configured";
#else
  const auto Path = std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
                    "corpus/windows-eh/msvc/x86_64/fh3/no-gs/o0/abi-probe/"
                    "cxx_eh_probe-msvc-x86_64-fh3-no-gs-o0.exe";
  if (!std::filesystem::exists(Path))
    GTEST_SKIP() << "public C++ EH corpus fixture is missing";
  constexpr va_t Entry = 0x140001220;
  auto Img = loadBinary(Path);
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries.insert(Entry);
  llvm::LLVMContext Context;
  auto Result = Pipeline().run(*Img, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  auto Main = std::find_if(Result.MedFuncs.begin(), Result.MedFuncs.end(),
                           [&](const MedFunc &F) { return F.Entry == Entry; });
  ASSERT_NE(Main, Result.MedFuncs.end());
  EXPECT_EQ(Main->Params.size(), 1u);
  ASSERT_EQ(Main->CxxContinuationEntries.size(), 3u);
  attachCxxFuncletBodies(Result.HighFuncs);
  auto High = std::find_if(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                           [&](const HighFunc &F) { return F.Entry == Entry; });
  ASSERT_NE(High, Result.HighFuncs.end());
  std::set<va_t> Jumps, Labels;
  walkStmts(High->Body, [&](const HighStmt &S) {
    Labels.insert(S.Addr);
    if (S.Kind == StmtKind::Goto)
      Jumps.insert(S.GotoTarget);
  });
  for (va_t A : {va_t(0x1400012ab), va_t(0x1400012bd), va_t(0x1400012da),
                 va_t(0x140002847)}) {
    EXPECT_TRUE(Jumps.count(A)) << llvm::utohexstr(A);
    EXPECT_TRUE(Labels.count(A)) << llvm::utohexstr(A);
  }
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions COptions;
  COptions.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({*High}, Stream, COptions));
  Stream.flush();
  EXPECT_EQ(Source.find("unknown value"), std::string::npos);
  EXPECT_EQ(Source.find("return 0x14000"), std::string::npos);
  EXPECT_EQ(Source.find("return (uint32_t)(e_1.Value)"), std::string::npos);
#endif
}

TEST(MedCxxContinuationFrame, ClangInteriorStateMarkersKeepThePrologueFrame) {
#ifndef NEVERD_BINARY_CORPUS_ROOT
  GTEST_SKIP() << "windows-eh corpus root is not configured";
#else
  struct Case {
    const char *Directory;
    const char *File;
    va_t Entry;
    va_t Throw;
    size_t Parameters;
  };
  for (const Case &C :
       {Case{"no-gs/o0", "cxx_eh_probe-clang-cl-x86_64-fh3-no-gs-o0.exe",
             0x1400010e0, 0x14000113e, 1},
        Case{"gs/o2", "cxx_eh_probe-clang-cl-x86_64-fh3-gs-o2.exe", 0x140001090,
             0x1400010e4, 0}}) {
    SCOPED_TRACE(C.File);
    const auto Path = std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
                      "corpus/windows-eh/clang-cl/x86_64/fh3" / C.Directory /
                      "abi-probe" / C.File;
    if (!std::filesystem::exists(Path))
      GTEST_SKIP() << "public Clang C++ EH corpus fixture is missing";
    auto Img = loadBinary(Path);
    ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries.insert(C.Entry);
    llvm::LLVMContext Context;
    auto Result = Pipeline().run(*Img, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    const auto Main = llvm::find_if(
        Result.MedFuncs, [&](const auto &F) { return F.Entry == C.Entry; });
    ASSERT_NE(Main, Result.MedFuncs.end());
    EXPECT_EQ(Main->Params.size(), C.Parameters);
    EXPECT_EQ(Main->CxxContinuationEntries.size(), 2u);
    bool FoundCall = false;
    for (const auto &B : Main->Blocks)
      for (const auto &Edge : B.ExceptionalSuccs)
        FoundCall |=
            Edge.Kind == ExceptionalEdgeKind::CxxCatch &&
            Edge.singleContext() && Edge.singleContext()->RegionIndex == 0 &&
            Edge.singleContext()->State == 3 && Edge.SourceVA == C.Throw;
    EXPECT_TRUE(FoundCall);
  }
#endif
}

TEST(MedSEHEstablisherFrame, HandlerSharedWithTheNormalPathKeepsTheFrame) {
  // NtLockVirtualMemory: an empty __except body resumes at the code after
  // its __try, which ordinary flow reaches too, and that join reads the
  // frame.  Ordinary flow brings the certified establisher frame, so both
  // entries address one slot.
  auto Img = makeFixedSEHFrameImage();
  Img.ExceptionMetadata.Functions.front().SEH->Scopes.front().HandlerVA =
      Img.Entry + 0x30;
  Img.ExceptionMetadata.rebuildIndex();
  auto Low = decodeFixedSEHFrame(Img);
  MedFunc Med;
  ASSERT_NO_THROW(
      Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF));
  ASSERT_TRUE(verifyMedFunc(Med, "seh-shared-frame"));
  bool Seen = false;
  for (const MedBlock &B : Med.Blocks)
    for (const MedOp &Op : B.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.Addr == Img.Entry + 0x30) {
        ASSERT_GT(Op.NumInputs, 0u);
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -100);
        Seen = true;
      }
  EXPECT_TRUE(Seen);
}

BinaryImage makeFramePointerSEHFrameImage() {
  BinaryImage Img = makeFixedSEHFrameImage();
  const va_t Entry = Img.Entry;
  Segment &Text = Img.Segments.front();
  Text.Size = 0x39;
  Text.Data.assign(Text.Size, 0xcc);
  // push rbp; sub rsp,64; lea rbp,[rsp+64]; mov dword [rbp-4],1; nop; jmp join.
  const uint8_t Prologue[] = {0x55, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8d,
                              0x6c, 0x24, 0x40, 0xc7, 0x45, 0xfc, 1,
                              0,    0,    0,    0x90, 0xeb, 0x1c};
  std::copy(std::begin(Prologue), std::end(Prologue), Text.Data.begin());
  // Handler: eax=[rbp-4]; eax+=20; [rbp-4]=eax; jmp join.
  const uint8_t Handler[] = {0x8b, 0x45, 0xfc, 0x83, 0xc0, 20,
                             0x89, 0x45, 0xfc, 0xeb, 5};
  std::copy(std::begin(Handler), std::end(Handler), Text.Data.begin() + 0x20);
  // Join: eax=[rbp-4]; add rsp,64; pop rbp; ret.
  const uint8_t Join[] = {0x8b, 0x45, 0xfc, 0x48, 0x83, 0xc4, 0x40, 0x5d, 0xc3};
  std::copy(std::begin(Join), std::end(Join), Text.Data.begin() + 0x30);
  Img.Sections.front().Size = Text.Size;
  Img.Symbols.front() = Symbol::makeFunc(Entry, Text.Size);

  ExceptionFunction &EH = Img.ExceptionMetadata.Functions.front();
  EH.CodeRange = {Entry, Entry + Text.Size};
  EH.PrologueSize = 10;
  EH.FrameRegister = 5;
  EH.FrameOffset = 0x40;
  EH.UnwindOperations.clear();
  UnwindOperation SetFP;
  SetFP.Kind = UnwindOperationKind::SetFramePointer;
  SetFP.CodeOffset = 10;
  EH.UnwindOperations.push_back(SetFP);
  UnwindOperation Alloc;
  Alloc.Kind = UnwindOperationKind::AllocateSmall;
  Alloc.CodeOffset = 5;
  Alloc.StackOffset = 0x40;
  EH.UnwindOperations.push_back(Alloc);
  UnwindOperation Push;
  Push.Kind = UnwindOperationKind::PushNonVolatile;
  Push.CodeOffset = 1;
  Push.Register = 5;
  EH.UnwindOperations.push_back(Push);
  EH.SEH->Scopes.front().GuardedRange = {Entry + 0x11, Entry + 0x12};
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

TEST(MedSEHEstablisherFrame, FramePointerHandlerAndJoinUseTheSameLocalSlot) {
  // clang-cl keeps locals behind `lea rbp,[rsp+N]`.  The unwinder resumes the
  // __except body with the fixed-frame SP and restores RBP as a nonvolatile,
  // so every `[rbp-4]` names one slot, 12 bytes below the entry SP.
  auto Img = makeFramePointerSEHFrameImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-establisher-frame-pointer"));
  std::set<va_t> Seen;
  const std::set<va_t> Expected = {Img.Entry + 0x0a, Img.Entry + 0x20,
                                   Img.Entry + 0x26, Img.Entry + 0x30};
  for (const MedBlock &B : Med.Blocks)
    for (const MedOp &Op : B.Ops)
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Expected.count(Op.Addr)) {
        ASSERT_GT(Op.NumInputs, 0u);
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -12) << Op.Addr;
        Seen.insert(Op.Addr);
      }
  EXPECT_EQ(Seen, Expected);
}

TEST(MedSEHEstablisherFrame, RejectsAFramePointerTheUnwindInfoDoesNotName) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    SCOPED_TRACE(Case);
    auto Img = makeFramePointerSEHFrameImage();
    auto Low = decodeFixedSEHFrame(Img);
    auto &EH = *Low.ExceptionMetadata;
    if (Case == 0)
      EH.FrameOffset = 0x20;
    if (Case == 1)
      EH.UnwindOperations.front().CodeOffset = 5;
    if (Case == 2) {
      // A second frame-register write before the protected range.
      for (auto &Op : Low.Blocks.front().Ops)
        if (Op.Opcode == NdOp::STORE && Op.Addr == Low.Entry + 0x0a) {
          Op.Opcode = NdOp::COPY;
          Op.Output = NdVar::reg(x86reg::RBP, 8);
          Op.NumInputs = 1;
          Op.Inputs[0] =
              NdVar::reg(getTargetRegInfo(Arch::X64).IntReturnReg, 8);
        }
    }
    EXPECT_THROW(
        LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF),
        LowToMedConversionError);
  }
}

TEST(MedSEHEstablisherFrame, AcceptsVersion2EpilogDescriptors) {
  // Version 2 unwind info lists epilog descriptors before the prologue codes.
  // They locate epilogs for the unwinder and do not move the frame.
  auto Img = makeFixedSEHFrameImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto &EH = *Low.ExceptionMetadata;
  EH.Encoding = ExceptionEncoding::X64UnwindV2;
  EH.UnwindVersion = 2;
  UnwindOperation Epilog;
  Epilog.Kind = UnwindOperationKind::Epilog;
  Epilog.CodeOffset = 13;
  Epilog.StackOffset = 13;
  EH.UnwindOperations.insert(EH.UnwindOperations.begin(), Epilog);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-establisher-v2"));
  bool SawHandlerLoad = false;
  for (const MedBlock &B : Med.Blocks)
    for (const MedOp &Op : B.Ops)
      if (Op.Opcode == NdOp::LOAD && Op.Addr == Img.Entry + 0x20) {
        ASSERT_GT(Op.NumInputs, 0u);
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -100);
        SawHandlerLoad = true;
      }
  EXPECT_TRUE(SawHandlerLoad);
}

TEST(MedSEHEstablisherFrame, RejectsEpilogCodesOutsideVersion2) {
  for (unsigned Case = 0; Case != 2; ++Case) {
    SCOPED_TRACE(Case);
    auto Img = makeFixedSEHFrameImage();
    auto Low = decodeFixedSEHFrame(Img);
    auto &EH = *Low.ExceptionMetadata;
    UnwindOperation Extra;
    Extra.CodeOffset = 13;
    if (Case == 0) {
      // Version 1 has no epilog descriptors.
      Extra.Kind = UnwindOperationKind::Epilog;
    } else {
      // A version 2 spare code has no certified SP effect.
      EH.Encoding = ExceptionEncoding::X64UnwindV2;
      EH.UnwindVersion = 2;
      Extra.Kind = UnwindOperationKind::Spare;
    }
    EH.UnwindOperations.insert(EH.UnwindOperations.begin(), Extra);
    EXPECT_THROW(
        LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF),
        LowToMedConversionError);
  }
}

TEST(MedSEHEstablisherFrame, HighCPreservesTheCertifiedHandlerSlot) {
  auto Img = makeFixedSEHFrameImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  auto High = MedToHighConverter().convert(Med, Arch::X64);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream, Options));
  Stream.flush();
  const size_t Handler = Source.find("__except");
  ASSERT_NE(Handler, std::string::npos) << Source;
  EXPECT_NE(Source.find("var_m64 = 1;"), std::string::npos) << Source;
  // The handler adds 20 to the certified slot, as an update or folded into
  // the return that reads it.
  EXPECT_TRUE(
      Source.substr(Handler).find("var_m64 += 20;") != std::string::npos ||
      Source.substr(Handler).find("var_m64 + 0x14") != std::string::npos)
      << Source;
  // The handler returns the slot, directly, through a copy of it, or with
  // the update folded in.
  const std::string AfterHandler = Source.substr(Handler);
  EXPECT_TRUE(AfterHandler.find("return var_m64;") != std::string::npos ||
              AfterHandler.find("var_m64 + 20);") != std::string::npos ||
              AfterHandler.find("return var_m64 + 20;") != std::string::npos ||
              std::regex_search(AfterHandler,
                                std::regex(R"((\w+) = var_m64;\s+return \1;)")))
      << Source;
  EXPECT_EQ(Source.find("var_mEC"), std::string::npos) << Source;
}

// The normal path writes ECX before the guarded nop; the handler stores EAX
// and ECX without writing either.
BinaryImage makeSEHHandlerRegisterImage() {
  BinaryImage Img = makeFixedSEHFrameImage();
  auto &Data = Img.Segments.front().Data;
  // mov ecx,5; nop; nop; nop in place of the slot initializer.
  const uint8_t Normal[] = {0xb9, 5, 0, 0, 0, 0x90, 0x90, 0x90};
  std::copy(std::begin(Normal), std::end(Normal), Data.begin() + 8);
  // Handler: [rsp+36]=eax; [rsp+40]=ecx; nop; nop; nop; jmp join.
  const uint8_t Handler[] = {0x89, 0x44, 0x24, 0x24, 0x89, 0x4c, 0x24,
                             0x28, 0x90, 0x90, 0x90, 0xeb, 3};
  std::copy(std::begin(Handler), std::end(Handler), Data.begin() + 0x20);
  return Img;
}

// Follow value copies and register-view extensions. A runtime frame definition
// has its own occurrence and cannot be replaced by the incoming register.
MedVar sourceValue(const MedFunc &F, MedVar V) {
  for (unsigned Step = 0; Step != 16; ++Step) {
    const MedOp *Def = nullptr;
    for (const MedBlock &B : F.Blocks)
      for (const MedOp &Op : B.Ops)
        if (Op.Output.Kind == V.Kind && Op.Output.Id == V.Id &&
            Op.Output.SSAVer == V.SSAVer)
          Def = &Op;
    if (!Def || Def->NumInputs == 0 ||
        Def->RegistrationRoot != MedOp::RegistrationRootKind::None ||
        (Def->Opcode != NdOp::COPY && Def->Opcode != NdOp::INT_ZEXT))
      return V;
    V = Def->Inputs[0];
  }
  return V;
}

std::optional<MedVar> storedValueAt(const MedFunc &F, va_t Addr) {
  for (const MedBlock &B : F.Blocks)
    for (const MedOp &Op : B.Ops)
      if (Op.Opcode == NdOp::STORE && Op.Addr == Addr && Op.NumInputs > 1)
        return sourceValue(F, Op.Inputs[1]);
  return std::nullopt;
}

// push rbp; sub rsp,64; lea rbp,[rsp+64]; sub rsp,rcx; jmp next;
// next: mov dword [rbp-4],1; mov [rsp+32],ecx (protected); nop; jmp join.
// The handler stores [rbp-4] to [rsp+32] too; the join leaves through
// lea rsp,[rbp].
BinaryImage makeDynamicFramePointerSEHImage() {
  BinaryImage Img = makeFramePointerSEHFrameImage();
  Segment &Text = Img.Segments.front();
  Text.Data.assign(Text.Size, 0xcc);
  const uint8_t Body[] = {0x55, 0x48, 0x83, 0xec, 0x40, 0x48, 0x8d, 0x6c,
                          0x24, 0x40, 0x48, 0x29, 0xcc, 0xeb, 0,    0xc7,
                          0x45, 0xfc, 1,    0,    0,    0,    0x89, 0x4c,
                          0x24, 0x20, 0x90, 0xeb, 0x13};
  std::copy(std::begin(Body), std::end(Body), Text.Data.begin());
  const uint8_t Handler[] = {0x8b, 0x45, 0xfc, 0x89, 0x44, 0x24, 0x20, 0xeb, 7};
  std::copy(std::begin(Handler), std::end(Handler), Text.Data.begin() + 0x20);
  const uint8_t Join[] = {0x8b, 0x45, 0xfc, 0x48, 0x8d, 0x65, 0, 0x5d, 0xc3};
  std::copy(std::begin(Join), std::end(Join), Text.Data.begin() + 0x30);
  Img.ExceptionMetadata.Functions.front().SEH->Scopes.front().GuardedRange = {
      Img.Entry + 0x16, Img.Entry + 0x1a};
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

// The value the `[rsp+disp]` store at Addr adds its displacement to.
std::optional<MedVar> storeAddressBase(const MedFunc &F, va_t Addr) {
  for (const MedBlock &B : F.Blocks)
    for (const MedOp &Op : B.Ops) {
      if (Op.Opcode != NdOp::STORE || Op.Addr != Addr || Op.NumInputs < 2)
        continue;
      const MedVar Address = sourceValue(F, Op.Inputs[0]);
      for (const MedBlock &DefBlock : F.Blocks)
        for (const MedOp &Def : DefBlock.Ops)
          if (Def.Opcode == NdOp::INT_ADD && Def.NumInputs == 2 &&
              Def.Output.Kind == Address.Kind && Def.Output.Id == Address.Id &&
              Def.Output.SSAVer == Address.SSAVer && Def.Inputs[1].isConst())
            return sourceValue(F, Def.Inputs[0]);
      return std::nullopt;
    }
  return std::nullopt;
}

TEST(MedSEHEstablisherFrame, DynamicFramePointerHandlerResumesWithTheRangeSP) {
  // An alloca after a frame-pointer prologue moves SP before the __try.  The
  // unwinder resumes the __except body with the faulting point's SP, which
  // the protected range never moves, and restores RBP as a nonvolatile.
  auto Img = makeDynamicFramePointerSEHImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-dynamic-frame-pointer"));
  std::set<va_t> Seen;
  const std::set<va_t> Expected = {Img.Entry + 0x0f, Img.Entry + 0x20,
                                   Img.Entry + 0x30};
  for (const MedBlock &B : Med.Blocks)
    for (const MedOp &Op : B.Ops)
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Expected.count(Op.Addr)) {
        ASSERT_GT(Op.NumInputs, 0u);
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -12) << Op.Addr;
        Seen.insert(Op.Addr);
      }
  EXPECT_EQ(Seen, Expected);
  auto RangeBase = storeAddressBase(Med, Img.Entry + 0x16);
  auto HandlerBase = storeAddressBase(Med, Img.Entry + 0x23);
  ASSERT_TRUE(RangeBase);
  ASSERT_TRUE(HandlerBase);
  EXPECT_EQ(RangeBase->Kind, MedVar::Reg) << RangeBase->display();
  EXPECT_EQ(HandlerBase->Id, RangeBase->Id) << HandlerBase->display();
  EXPECT_EQ(HandlerBase->SSAVer, RangeBase->SSAVer) << HandlerBase->display();
  // The alloca leaves that SP at no fixed offset from the entry SP.
  EXPECT_FALSE(entrySPOffset(Med, *RangeBase));
}

TEST(MedSEHEstablisherFrame, RejectsADynamicSPWriteInAProtectedBlock) {
  // An alloca in a block the scope covers can leave the faulting point more
  // than one SP.
  auto Img = makeDynamicFramePointerSEHImage();
  auto Low = decodeFixedSEHFrame(Img);
  Low.ExceptionMetadata->SEH->Scopes.front().GuardedRange = {Img.Entry + 0x0a,
                                                             Img.Entry + 0x1a};
  EXPECT_THROW(LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF),
               LowToMedConversionError);
  LowToMedConverter Source;
  Source.setSEHSourceProjectionEnabled(true);
  EXPECT_THROW(Source.convert(Low, Arch::X64, BinaryFormat::COFF),
               LowToMedConversionError);
}

TEST(MedSEHEstablisherFrame,
     SourceHandlerCallRetainsEveryProtectedStackPosition) {
  for (bool SharedHandler : {false, true})
    for (bool ObservesReturnSlot : {false, true}) {
      SCOPED_TRACE(SharedHandler);
      SCOPED_TRACE(ObservesReturnSlot);
      auto Image = makeDynamicFramePointerSEHImage();
      auto &Bytes = Image.Segments.front().Data;
      // Allocate a second area inside the guard, so faults before and after
      // the allocation enter the same handler with different physical SPs.
      const uint8_t Allocate[] = {0x48, 0x83, 0xec, 0x20, 0x90};
      std::copy(std::begin(Allocate), std::end(Allocate), Bytes.begin() + 0x0a);
      // The handler uses RBP, calls a helper, then restores SP in the join.
      const uint8_t Handler[] = {0x8b, 0x45, 0xfc, 0xe8, 0xd8,
                                 0x0f, 0,    0,    0xeb, 6};
      std::copy(std::begin(Handler), std::end(Handler), Bytes.begin() + 0x20);
      Image.ExceptionMetadata.Functions.front()
          .SEH->Scopes.front()
          .GuardedRange = {Image.Entry + 0x0a, Image.Entry + 0x1a};
      if (SharedHandler) {
        // This scope starts after the allocation. Ordinary execution also
        // branches into its handler; it must not become an exact SP seed.
        Image.ExceptionMetadata.Functions.front()
            .SEH->Scopes.front()
            .GuardedRange = {Image.Entry + 0x16, Image.Entry + 0x1a};
        Bytes[0x1c] = 3; // jmp handler instead of jmp join.
      }
      if (ObservesReturnSlot)
        // Read a possible CALL return slot on an exceptional entry.
        Bytes[0x32] = SharedHandler ? 0x98 : 0xb8;
      Image.ExceptionMetadata.rebuildIndex();
      const auto Low = decodeFixedSEHFrame(Image);
      LowToMedConverter Machine;
      Machine.setBinaryImage(&Image);
      EXPECT_THROW(Machine.convert(Low, Arch::X64, BinaryFormat::COFF),
                   LowToMedConversionError);
      LowToMedConverter Source;
      Source.setBinaryImage(&Image);
      Source.setSEHSourceProjectionEnabled(true);
      MedFunc Med;
      ASSERT_NO_THROW(Med = Source.convert(Low, Arch::X64, BinaryFormat::COFF));
      ASSERT_TRUE(verifyMedFunc(Med, "varying-protected-sp"));
      EXPECT_TRUE(Med.HasUnresolvedSEHFrame);
      markSourceStackAccesses(Med, Arch::X64, BinaryFormat::COFF);
      const auto Save =
          llvm::find_if(Med.Blocks.front().Ops, [&](const auto &Op) {
            return Op.Opcode == NdOp::STORE && Op.Addr == Image.Entry;
          });
      ASSERT_NE(Save, Med.Blocks.front().Ops.end());
      EXPECT_TRUE(Save->PrologueSave);
      std::string Error;
      EXPECT_EQ(
          validateSourceCallReturnReads(
              Med, Image, Error, limits::ir::kSourceCallReturnReadWork, &Low),
          !ObservesReturnSlot)
          << Error;
      if (ObservesReturnSlot)
        EXPECT_NE(Error.find("physical CALL return slot"), std::string::npos);
      else {
        // Losing the fresh unwind/frame proof must not authorize the same
        // source reads from an unknown physical stack.
        EXPECT_FALSE(validateSourceCallReturnReads(Med, Image, Error));
        auto Stale = Med;
        for (auto &Block : Stale.Blocks)
          if (!Block.ExceptionalPreds.empty()) {
            auto Edge = Block.ExceptionalPreds.front();
            Edge.BlockId = static_cast<int>(Stale.Blocks.size());
            Block.ExceptionalPreds.push_back(Edge);
          }
        EXPECT_FALSE(validateSourceCallReturnReads(
            Stale, Image, Error, limits::ir::kSourceCallReturnReadWork, &Low));
      }
    }
}

// The fixed frame whose handler starts with fninit. The CFG builder carries
// the x87 state in one function temporary, the context handle, which every
// root begins: the entry and the handler the dispatcher resumes.
BinaryImage makeX87HandlerSEHFrameImage() {
  BinaryImage Img = makeFixedSEHFrameImage();
  // Handler: fninit; eax=[rsp+36]; eax+=20; [rsp+36]=eax; jmp join.
  const uint8_t Handler[] = {0xdb, 0xe3, 0x8b, 0x44, 0x24, 0x24, 0x83, 0xc0,
                             20,   0x89, 0x44, 0x24, 0x24, 0xeb, 1};
  std::copy(std::begin(Handler), std::end(Handler),
            Img.Segments.front().Data.begin() + 0x20);
  return Img;
}

// Decoded with the image's own conventions, as the pipeline decodes it: x64
// PE code owns the x87 context.
LowFunc decodeX87HandlerSEHFrame(const BinaryImage &Img) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Img));
  return CFGBuilder().build(Img, Dec, Img.Entry, "x87_seh_frame");
}

LowBlock &blockAt(LowFunc &Low, va_t Start) {
  const auto Block =
      std::find_if(Low.Blocks.begin(), Low.Blocks.end(),
                   [&](const LowBlock &B) { return B.StartAddr == Start; });
  EXPECT_NE(Block, Low.Blocks.end());
  return *Block;
}

std::string frameProofError(const LowFunc &Low, va_t Handler) {
  size_t Work = limits::kMaxSEHEstablisherFrameWork;
  auto Proof = proveLowSEHEstablisherFrame(
      Low, Handler, getTargetRegInfo(Arch::X64), /*OrdinaryEntry=*/false, Work);
  return Proof ? std::string() : llvm::toString(Proof.takeError());
}

TEST(MedSEHEstablisherFrame, TheX87ContextHandleIsNotAFrameValue) {
  // MinGW's startup code: x87 state travels through its __try.
  auto Img = makeX87HandlerSEHFrameImage();
  auto Low = decodeX87HandlerSEHFrame(Img);
  ASSERT_EQ(Low.FunctionTemporaries.size(), 1u);
  EXPECT_EQ(frameProofError(Low, Img.Entry + 0x20), "");
  MedFunc Med;
  ASSERT_NO_THROW(
      Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF));
  EXPECT_TRUE(verifyMedFunc(Med, "seh-x87-context"));
}

TEST(MedSEHEstablisherFrame, TheHandlerBeginsTheX87ContextBeforeUsingIt) {
  // No ordinary path brings the handle to the handler the dispatcher resumes.
  auto Img = makeX87HandlerSEHFrameImage();
  auto Low = decodeX87HandlerSEHFrame(Img);
  ASSERT_EQ(Low.FunctionTemporaries.size(), 1u);
  LowBlock &Handler = blockAt(Low, Img.Entry + 0x20);
  ASSERT_FALSE(Handler.Ops.empty());
  const LowOp &Begin = Handler.Ops.front();
  ASSERT_EQ(x87ContextHandleOperand(Begin), &Begin.Output);
  Handler.Ops.erase(Handler.Ops.begin());
  ASSERT_FALSE(Handler.InstructionBoundaries.empty());
  --Handler.InstructionBoundaries.front().OpCount;
  for (size_t I = 1; I < Handler.InstructionBoundaries.size(); ++I)
    --Handler.InstructionBoundaries[I].FirstOp;
  EXPECT_NE(frameProofError(Low, Img.Entry + 0x20)
                .find("handler reads a function temporary before binding it"),
            std::string::npos);
}

TEST(MedSEHEstablisherFrame, OnlyTheX87ContextHandleIsAdmitted) {
  // A copy makes the handle an ordinary value the proof does not follow.
  auto Img = makeX87HandlerSEHFrameImage();
  auto Low = decodeX87HandlerSEHFrame(Img);
  ASSERT_EQ(Low.FunctionTemporaries.size(), 1u);
  LowBlock &Entry = Low.Blocks.front();
  ASSERT_FALSE(Entry.Ops.empty());
  ASSERT_FALSE(Entry.InstructionBoundaries.empty());
  LowOp Copy;
  Copy.Opcode = NdOp::COPY;
  Copy.Addr = Entry.Ops.front().Addr;
  Copy.Output = NdVar::reg(x86reg::RAX, 8);
  Copy.addInput(NdVar::tmp(Low.FunctionTemporaries.front().Offset, 8));
  Entry.Ops.insert(Entry.Ops.begin() + 1, Copy);
  ++Entry.InstructionBoundaries.front().OpCount;
  for (size_t I = 1; I < Entry.InstructionBoundaries.size(); ++I)
    ++Entry.InstructionBoundaries[I].FirstOp;
  EXPECT_NE(frameProofError(Low, Img.Entry + 0x20)
                .find("function temporary other than the x87 context handle"),
            std::string::npos);
}

TEST(MedSEHHandlerEntry, EAXHoldsTheExceptionCode) {
  auto Img = makeSEHHandlerRegisterImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-handler-entry"));
  // __C_specific_handler resumes the handler with the code in EAX.
  auto Code = storedValueAt(Med, Img.Entry + 0x20);
  ASSERT_TRUE(Code);
  EXPECT_EQ(Code->Kind, MedVar::SEHExceptionCode) << Code->display();
  EXPECT_EQ(Code->SSAVer, 1);
  EXPECT_EQ(Code->ConstVal, Img.Entry + 0x20);
}

// A second scope guards the nop before the first one and resumes at 0x13,
// before the first handler.
BinaryImage makeTwoSEHHandlerImage() {
  BinaryImage Img = makeSEHHandlerRegisterImage();
  auto &Data = Img.Segments.front().Data;
  // Second handler: [rsp+40]=eax; jmp join.
  const uint8_t Handler[] = {0x89, 0x44, 0x24, 0x28, 0xeb, 0x17};
  std::copy(std::begin(Handler), std::end(Handler), Data.begin() + 0x13);
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Img.Entry + 0xf, Img.Entry + 0x10};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Img.Entry + 0x13;
  Img.ExceptionMetadata.Functions.front().SEH->Scopes.push_back(Scope);
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

TEST(MedSEHHandlerEntry, EachHandlerEntryHasItsOwnExceptionCode) {
  auto Img = makeTwoSEHHandlerImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-two-handlers"));
  auto Early = storedValueAt(Med, Img.Entry + 0x13);
  auto Late = storedValueAt(Med, Img.Entry + 0x20);
  ASSERT_TRUE(Early && Late);
  ASSERT_EQ(Early->Kind, MedVar::SEHExceptionCode) << Early->display();
  ASSERT_EQ(Late->Kind, MedVar::SEHExceptionCode) << Late->display();
  // Numbered in address order and keyed by the entry each handler starts at.
  EXPECT_EQ(Early->SSAVer, 1);
  EXPECT_EQ(Late->SSAVer, 2);
  EXPECT_EQ(Early->ConstVal, Img.Entry + 0x13);
  EXPECT_EQ(Late->ConstVal, Img.Entry + 0x20);
}

TEST(MedSEHHandlerEntry, ScratchRegisterIsNotTheNormalPathValue) {
  auto Img = makeSEHHandlerRegisterImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-handler-entry"));
  // The unwinder leaves ECX unspecified: neither the normal path's
  // `mov ecx,5` nor the value the function was entered with.
  auto Scratch = storedValueAt(Med, Img.Entry + 0x24);
  ASSERT_TRUE(Scratch);
  EXPECT_EQ(Scratch->Kind, MedVar::Unspecified) << Scratch->display();
}

TEST(MedSEHHandlerEntry, NonvolatileRegisterHoldsItsProtectedValue) {
  // mov rdi,rcx before the protected nop; the handler stores rdi. The
  // unwinder restores the protected frame's rdi, which the range never
  // writes: the handler sees rcx's value, not rdi's value at entry.
  auto Img = makeFixedSEHFrameImage();
  auto &Data = Img.Segments.front().Data;
  const uint8_t Normal[] = {0x48, 0x89, 0xcf, 0x90, 0x90, 0x90, 0x90, 0x90};
  std::copy(std::begin(Normal), std::end(Normal), Data.begin() + 8);
  const uint8_t Handler[] = {0x48, 0x89, 0x7c, 0x24, 0x28, 0x90, 0x90,
                             0x90, 0x90, 0x90, 0x90, 0xeb, 3};
  std::copy(std::begin(Handler), std::end(Handler), Data.begin() + 0x20);
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-handler-entry"));
  auto Saved = storedValueAt(Med, Img.Entry + 0x20);
  ASSERT_TRUE(Saved);
  EXPECT_EQ(Saved->Kind, MedVar::Reg) << Saved->display();
  EXPECT_EQ(Saved->RegOff, x86reg::RCX) << Saved->display();
  EXPECT_EQ(Saved->SSAVer, 0) << Saved->display();
}

namespace {

// xor eax,eax; mov [rcx],dl (protected); jmp H; H: ret -- the handler is the
// normal path's return block too, as for `status = 0; __try {..}
// __except (1) { status = GetExceptionCode(); } return status;`.
BinaryImage makeSharedHandlerImage() {
  constexpr va_t Entry = 0x140001000;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = 0x10;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const uint8_t Code[] = {0x31, 0xc0, // xor eax, eax
                          0x88, 0x11, // mov [rcx], dl
                          0xeb, 0x00, // jmp +0
                          0xc3};      // ret
  std::copy(std::begin(Code), std::end(Code), Text.Data.begin());
  Img.Segments.push_back(std::move(Text));
  Section Section;
  Section.Name = ".text";
  Section.VA = Entry;
  Section.Size = 0x10;
  Section.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Section));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, sizeof(Code)));
  ExceptionFunction EH;
  EH.CodeRange = {Entry, Entry + sizeof(Code)};
  EH.Encoding = ExceptionEncoding::X64UnwindV1;
  EH.UnwindVersion = 1;
  EH.UnwindFlags = 1;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.GuardedRange = {Entry + 2, Entry + 6};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = Entry + 6;
  EH.SEH->Scopes.push_back(Scope);
  Img.ExceptionMetadata.Functions.push_back(EH);
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

} // namespace

TEST(MedSEHHandlerEntry, ArgumentLiveIntoTheHandlerStaysAParameter) {
  // mov [rsp+40],rdx before the protected nop; the handler stores rdx too.
  // rdx is then live into both roots, so its entry copy takes a new SSA
  // version, yet it is still the incoming second argument.
  auto Img = makeFixedSEHFrameImage();
  auto &Data = Img.Segments.front().Data;
  const uint8_t Normal[] = {0x48, 0x89, 0x54, 0x24, 0x28, 0x90, 0x90, 0x90};
  std::copy(std::begin(Normal), std::end(Normal), Data.begin() + 8);
  const uint8_t Handler[] = {0x48, 0x89, 0x54, 0x24, 0x30, 0x90, 0x90,
                             0x90, 0x90, 0x90, 0x90, 0xeb, 3};
  std::copy(std::begin(Handler), std::end(Handler), Data.begin() + 0x20);
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-handler-argument"));
  auto Param =
      std::find_if(Med.Params.begin(), Med.Params.end(),
                   [](const MedVar &P) { return P.RegOff == x86reg::RDX; });
  ASSERT_NE(Param, Med.Params.end());
  EXPECT_GE(Param->Id, 0) << "rdx is a placeholder, not the read argument";
}

TEST(MedSEHHandlerEntry, X86RegistrationRestoresOnlyTheProvenFramePointer) {
  // The personality resumes at an except handler with the frame's EBP, and
  // calls a filter and a finally block with it as well.
  const auto &TRI = getTargetRegInfo(Arch::X86);
  for (auto Encoding : {ExceptionEncoding::X86ScopeTableEH3,
                        ExceptionEncoding::X86ScopeTableEH4}) {
    for (auto [WritesFrame, Kind] :
         {std::pair{false, ExceptionalEdgeKind::SEHHandler},
          std::pair{true, ExceptionalEdgeKind::SEHHandler},
          std::pair{false, ExceptionalEdgeKind::SEHFilter},
          std::pair{true, ExceptionalEdgeKind::SEHFilter},
          std::pair{false, ExceptionalEdgeKind::SEHFinally},
          std::pair{true, ExceptionalEdgeKind::SEHFinally}}) {
      SCOPED_TRACE(static_cast<int>(Kind) * 2 + WritesFrame);
      LowFunc Low;
      Low.Entry = 0x1000;
      Low.Name = "x86_registration_frame";
      Low.Blocks.resize(4);
      for (unsigned I = 0; I < 4; ++I) {
        auto &B = Low.Blocks[I];
        B.Id = I;
        B.StartAddr = 0x1000 + I * 0x10;
        B.EndAddr = B.StartAddr + 0x10;
      }
      Low.Blocks[0].Succs = {1};
      Low.Blocks[1].Preds = {0};
      Low.Blocks[1].Succs = {3};
      Low.Blocks[2].Succs = {3};
      Low.Blocks[3].Preds = {1, 2};
      Low.Blocks[1].ExceptionalSuccs.push_back({2, 0x1020, Kind});
      Low.Blocks[2].ExceptionalPreds.push_back({1, 0x1020, Kind});
      ExceptionFunction EH;
      EH.CodeRange = {0x1000, 0x1040};
      EH.Encoding = Encoding;
      EH.Registration.emplace();
      Low.ExceptionMetadata = EH;
      auto Copy = [&](unsigned Block, uint64_t Register, uint64_t Value) {
        LowOp Op;
        Op.Addr = Low.Blocks[Block].StartAddr;
        Op.Opcode = NdOp::COPY;
        Op.Output = NdVar::reg(Register, 4);
        Op.addInput(NdVar::scalar(Value, 4));
        Low.Blocks[Block].Ops.push_back(Op);
      };
      Copy(0, TRI.FramePointer, 0x2000);
      Copy(0, x86reg::RBX, 99);
      if (WritesFrame)
        Copy(1, TRI.FramePointer, 0x3000);
      for (unsigned I = 0; I < 2; ++I) {
        LowOp Store;
        Store.Addr = 0x1020 + I;
        Store.Opcode = NdOp::STORE;
        Store.addInput(NdVar::cst(0x4000 + I * 4, 4));
        Store.addInput(NdVar::reg(I ? x86reg::RBX : TRI.FramePointer, 4));
        Low.Blocks[2].Ops.push_back(Store);
      }
      for (unsigned I = 0; I < 4; ++I) {
        LowOp End;
        End.Addr = Low.Blocks[I].EndAddr - 1;
        End.Opcode = I == 3 ? NdOp::RETURN : NdOp::BRANCH;
        if (I != 3)
          End.addInput(NdVar::cst(I == 0 ? 0x1010 : 0x1030, 4));
        Low.Blocks[I].Ops.push_back(End);
      }
      auto Med =
          LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
      ASSERT_TRUE(verifyMedFunc(Med, "x86-registration-frame"));
      auto Frame = storedValueAt(Med, 0x1020);
      auto Other = storedValueAt(Med, 0x1021);
      ASSERT_TRUE(Frame);
      ASSERT_TRUE(Other);
      if (WritesFrame)
        EXPECT_EQ(Frame->Kind, MedVar::Unspecified) << Frame->display();
      else {
        EXPECT_TRUE(Frame->isConst()) << Frame->display();
        EXPECT_EQ(Frame->ConstVal, 0x2000u);
      }
      EXPECT_EQ(Other->Kind, MedVar::Unspecified) << Other->display();
    }
  }
}

TEST(MedSEHHandlerEntry, IsolatedX86TableEntryKeepsFrameAfterStateExhaustion) {
  const auto &TRI = getTargetRegInfo(Arch::X86);
  for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
    SCOPED_TRACE(Mutation);
    LowFunc Low;
    Low.Entry = 0x1000;
    Low.Name = "isolated_x86_table_entry";
    Low.Blocks.resize(2);
    for (int I = 0; I != 2; ++I) {
      auto &Block = Low.Blocks[I];
      Block.Id = I;
      Block.StartAddr = Low.Entry + 0x20 * I;
      Block.EndAddr = Block.StartAddr + 0x10;
    }
    LowOp PushFrame;
    PushFrame.Opcode = NdOp::INT_SUB;
    PushFrame.Addr = Low.Entry;
    PushFrame.Output = NdVar::reg(TRI.StackPointer, 4);
    PushFrame.addInput(PushFrame.Output);
    PushFrame.addInput(NdVar::scalar(4, 4));
    Low.Blocks[0].Ops.push_back(PushFrame);
    LowOp EstablishFrame;
    EstablishFrame.Opcode = NdOp::COPY;
    EstablishFrame.Addr = Low.Entry + 1;
    EstablishFrame.Output = NdVar::reg(TRI.FramePointer, 4);
    EstablishFrame.addInput(PushFrame.Output);
    Low.Blocks[0].Ops.push_back(EstablishFrame);
    LowOp Store;
    Store.Opcode = NdOp::STORE;
    Store.Addr = 0x1020;
    Store.addInput(NdVar::cst(0x3000, 4));
    Store.addInput(NdVar::reg(TRI.FramePointer, 4));
    Low.Blocks[1].Ops.push_back(Store);
    for (auto &Block : Low.Blocks) {
      LowOp Return;
      Return.Opcode = NdOp::RETURN;
      Return.Addr = Block.EndAddr - 1;
      Block.Ops.push_back(Return);
    }
    auto &EH = Low.ExceptionMetadata.emplace();
    EH.Encoding = ExceptionEncoding::X86ScopeTableEH3;
    EH.Personality = ExceptionPersonality::ExceptHandler3;
    EH.CodeRange = {0x1000, 0x1040};
    auto &Chain = EH.Registration.emplace();
    Chain.RegistrationOffset = -16;
    Chain.TryLevelOffset = -4;
    Chain.ChainInstallVA = 0x1004;
    Chain.Scopes = {{-1, 0x1020, 0x1030, false}};
    auto &States = Low.RegistrationStates.emplace();
    States.Complete = false;
    States.Diagnostics = {"registration-state propagation budget exhausted"};
    switch (Mutation) {
    case 1:
      Low.OrdinaryModuleAnalysisRoots.insert(0x1020);
      break;
    case 2:
      Low.Blocks[0].Succs = {1};
      Low.Blocks[1].Preds = {0};
      break;
    case 3:
      EH.Personality = ExceptionPersonality::ExceptHandler4;
      break;
    case 4:
      Chain.RegistrationOffset = -12;
      break;
    case 5:
      EH.CodeRange.Begin = 0x0ff0;
      break;
    case 6:
      Chain.Scopes.front().FilterVA = 0x1021;
      break;
    case 7:
      Chain.ChainInstallVA = 0x1040;
      break;
    case 8:
      Chain.Scopes.push_back({-1, 0, 0x1020, false});
      break;
    }
    const auto Med =
        LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
    ASSERT_TRUE(verifyMedFunc(Med, "isolated-x86-table-entry"));
    auto Frame = storedValueAt(Med, 0x1020);
    ASSERT_TRUE(Frame);
    unsigned RuntimeDefinitions = 0;
    for (const auto &Block : Med.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.RegistrationRoot ==
            MedOp::RegistrationRootKind::EstablishedFramePointer) {
          ++RuntimeDefinitions;
          EXPECT_EQ(registrationRootEntryStackOffset(Op), -4)
              << Op.Output.display() << " <- " << Op.Inputs[0].display()
              << " origin=" << Op.OriginSeq;
        }
    EXPECT_EQ(RuntimeDefinitions, Mutation == 0 ? 1u : 0u);
    if (!Mutation)
      EXPECT_EQ(entryStackOffset(Med, *Frame, Arch::X86, BinaryFormat::COFF),
                -4)
          << Frame->display() << " kind=" << unsigned(Frame->Kind)
          << " id=" << Frame->Id << " ssa=" << Frame->SSAVer;
  }
}

TEST(MedSEHHandlerEntry, NonvolatileRegisterWrittenInTheRangeIsUnspecified) {
  // mov rdi,rcx inside the protected range; the handler stores rdi. Where
  // the exception struck decides rdi, so the handler cannot know it.
  auto Img = makeFixedSEHFrameImage();
  auto &Data = Img.Segments.front().Data;
  const uint8_t Normal[] = {0x48, 0x89, 0xcf, 0x90, 0x90, 0x90, 0x90, 0x90};
  std::copy(std::begin(Normal), std::end(Normal), Data.begin() + 8);
  const uint8_t Handler[] = {0x48, 0x89, 0x7c, 0x24, 0x28, 0x90, 0x90,
                             0x90, 0x90, 0x90, 0x90, 0xeb, 3};
  std::copy(std::begin(Handler), std::end(Handler), Data.begin() + 0x20);
  Img.ExceptionMetadata.Functions.front().SEH->Scopes.front().GuardedRange = {
      Img.Entry + 8, Img.Entry + 0x11};
  Img.ExceptionMetadata.rebuildIndex();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-handler-entry"));
  auto Saved = storedValueAt(Med, Img.Entry + 0x20);
  ASSERT_TRUE(Saved);
  EXPECT_EQ(Saved->Kind, MedVar::Unspecified) << Saved->display();
}

TEST(MedSEHHandlerEntry, HandlerSharedWithTheNormalPathMergesBothValues) {
  // The normal path returns 0 and the dispatcher's entry the exception code;
  // the handler block is both, so RAX there must merge the two.
  auto Img = makeSharedHandlerImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-shared-handler"));
  bool Merged = false;
  for (const MedBlock &Block : Med.Blocks)
    for (const PhiNode &Phi : Block.Phis)
      Merged |= Block.StartAddr == Img.Entry + 6 && Phi.ExceptionalEntry &&
                Phi.ExceptionalEntry->Kind == MedVar::SEHExceptionCode;
  EXPECT_TRUE(Merged);
  auto High = MedToHighConverter().convert(Med, Arch::X64);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream, Options));
  Stream.flush();
  const size_t Handler = Source.find("__except");
  ASSERT_NE(Handler, std::string::npos) << Source;
  // The normal path keeps its zero; the arm assigns the code.
  EXPECT_NE(Source.substr(0, Handler).find(" = 0;"), std::string::npos)
      << Source;
  EXPECT_NE(Source.substr(Handler).find("exception_code;"), std::string::npos)
      << Source;
  // The handler block follows the __try: the arm simply ends there.
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
}

// MinGW's startup scope ends at a nop the guarded code falls through, and
// resumes at the next nop, which prints nothing: no statement carries the
// handler's address, and the guarded block reaches the handler block both as
// its successor and as its scope's target. The normal path returns 7.
static BinaryImage makeNopResumeSharedHandlerImage() {
  auto Img = makeSharedHandlerImage();
  const uint8_t Code[] = {0xb8, 7,    0, 0, 0, // mov eax, 7
                          0x88, 0x11,          // mov [rcx], dl
                          0x90,                // nop
                          0x90,                // nop
                          0xc3};               // ret
  std::copy(std::begin(Code), std::end(Code),
            Img.Segments.front().Data.begin());
  Img.Symbols.front() = Symbol::makeFunc(Img.Entry, sizeof(Code));
  ExceptionFunction &EH = Img.ExceptionMetadata.Functions.front();
  EH.CodeRange = {Img.Entry, Img.Entry + sizeof(Code)};
  EH.SEH->Scopes.front().GuardedRange = {Img.Entry + 5, Img.Entry + 8};
  EH.SEH->Scopes.front().HandlerVA = Img.Entry + 8;
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

TEST(MedSEHHandlerEntry, AHandlerResumingAtANopEndsTheArmThere) {
  // The arm runs on to the return after the __try instead of jumping to a
  // label that does not exist.
  auto Img = makeNopResumeSharedHandlerImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  ASSERT_TRUE(verifyMedFunc(Med, "seh-shared-handler-nop"));
  auto High = MedToHighConverter().convert(Med, Arch::X64);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream, Options));
  Stream.flush();
  const size_t Handler = Source.find("__except");
  ASSERT_NE(Handler, std::string::npos) << Source;
  EXPECT_NE(Source.substr(Handler).find("exception_code;"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("goto "), std::string::npos) << Source;
}

TEST(MedSEHHandlerEntry, LLVMKeepsTheNormalPathValueOfAHandlerItFallsInto) {
  // The edge into the handler block is both ordinary and exceptional; its
  // ordinary copy into the merged RAX must survive, or the function returns
  // what that PHI's storage started with.
  auto Img = makeNopResumeSharedHandlerImage();
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  // The emitter's own IR: this image has no native SEH lowering, which the
  // optimizer's exception-rewrite contract requires.
  Opts.NoOpt = true;
  Opts.OnlyFunctionEntries.insert(Img.Entry);
  llvm::LLVMContext Context;
  auto Result = Pipeline().run(Img, Context, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  std::vector<uint64_t> Returned;
  for (llvm::Function &Fn : *Result.LlvmModule)
    for (llvm::BasicBlock &Block : Fn)
      if (auto *Ret = llvm::dyn_cast<llvm::ReturnInst>(Block.getTerminator()))
        if (auto *Value = llvm::dyn_cast_or_null<llvm::ConstantInt>(
                Ret->getReturnValue()))
          Returned.push_back(Value->getZExtValue());
  std::string IR;
  llvm::raw_string_ostream Stream(IR);
  Result.LlvmModule->print(Stream, nullptr);
  EXPECT_EQ(Returned, std::vector<uint64_t>{7}) << IR;
}

TEST(MedSEHHandlerEntry, HighCCapturesTheExceptionCodeInTheExceptArm) {
  auto Img = makeSEHHandlerRegisterImage();
  auto Low = decodeFixedSEHFrame(Img);
  auto Med = LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF);
  auto High = MedToHighConverter().convert(Med, Arch::X64);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.TheArch = Arch::X64;
  ASSERT_TRUE(HighCEmitter().emit({High}, Stream, Options));
  Stream.flush();
  const size_t Handler = Source.find("__except");
  ASSERT_NE(Handler, std::string::npos) << Source;
  const std::string Arm = Source.substr(Handler);
  EXPECT_NE(Arm.find("seh_exception_code = GetExceptionCode();"),
            std::string::npos)
      << Source;
  EXPECT_NE(Arm.find("var_m64 = seh_exception_code;"), std::string::npos)
      << Source;
  EXPECT_NE(Source.substr(0, Handler).find(" seh_exception_code;"),
            std::string::npos)
      << Source;
}

TEST(MedSEHEstablisherFrame, RejectsUncertifiedFramesWithoutAborting) {
  for (unsigned Case = 0; Case != 11; ++Case) {
    SCOPED_TRACE(Case);
    auto Img = makeFixedSEHFrameImage();
    auto Low = decodeFixedSEHFrame(Img);
    auto &EH = *Low.ExceptionMetadata;
    if (Case == 0)
      EH.UnwindOperations[0].StackOffset = 64;
    if (Case == 1)
      EH.ParseStatus = ExceptionParseStatus::Partial;
    if (Case == 2)
      EH.FrameRegister = 5;
    if (Case == 3)
      EH.ChainedUnwindInfoRVA = 0x500;
    if (Case == 4)
      EH.SEH->Scopes[0].GuardedRange.Begin = Low.Entry + 1;
    if (Case == 5)
      Low.OrdinaryModuleAnalysisRoots.insert(Low.Entry + 0x20);
    if (Case == 6)
      Low.Blocks.back().Succs.push_back(0);
    if (Case == 7)
      Low.Blocks.front().InstructionBoundaries.clear();
    if (Case == 8) {
      // Dynamic SP update in the ordinary prefix before the protected range.
      for (auto &Op : Low.Blocks.front().Ops)
        if (Op.Opcode == NdOp::STORE && Op.Addr == Low.Entry + 8) {
          Op.Opcode = NdOp::COPY;
          Op.Output = NdVar::reg(getTargetRegInfo(Arch::X64).StackPointer, 8);
          Op.NumInputs = 1;
          Op.Inputs[0] =
              NdVar::reg(getTargetRegInfo(Arch::X64).IntReturnReg, 8);
        }
    }
    if (Case == 9) {
      // Ordinary flow entering the handler from the epilogue, after the
      // frame is gone, does not bring the establisher frame.
      for (auto &B : Low.Blocks)
        if (B.StartAddr == Low.Entry + 0x20)
          Low.Blocks.back().Succs.push_back(B.Id);
    }
    if (Case == 10)
      Low.DecodedInstructionCount = uint64_t(limits::kMaxSSAFunctionOps) + 1;
    EXPECT_THROW(
        LowToMedConverter().convert(Low, Arch::X64, BinaryFormat::COFF),
        LowToMedConversionError);
    LowToMedConverter Source;
    Source.setSEHSourceProjectionEnabled(true);
    EXPECT_THROW(Source.convert(Low, Arch::X64, BinaryFormat::COFF),
                 LowToMedConversionError);
  }
}

TEST(MedSEHEstablisherFrame, PipelineReturnsExplicitFailureForUnknownFrame) {
  auto Img = makeFixedSEHFrameImage();
  Img.ExceptionMetadata.Functions.front().UnwindOperations.front().StackOffset =
      64;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.DumpMed = true;
  Opts.OnlyFunctionEntries.insert(Img.Entry);
  llvm::LLVMContext Context;
  auto Result = Pipeline().run(Img, Context, Opts);
  EXPECT_FALSE(Result.Success);
  EXPECT_TRUE(Result.MedFuncs.empty());
  EXPECT_TRUE(Result.HighFuncs.empty());
  EXPECT_NE(Result.Error.find("Windows SEH establisher frame"),
            std::string::npos);
  // The same session/process can still perform a supported conversion.
  auto Valid = makeFixedSEHFrameImage();
  Result = Pipeline().run(Valid, Context, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
}

TEST(MedSEHEstablisherFrame, RejectsConvertedCalleePopEffects) {
  for (bool CallInPrologue : {false, true}) {
    SCOPED_TRACE(CallInPrologue);
    auto Img = makeFixedSEHFrameImage();
    auto &Bytes = Img.Segments.front().Data;
    // CALL entry+0x1000 at +0x10; its ret 8 contract adjusts SP only in
    // converted MedIR. Exercise both a protected prefix and the prologue.
    const uint8_t CallAndJump[] = {0xe8, 0xeb, 0x0f, 0, 0, 0x90, 0xeb, 0x18};
    std::copy(std::begin(CallAndJump), std::end(CallAndJump),
              Bytes.begin() + 0x10);
    auto &EH = Img.ExceptionMetadata.Functions.front();
    EH.SEH->Scopes.front().GuardedRange = {Img.Entry + 0x15, Img.Entry + 0x16};
    if (CallInPrologue)
      EH.PrologueSize = 0x15;
    auto Low = decodeFixedSEHFrame(Img);
    std::map<va_t, int> Pop{{Img.Entry + 0x1000, 8}};
    LowToMedConverter Converter;
    Converter.setCalleePopMap(&Pop);
    EXPECT_THROW(Converter.convert(Low, Arch::X64, BinaryFormat::COFF),
                 LowToMedConversionError);
  }
}

TEST(MedSEHEstablisherFrame, PublicNestedSEHUsesOneSlotInMedAndHighC) {
#ifndef NEVERD_BINARY_CORPUS_ROOT
  GTEST_SKIP() << "windows-eh corpus root is not configured";
#else
  const auto Path = std::filesystem::path(NEVERD_BINARY_CORPUS_ROOT) /
                    "corpus/windows-eh/msvc/x86_64/fh4/no-gs/o0/abi-probe/"
                    "seh_probe-msvc-x86_64-fh4-no-gs-o0.exe";
  if (!std::filesystem::exists(Path))
    GTEST_SKIP() << "public SEH corpus fixture is missing";
  constexpr va_t Entry = 0x1400010b0;
  BinaryLoadOptions LoadOptions;
  LoadOptions.OnlyFunctionEntries.insert(Entry);
  auto Img = loadBinary(Path, LoadOptions);
  ASSERT_TRUE(bool(Img)) << llvm::toString(Img.takeError());
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.OnlyFunctionEntries.insert(Entry);
  llvm::LLVMContext Context;
  auto Result = Pipeline().run(*Img, Context, Options);
  ASSERT_TRUE(Result.Success) << Result.Error;
  auto Main = std::find_if(Result.MedFuncs.begin(), Result.MedFuncs.end(),
                           [&](const MedFunc &F) { return F.Entry == Entry; });
  ASSERT_NE(Main, Result.MedFuncs.end());
  const MedFunc &Med = *Main;
  const std::set<va_t> Expected = {Entry + 0x1d, Entry + 0x81, Entry + 0x88,
                                   Entry + 0x8c};
  std::set<va_t> Seen;
  for (const MedBlock &B : Med.Blocks)
    for (const MedOp &Op : B.Ops)
      if ((Op.Opcode == NdOp::LOAD || Op.Opcode == NdOp::STORE) &&
          Expected.count(Op.Addr)) {
        EXPECT_EQ(entrySPOffset(Med, Op.Inputs[0]), -100) << Op.Addr;
        Seen.insert(Op.Addr);
      }
  EXPECT_EQ(Seen, Expected);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions COptions;
  COptions.TheArch = Arch::X64;
  auto MainHigh =
      std::find_if(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                   [&](const HighFunc &F) { return F.Entry == Entry; });
  ASSERT_NE(MainHigh, Result.HighFuncs.end());
  ASSERT_TRUE(HighCEmitter().emit({*MainHigh}, Stream, COptions));
  Stream.flush();
  // The handler reaches the -100 slot as the frame record's member, never
  // through another frame displacement.
  const size_t Handler = Source.find("__except");
  ASSERT_NE(Handler, std::string::npos) << Source;
  EXPECT_NE(Source.substr(Handler).find("stack_storage.var_m64"),
            std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("frame_base"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("var_mEC"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("var_m110"), std::string::npos) << Source;
#endif
}

} // namespace

namespace {

// sub rsp,0x38; mov rax,[rsp+0x60]; mov [rsp+0x20],rax; call callee;
// add rsp,0x38; ret -- RCX, RDX, R8 and R9 are passed through untouched.
BinaryImage makeWin64ForwarderImage() {
  constexpr va_t Entry = 0x140001000;
  constexpr va_t Callee = 0x140001020;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = 0x40;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const int32_t Rel = static_cast<int32_t>(Callee - (Entry + 0x13 + 5));
  const uint8_t Code[] = {0x48,
                          0x83,
                          0xec,
                          0x38, // sub
                          0x48,
                          0x8b,
                          0x44,
                          0x24,
                          0x60, // mov rax
                          0x48,
                          0x89,
                          0x44,
                          0x24,
                          0x20, // mov [rsp]
                          0x90,
                          0x90,
                          0x90,
                          0x90,
                          0x90, // pad
                          0xe8,
                          static_cast<uint8_t>(Rel), // call
                          static_cast<uint8_t>(Rel >> 8),
                          static_cast<uint8_t>(Rel >> 16),
                          static_cast<uint8_t>(Rel >> 24),
                          0x48,
                          0x83,
                          0xc4,
                          0x38,  // add
                          0xc3}; // ret
  std::copy(std::begin(Code), std::end(Code), Text.Data.begin());
  // All five positions are live: the actual callee consumes the register
  // arguments as well as its fifth stack argument. An unused register slot
  // alone is not evidence that the forwarder has a parameter in that slot.
  const uint8_t CalleeCode[] = {0x48, 0x8b, 0x44,
                                0x24, 0x28,       // mov rax,[rsp+28h]
                                0x48, 0x01, 0xc8, // add rax,rcx
                                0x48, 0x01, 0xd0, // add rax,rdx
                                0x4c, 0x01, 0xc0, // add rax,r8
                                0x4c, 0x01, 0xc8, // add rax,r9
                                0xc3};
  std::copy(std::begin(CalleeCode), std::end(CalleeCode),
            Text.Data.begin() + (Callee - Entry));
  Img.Segments.push_back(std::move(Text));
  Section Section;
  Section.Name = ".text";
  Section.VA = Entry;
  Section.Size = 0x40;
  Section.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Section));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, 0x20));
  Img.Symbols.push_back(Symbol::makeFunc(Callee, sizeof(CalleeCode)));
  return Img;
}

} // namespace

TEST(Win64Forwarder, UntouchedRegisterArgumentsPassTheParameters) {
  auto Img = makeWin64ForwarderImage();
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  const HighFunc *Forwarder = nullptr;
  for (const HighFunc &Func : Result.HighFuncs)
    if (Func.Entry == Img.Entry)
      Forwarder = &Func;
  ASSERT_NE(Forwarder, nullptr);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({*Forwarder}, Stream, Options));
  Stream.flush();
  // The callee consumes all five slots; the untouched register arguments
  // carry the forwarder's own parameters.
  EXPECT_NE(Source.find("(arg0, arg1, arg2, arg3, arg4)"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("(0, 0, 0, 0,"), std::string::npos) << Source;
}

namespace {

// xor r11d,r11d; lea r8,[rsp+8]; test rdx,rdx; mov [rsp+8],r11w;
// cmovne r8,rdx; mov word [r8],0x14; movzx eax,word [r8]; ret
BinaryImage makeMaskedFrameSelectImage() {
  constexpr va_t Entry = 0x140001000;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = 0x30;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const uint8_t Code[] = {0x45, 0x31, 0xdb,                   // xor
                          0x4c, 0x8d, 0x44, 0x24, 0x08,       // lea
                          0x48, 0x85, 0xd2,                   // test
                          0x66, 0x44, 0x89, 0x5c, 0x24, 0x08, // mov
                          0x4c, 0x0f, 0x45, 0xc2,             // cmovne
                          0x66, 0x41, 0xc7, 0x00, 0x14, 0x00, // mov
                          0x41, 0x0f, 0xb7, 0x00,             // movzx
                          0xc3};                              // ret
  std::copy(std::begin(Code), std::end(Code), Text.Data.begin());
  Img.Segments.push_back(std::move(Text));
  Section Section;
  Section.Name = ".text";
  Section.VA = Entry;
  Section.Size = 0x30;
  Section.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Section));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, sizeof(Code)));
  return Img;
}

} // namespace

TEST(Win64Forwarder, MaskedSelectOfAFrameAddressUsesItsIntegerValue) {
  // `cmovne` over a frame address lifts to `(p & m) | (&slot & ~m)`; C has
  // no bitwise operator on a pointer, so the address is an integer there.
  auto Img = makeMaskedFrameSelectImage();
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_FALSE(Result.HighFuncs.empty());
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({Result.HighFuncs.front()}, Stream, Options));
  Stream.flush();
  for (size_t At = Source.find("&var_"); At != std::string::npos;
       At = Source.find("&var_", At + 1)) {
    const size_t End = Source.find_first_of(" ),;", At);
    if (End == std::string::npos || Source.compare(End, 3, " & ") != 0)
      continue;
    EXPECT_EQ(Source.compare(At - 11, 11, "(uintptr_t)"), 0) << Source;
  }
  EXPECT_NE(Source.find("(uintptr_t)&var_"), std::string::npos) << Source;
}

namespace {

// caller: sub rsp,28h; call clobber; mov edx,1; call reads_dl; add rsp,28h;
// ret. clobber: xor ecx,ecx; ret. reads_dl: movzx eax,dl; ret.
BinaryImage makeUnreadSlotImage() {
  constexpr va_t Entry = 0x140001000;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Img.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = 0x40;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0xcc);
  const uint8_t Caller[] = {0x48, 0x83, 0xec, 0x28,       // sub rsp, 28h
                            0xe8, 0x17, 0x00, 0x00, 0x00, // call clobber
                            0xba, 0x01, 0x00, 0x00, 0x00, // mov edx, 1
                            0xe8, 0x1d, 0x00, 0x00, 0x00, // call reads_dl
                            0x48, 0x83, 0xc4, 0x28,       // add rsp, 28h
                            0xc3};                        // ret
  const uint8_t Clobber[] = {0x31, 0xc9, 0xc3};
  const uint8_t ReadsDl[] = {0x0f, 0xb6, 0xc2, 0xc3};
  std::copy(std::begin(Caller), std::end(Caller), Text.Data.begin());
  std::copy(std::begin(Clobber), std::end(Clobber), Text.Data.begin() + 0x20);
  std::copy(std::begin(ReadsDl), std::end(ReadsDl), Text.Data.begin() + 0x30);
  Img.Segments.push_back(std::move(Text));
  Section Section;
  Section.Name = ".text";
  Section.VA = Entry;
  Section.Size = 0x40;
  Section.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Section));
  Img.Symbols.push_back(Symbol::makeFunc(Entry, sizeof(Caller)));
  Img.Symbols.push_back(Symbol::makeFunc(Entry + 0x20, sizeof(Clobber)));
  Img.Symbols.push_back(Symbol::makeFunc(Entry + 0x30, sizeof(ReadsDl)));
  return Img;
}

} // namespace

TEST(Win64Forwarder, ArgumentSlotTheCalleeNeverReadsIsZero) {
  // The callee reads only DL, so RCX is a positional slot it ignores. The
  // caller left nothing defined there; the call passes zero, not an
  // unknown value.
  auto Img = makeUnreadSlotImage();
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  const HighFunc *Caller = nullptr;
  for (const HighFunc &Func : Result.HighFuncs)
    if (Func.Entry == Img.Entry)
      Caller = &Func;
  ASSERT_NE(Caller, nullptr);
  std::string Source;
  llvm::raw_string_ostream Stream(Source);
  CEmitterOptions Options;
  Options.EmitIncludes = false;
  Options.TheArch = Arch::X64;
  Options.Format = BinaryFormat::COFF;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit({*Caller}, Stream, Options));
  Stream.flush();
  EXPECT_NE(Source.find("sub_140001030(0, 1)"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
}
