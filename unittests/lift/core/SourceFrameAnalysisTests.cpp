#include "gtest/gtest.h"

#include "neverd/ir/Limits.h"
#include "neverd/ir/SourceABI.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/ir/low/ImportCallee.h"
#include "neverd/ir/low/SourceFrameAnalysis.h"
#include "neverd/ir/low/SourceFrameBytes.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>

using namespace neverd;
namespace {
const auto SP = NdVar::reg(a64reg::SP, 8);
const auto Slot = NdVar::reg(a64reg::X19, 8);
const auto Value = NdVar::reg(a64reg::X21, 8);
const auto X0 = NdVar::reg(a64reg::X0, 8);

LowOp op(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs,
         va_t Address) {
  LowOp Result;
  Result.Opcode = Opcode;
  Result.Output = Output;
  Result.Addr = Address;
  for (const auto &Input : Inputs)
    Result.addInput(Input);
  return Result;
}
struct FrameFixture {
  LowFunc Low;
  FrameFixture() {
    Low.Entry = 0x1000;
    LowBlock Block;
    Block.Id = 7;
    Block.StartAddr = Low.Entry;
    Block.Ops = {
        op(NdOp::INT_SUB, SP, {SP, NdVar::scalar(64, 8)}, 0x1000),
        op(NdOp::COPY, Value, {NdVar::addressFragment(0x6000, 8)}, 0x1004),
        op(NdOp::INT_ADD, Value, {Value, NdVar::scalar(128, 8)}, 0x1008),
        op(NdOp::INT_ADD, Slot, {SP, NdVar::scalar(32, 8)}, 0x100c),
        op(NdOp::STORE, {}, {Slot, Value}, 0x1010),
        op(NdOp::COPY, Value, {NdVar::scalar(0, 8)}, 0x1014),
        op(NdOp::LOAD, NdVar::tmp(16, 8), {Slot}, 0x1018),
        // Later unknown cleanup does not authorize this earlier query.
        op(NdOp::CALL, X0, {NdVar::cst(0x5000, 8)}, 0x101c),
        op(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)}, 0x1020)};
    Low.Blocks.push_back(std::move(Block));
  }
  std::optional<SourceFrameLoadDefinition>
  query(const NativeSourceCalls &Calls = {}) const {
    for (const auto &Block : Low.Blocks)
      for (size_t I = 0; I < Block.Ops.size(); ++I)
        if (Block.Ops[I].Addr == 0x1018 && Block.Ops[I].Opcode == NdOp::LOAD)
          return sourceFrameLoadedDefinition(Low, Arch::AArch64, Calls,
                                             Block.Id, I);
    return std::nullopt;
  }
  void prefix(std::initializer_list<LowOp> Ops) {
    auto &B = Low.Blocks.front();
    B.Ops.insert(B.Ops.begin() + 6, Ops);
  }
  void diamond() {
    const auto Original = Low.Blocks.front();
    Low.Blocks.resize(4);
    for (size_t I = 0; I < 4; ++I) {
      Low.Blocks[I] = {};
      Low.Blocks[I].Id = 7 + I;
      Low.Blocks[I].StartAddr = 0x1000 + I * 0x100;
    }
    Low.Blocks[0].Ops.assign(Original.Ops.begin(), Original.Ops.begin() + 6);
    Low.Blocks[0].Succs = {8, 9};
    for (size_t I : {1, 2}) {
      Low.Blocks[I].Preds = {7};
      Low.Blocks[I].Succs = {10};
      Low.Blocks[I].Ops = {op(NdOp::COPY, NdVar::reg(a64reg::X9, 8),
                              {NdVar::scalar(0, 8)}, 0x1000 + I * 0x100)};
    }
    Low.Blocks[3].Preds = {8, 9};
    Low.Blocks[3].Ops.assign(Original.Ops.begin() + 6, Original.Ops.end());
  }
};

SourceFunctionTypeHint signature(size_t Parameters = 1,
                                 bool ReturnsPointer = false) {
  SourceFunctionTypeHint Result;
  Result.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
  Result.ReturnType =
      ReturnsPointer ? NdType::makePtr(NdType::makeVoid()) : NdType::makeVoid();
  for (size_t I = 0; I < Parameters; ++I)
    Result.Parameters.push_back({"arg", NdType::makeInt(8, false)});
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Result, Arch::AArch64, Error))
      << Error;
  return Result;
}
NativeSourceCalls callsAt(va_t Address, const SourceFunctionTypeHint &Signature,
                          const SourceFrameEffects &Effects) {
  NativeSourceCallContract Contract;
  Contract.Signature = &Signature;
  static_cast<SourceFrameEffects &>(Contract) = Effects;
  const auto Call = op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, Address);
  return {{*nativeSourceCallKey(Call), Contract}};
}
} // namespace

TEST(SourceFrameAnalysis, CallStorageKeepsDefinitionsAndPaddingDistinct) {
  FrameFixture F;
  auto &Ops = F.Low.Blocks.front().Ops;
  Ops.insert(Ops.begin() + 7, op(NdOp::COPY, X0, {Slot}, 0x101a));
  auto ABI = signature();
  ABI.Parameters[0].Type = NdType::makePtr(NdType::makeVoid());
  const auto Site = *nativeSourceCallKey(Ops[8]);
  NativeSourceCallContract Contract;
  Contract.Signature = &ABI;
  const NativeSourceCalls Calls{{Site, Contract}};
  const auto Storage =
      sourceFrameCallArgumentStorage(F.Low, Arch::AArch64, Calls, Site, 0, 16);
  ASSERT_TRUE(Storage);
  EXPECT_EQ(Storage->FrameOffset, -32);
  ASSERT_EQ(Storage->Bytes.size(), 16U);
  for (unsigned I = 0; I < 8; ++I) {
    EXPECT_TRUE(Storage->Bytes[I].Initialized);
    ASSERT_TRUE(Storage->Bytes[I].Definition);
    EXPECT_EQ(Storage->Bytes[I].Definition->Instruction, 0x1008U);
    EXPECT_EQ(Storage->Bytes[I].DefinitionByte, I);
    EXPECT_FALSE(Storage->Bytes[I].Constant);
    EXPECT_FALSE(Storage->Bytes[8 + I].Initialized);
    EXPECT_FALSE(Storage->Bytes[8 + I].Definition);
    EXPECT_FALSE(Storage->Bytes[8 + I].Constant);
  }
  // An observation supplies neither this consumer's borrow permission nor
  // the enclosing function's restoration/source certificate.
  EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, Calls));
}

TEST(SourceFrameAnalysis, CallStorageKeepsEveryByteOfAVectorDefinition) {
  FrameFixture F;
  auto &Ops = F.Low.Blocks.front().Ops;
  const auto Vector = NdVar::reg(a64reg::V0, 16);
  Ops[1] = op(NdOp::LOAD, Vector, {NdVar::addressFragment(0x6000, 8)}, 0x1004);
  Ops[2] = op(NdOp::COPY, Vector, {Vector}, 0x1008);
  Ops[4].Inputs[1] = Vector;
  // A low eight-byte slice is not a complete pointer-producing definition.
  EXPECT_FALSE(F.query());
  Ops.insert(Ops.begin() + 7, op(NdOp::COPY, X0, {Slot}, 0x101a));
  auto ABI = signature();
  ABI.Parameters[0].Type = NdType::makePtr(NdType::makeVoid());
  const auto Site = *nativeSourceCallKey(Ops[8]);
  NativeSourceCallContract Contract;
  Contract.Signature = &ABI;
  const auto Storage = sourceFrameCallArgumentStorage(
      F.Low, Arch::AArch64, {{Site, Contract}}, Site, 0, 16);
  ASSERT_TRUE(Storage);
  ASSERT_EQ(Storage->Bytes.size(), 16U);
  for (unsigned I = 0; I < 16; ++I) {
    const auto &Byte = Storage->Bytes[I];
    EXPECT_TRUE(Byte.Initialized);
    ASSERT_TRUE(Byte.Definition);
    EXPECT_EQ(Byte.Definition->Instruction, 0x1004U);
    EXPECT_EQ(Byte.Definition->Output, Vector);
    EXPECT_EQ(Byte.DefinitionByte, I);
    EXPECT_FALSE(Byte.Constant);
  }
}

namespace {
struct StorageObservationFixture {
  LowFunc Function;
  NdVar Stack, Pointer, Tail, External, Vector;

  explicit StorageObservationFixture(Arch Architecture) {
    const auto &TRI = getTargetRegInfo(Architecture);
    Stack = NdVar::reg(TRI.StackPointer, 8);
    Pointer =
        NdVar::reg(Architecture == Arch::X64 ? x86reg::RCX : a64reg::X0, 8);
    Tail = NdVar::reg(Architecture == Arch::X64 ? x86reg::R10 : a64reg::X8, 8);
    External =
        NdVar::reg(Architecture == Arch::X64 ? x86reg::RDX : a64reg::X1, 8);
    Vector = NdVar::reg(TRI.FPReturnReg, 16);
    Function.Entry = 0x1000;
    LowBlock Block;
    Block.Id = 7;
    Block.StartAddr = Function.Entry;
    Block.Ops = {
        op(NdOp::INT_SUB, Stack, {Stack, NdVar::scalar(64, 8)}, 0x1000),
        op(NdOp::COPY, Pointer, {Stack}, 0x1004),
        op(NdOp::LOAD, Vector, {NdVar::addressFragment(0x9000, 8)}, 0x1008),
        op(NdOp::STORE, {}, {Pointer, Vector}, 0x100c),
        op(NdOp::INT_ADD, Tail, {Stack, NdVar::scalar(16, 8)}, 0x1010),
        op(NdOp::STORE, {}, {Tail, NdVar::scalar(0, 2)}, 0x1014),
        op(NdOp::CALL, {}, {NdVar::addressFragment(0x5000, 8)}, 0x1018),
        op(NdOp::RETURN, {}, {}, 0x101c)};
    Function.Blocks.push_back(std::move(Block));
  }

  auto query(Arch Architecture, BinaryFormat Format,
             size_t *Remaining = nullptr) const {
    const auto &Block = Function.Blocks.back();
    return sourceFrameStorageBefore(
        Function, Architecture, Format, Block.Id, Block.Ops.size() - 2,
        {Pointer, External, Pointer}, 64, Remaining);
  }
};
} // namespace

TEST(SourceFrameAnalysis, ObservesInitializedPrefixesBeforeCallsAcrossTargets) {
  for (const auto Architecture : {Arch::X64, Arch::AArch64}) {
    for (const auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      SCOPED_TRACE(int(Architecture));
      SCOPED_TRACE(int(Format));
      StorageObservationFixture F(Architecture);
      const auto Observed = F.query(Architecture, Format);
      ASSERT_EQ(Observed.size(), 1U);
      EXPECT_EQ(Observed[0].FrameOffset, -64);
      ASSERT_EQ(Observed[0].Bytes.size(), 18U);
      for (unsigned I = 0; I < 16; ++I) {
        const auto &Byte = Observed[0].Bytes[I];
        ASSERT_TRUE(Byte.Definition);
        EXPECT_EQ(Byte.Definition->Instruction, 0x1008U);
        EXPECT_EQ(Byte.Definition->Output, F.Vector);
        EXPECT_EQ(Byte.DefinitionByte, I);
        EXPECT_FALSE(Byte.Constant);
      }
      EXPECT_EQ(Observed[0].Bytes[16].Constant, 0);
      EXPECT_EQ(Observed[0].Bytes[17].Constant, 0);
      // The query observes the prefix; it does not authorize the call.
      EXPECT_FALSE(restoresNativeSourceState(F.Function, Architecture, {}));
    }
  }
}

TEST(SourceFrameAnalysis, StorageObservationsRejectUnprovenMemoryAndLifetimes) {
  for (const auto Architecture : {Arch::X64, Arch::AArch64}) {
    for (unsigned Mutation = 0; Mutation < 5; ++Mutation) {
      SCOPED_TRACE(int(Architecture));
      SCOPED_TRACE(Mutation);
      StorageObservationFixture F(Architecture);
      auto &Ops = F.Function.Blocks.front().Ops;
      switch (Mutation) {
      case 0:
        Ops.insert(
            Ops.begin() + 6,
            op(NdOp::STORE, {}, {F.External, NdVar::scalar(0, 8)}, 0x1016));
        break;
      case 1:
        Ops.insert(Ops.begin() + 6,
                   op(NdOp::STORE, {}, {F.External, F.Pointer}, 0x1016));
        break;
      case 2:
        Ops.insert(Ops.begin() + 6,
                   op(NdOp::CALL, {}, {NdVar::cst(0x7000, 8)}, 0x1016));
        break;
      case 3:
        Ops.insert(Ops.begin() + 6,
                   op(NdOp::INT_ADD, F.Stack, {F.Stack, NdVar::scalar(64, 8)},
                      0x1016));
        break;
      case 4:
        F.Function.Blocks.front().Succs = {7};
        F.Function.Blocks.front().Preds = {7};
        break;
      }
      EXPECT_TRUE(F.query(Architecture, BinaryFormat::COFF).empty());
    }
  }
}

TEST(SourceFrameAnalysis, StorageObservationsKeepUnknownBytesAndShareBudget) {
  for (const auto Architecture : {Arch::X64, Arch::AArch64}) {
    StorageObservationFixture F(Architecture);
    auto &Ops = F.Function.Blocks.front().Ops;
    auto Unknown = F.External;
    Unknown.Size = 1;
    Ops.insert(Ops.begin() + 6,
               op(NdOp::STORE, {}, {F.Pointer, Unknown}, 0x1016));
    const auto Observed = F.query(Architecture, BinaryFormat::COFF);
    ASSERT_EQ(Observed.size(), 1U);
    ASSERT_EQ(Observed[0].Bytes.size(), 18U);
    EXPECT_TRUE(Observed[0].Bytes[0].Initialized);
    EXPECT_FALSE(Observed[0].Bytes[0].Constant);
    EXPECT_FALSE(Observed[0].Bytes[0].Definition);
    EXPECT_TRUE(Observed[0].Bytes[1].Definition);
    size_t Remaining = 1;
    EXPECT_TRUE(F.query(Architecture, BinaryFormat::COFF, &Remaining).empty());
    EXPECT_LE(Remaining, 1U);
    const auto Left = Remaining;
    EXPECT_TRUE(F.query(Architecture, BinaryFormat::COFF, &Remaining).empty());
    EXPECT_LE(Remaining, Left);
    EXPECT_TRUE(F.query(Architecture, BinaryFormat::Unknown).empty());
  }
}

TEST(SourceFrameAnalysis, StorageObservationsMeetEveryPathInEitherOrder) {
  for (const auto Architecture : {Arch::X64, Arch::AArch64}) {
    for (bool Reverse : {false, true}) {
      for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
        SCOPED_TRACE(int(Architecture));
        SCOPED_TRACE(Reverse);
        SCOPED_TRACE(Mutation);
        StorageObservationFixture F(Architecture);
        const auto Original = F.Function.Blocks.front();
        F.Function.Blocks.resize(4);
        for (unsigned I = 0; I != 4; ++I) {
          auto &Block = F.Function.Blocks[I];
          Block = {};
          Block.Id = 7 + I;
          Block.StartAddr = 0x1000 + 0x100 * I;
        }
        auto &Entry = F.Function.Blocks[0];
        Entry.Ops.assign(Original.Ops.begin(), Original.Ops.begin() + 6);
        Entry.Succs = {8, 9};
        if (Mutation == 3 || Mutation == 4)
          Entry.Ops.erase(Entry.Ops.begin() + 3);
        for (unsigned I : {1U, 2U}) {
          auto &Block = F.Function.Blocks[I];
          Block.Preds = {7};
          Block.Succs = {10};
          Block.Ops = {op(NdOp::COPY, F.External, {NdVar::scalar(0, 8)},
                          Block.StartAddr)};
          if ((Mutation == 1 && I == 1) || Mutation == 2)
            Block.Ops = {op(NdOp::STORE, {}, {F.Pointer, NdVar::scalar('X', 1)},
                            Block.StartAddr)};
          if ((Mutation == 3 && I == 1) || Mutation == 4)
            Block.Ops = {
                op(NdOp::STORE, {}, {F.Pointer, F.Vector}, Block.StartAddr)};
          if (Mutation == 5 && I == 1)
            Block.Ops = {
                op(NdOp::CALL, {}, {NdVar::cst(0x7000, 8)}, Block.StartAddr)};
        }
        auto &Call = F.Function.Blocks[3];
        Call.Preds = {8, 9};
        Call.Ops.assign(Original.Ops.begin() + 6, Original.Ops.end());
        Call.Ops[0].Addr = Call.StartAddr;
        Call.Ops[1].Addr = Call.StartAddr + 4;
        if (Reverse) {
          std::reverse(Entry.Succs.begin(), Entry.Succs.end());
          std::reverse(Call.Preds.begin(), Call.Preds.end());
          std::swap(F.Function.Blocks[1], F.Function.Blocks[2]);
        }
        const auto Observed = sourceFrameStorageBefore(F.Function, Architecture,
                                                       BinaryFormat::COFF, 10,
                                                       0, {F.Pointer}, 64);
        if (Mutation == 3 || Mutation == 5) {
          EXPECT_TRUE(Observed.empty());
          continue;
        }
        ASSERT_EQ(Observed.size(), 1U);
        ASSERT_EQ(Observed[0].Bytes.size(), 18U);
        const auto &First = Observed[0].Bytes[0];
        EXPECT_TRUE(First.Initialized);
        if (Mutation == 1) {
          EXPECT_FALSE(First.Constant);
          EXPECT_FALSE(First.Definition);
        } else if (Mutation == 2) {
          EXPECT_EQ(First.Constant, 'X');
        } else {
          ASSERT_TRUE(First.Definition);
          EXPECT_EQ(First.Definition->Instruction, 0x1008U);
        }
      }
    }
  }
}

TEST(SourceFrameAnalysis, ReinitializationAndExactObservationBudget) {
  for (const auto Architecture : {Arch::X64, Arch::AArch64}) {
    StorageObservationFixture F(Architecture);
    auto &Ops = F.Function.Blocks.front().Ops;
    auto Store = Ops[3], Zero = Ops[5];
    Store.Addr = Zero.Addr = 0x1017;
    Zero.Seq = 1;
    Ops.insert(Ops.begin() + 6,
               {op(NdOp::STORE, {}, {F.External, NdVar::scalar(0, 8)}, 0x1016),
                Store, Zero});
    size_t Remaining = limits::ir::kSourceFrameAnalysisRemaining;
    ASSERT_EQ(F.query(Architecture, BinaryFormat::COFF, &Remaining).size(), 1U);
    const size_t Cost = limits::ir::kSourceFrameAnalysisRemaining - Remaining;
    ASSERT_GT(Cost, 0U);
    Remaining = Cost;
    EXPECT_EQ(F.query(Architecture, BinaryFormat::COFF, &Remaining).size(), 1U);
    EXPECT_EQ(Remaining, 0U);
    Remaining = Cost - 1;
    EXPECT_TRUE(F.query(Architecture, BinaryFormat::COFF, &Remaining).empty());
    EXPECT_LT(Remaining, Cost);
  }
}

TEST(SourceFrameAnalysis, ObservationGuardsPreserveChecksAndOriginalQueries) {
  for (unsigned Mutation = 0; Mutation != 11; ++Mutation) {
    SCOPED_TRACE(Mutation);
    StorageObservationFixture F(Arch::X64);
    auto &Block = F.Function.Blocks.front();
    auto &Ops = Block.Ops;
    const auto Address = NdVar::tmp(0x10000, 8);
    LowOp Guard = op(NdOp::INTRINSIC, {},
                     {NdVar::scalar(uint64_t(Intrinsic::RequireAligned), 2),
                      Address, NdVar::scalar(16, 8)},
                     0x1008);
    Guard.Seq = 1;
    switch (Mutation) {
    case 1:
      Guard.Inputs[2] = NdVar::scalar(0, 8);
      break;
    case 2:
      Guard.Inputs[2] = NdVar::scalar(3, 8);
      break;
    case 3:
      Guard.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      break;
    case 4:
      Guard.MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 5:
      Guard.Output = F.External;
      break;
    case 6:
      Guard.Inputs[1] = NdVar::ram(0x9000, 8);
      break;
    case 7:
      Guard.Inputs[0].Size = 4;
      break;
    case 8:
      Guard.Inputs[2] = F.External;
      break;
    case 9:
      Guard.addInput(NdVar::scalar(0, 1));
      break;
    case 10:
      Guard.addInput(NdVar::scalar(0, 8));
      break;
    }
    const bool Valid = Mutation == 0 || Mutation == 9;
    EXPECT_EQ(isSourceFrameObservationGuard(Guard), Valid);
    Ops[2].Inputs[0] = Address;
    Ops[2].Seq = 2;
    Ops.insert(
        Ops.begin() + 2,
        {op(NdOp::COPY, Address, {NdVar::addressFragment(0x9000, 8)}, 0x1008),
         Guard});
    EXPECT_EQ(F.query(Arch::X64, BinaryFormat::COFF).size(), Valid ? 1U : 0U);
    // The existing call-slot query still refuses an intrinsic by default.
    EXPECT_FALSE(loadedCallSlot(F.Function, Block, 5, F.Vector));
    LoadedCallSlotProof Proof;
    Proof.AllowAlignmentChecks = true;
    EXPECT_EQ(loadedCallSlot(F.Function, Block, 5, F.Vector, &Proof),
              Valid ? std::optional<va_t>(0x9000) : std::nullopt);
  }
}

TEST(SourceFrameAnalysis, DisplayBytesAuthenticateWholeImmutableLoads) {
  for (const auto Architecture : {Arch::X64, Arch::AArch64}) {
    for (const auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
      StorageObservationFixture F(Architecture);
      BinaryImage Image;
      Image.Arch = Architecture;
      Image.Format = Format;
      Image.Bits = Bitness::Bits64;
      Segment Data;
      Data.VA = 0x9000;
      Data.Name = ".rdata";
      Data.Flags = SegmentFlags::Readable;
      for (char Character : std::string("WideText")) {
        Data.Data.push_back(Character);
        Data.Data.push_back(0);
      }
      Data.Size = Data.Data.size();
      Image.Segments.push_back(Data);
      auto Expected = Data.Data;
      Expected.insert(Expected.end(), {0, 0});
      const auto Observed = observeSourceFrameBytes(F.Function, Image);
      ASSERT_EQ(Observed.size(), 1U);
      EXPECT_EQ(Observed[0].Instruction, 0x1018U);
      EXPECT_EQ(Observed[0].FrameOffset, -64);
      EXPECT_EQ(Observed[0].Bytes, Expected);
      Section ReadOnly;
      ReadOnly.VA = Data.VA;
      ReadOnly.Size = Data.Size;
      ReadOnly.Flags = SegmentFlags::Readable;
      Image.Sections = {ReadOnly};
      ASSERT_EQ(observeSourceFrameBytes(F.Function, Image).size(), 1U);
      Image.Sections.push_back(ReadOnly);
      EXPECT_TRUE(observeSourceFrameBytes(F.Function, Image).empty());
      Image.Sections.resize(1);
      Image.Sections[0].Size = Data.Size - 1;
      EXPECT_TRUE(observeSourceFrameBytes(F.Function, Image).empty());
      Image.Sections.clear();
      Image.DataPtrRelocSlots.insert(0x8ffc);
      EXPECT_TRUE(observeSourceFrameBytes(F.Function, Image).empty());
      Image.DataPtrRelocSlots.clear();
      Image.Segments.front().Flags =
          SegmentFlags::Readable | SegmentFlags::Writable;
      EXPECT_TRUE(observeSourceFrameBytes(F.Function, Image).empty());
      Image.Segments.front() = Data;
      Image.Segments.front().Data.resize(8);
      EXPECT_TRUE(observeSourceFrameBytes(F.Function, Image).empty());
      Image.Segments.front() = Data;
      Image.Segments.push_back(Data);
      EXPECT_TRUE(observeSourceFrameBytes(F.Function, Image).empty());
      Image.Segments.pop_back();
      F.Function.Blocks.front().Ops[2].MemoryOrdering =
          NdMemoryOrdering::Acquire;
      EXPECT_TRUE(observeSourceFrameBytes(F.Function, Image).empty());
    }
  }
}

TEST(SourceFrameAnalysis, CallStorageRejectsEscapesAndChangedOccurrence) {
  for (unsigned Mutation = 0; Mutation < 8; ++Mutation) {
    SCOPED_TRACE(Mutation);
    FrameFixture F;
    auto &Ops = F.Low.Blocks.front().Ops;
    Ops.insert(Ops.begin() + 7, op(NdOp::COPY, X0, {Slot}, 0x101a));
    auto ABI = signature();
    ABI.Parameters[0].Type = NdType::makePtr(NdType::makeVoid());
    const auto OriginalSite = *nativeSourceCallKey(Ops[8]);
    auto Site = OriginalSite;
    NativeSourceCallContract Contract;
    Contract.Signature = &ABI;
    NativeSourceCalls Calls{{OriginalSite, Contract}};
    size_t Parameter = 0, Bytes = 16;
    switch (Mutation) {
    case 0:
      ++Site.Sequence;
      break;
    case 1:
      Parameter = 1;
      break;
    case 2:
      Bytes = 33;
      break;
    case 3:
      Bytes = 4097;
      break;
    case 4:
      ABI.Parameters[0].Type = NdType::makeInt(8);
      break;
    case 5:
      Ops[4].Inputs[1] = SP;
      break;
    case 6:
      Ops[7].Inputs[0] = SP;
      Ops.insert(Ops.begin() + 8,
                 op(NdOp::CALL, X0, {NdVar::cst(0x5000, 8)}, 0x101b));
      Calls.emplace(*nativeSourceCallKey(Ops[8]), Contract);
      break;
    case 7:
      Ops[7].Inputs[0] = NdVar::scalar(0, 8);
      break;
    }
    EXPECT_FALSE(sourceFrameCallArgumentStorage(F.Low, Arch::AArch64, Calls,
                                                Site, Parameter, Bytes));
  }
}

TEST(SourceFrameAnalysis, CallStorageMeetsEveryReachingDefinition) {
  FrameFixture F;
  F.diamond();
  auto &Tail = F.Low.Blocks.back().Ops;
  Tail.insert(Tail.begin() + 1, op(NdOp::COPY, X0, {Slot}, 0x101a));
  auto ABI = signature();
  ABI.Parameters[0].Type = NdType::makePtr(NdType::makeVoid());
  const auto Site = *nativeSourceCallKey(Tail[2]);
  NativeSourceCallContract Contract;
  Contract.Signature = &ABI;
  const NativeSourceCalls Calls{{Site, Contract}};
  const auto Original =
      sourceFrameCallArgumentStorage(F.Low, Arch::AArch64, Calls, Site, 0, 8);
  ASSERT_TRUE(Original);
  EXPECT_TRUE(Original->Bytes[0].Definition);
  F.Low.Blocks[1].Ops.push_back(
      op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 4)}, 0x1104));
  const auto Changed =
      sourceFrameCallArgumentStorage(F.Low, Arch::AArch64, Calls, Site, 0, 8);
  ASSERT_TRUE(Changed);
  for (unsigned I = 0; I < 4; ++I) {
    EXPECT_TRUE(Changed->Bytes[I].Initialized);
    EXPECT_FALSE(Changed->Bytes[I].Constant);
    EXPECT_FALSE(Changed->Bytes[I].Definition);
  }
  EXPECT_TRUE(Changed->Bytes[4].Definition);
  // A suffix backedge participates in the next observation; a first visit
  // may not seed a block-construction certificate.
  F.Low.Blocks.back().Ops.pop_back();
  F.Low.Blocks.back().Succs = {8};
  F.Low.Blocks[1].Preds.push_back(10);
  EXPECT_FALSE(
      sourceFrameCallArgumentStorage(F.Low, Arch::AArch64, Calls, Site, 0, 8));
}

TEST(SourceFrameAnalysis, VolatileEntryIdentitySurvivesOnlyCompleteSpills) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 9; ++Mutation) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Mutation);
      const auto &TRI = getTargetRegInfo(Architecture);
      const auto Carrier =
          Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::R10;
      const auto Stack = NdVar::reg(TRI.StackPointer, 8);
      const auto FrameBytes = Architecture == Arch::AArch64 ? 32 : 24;
      const auto Input = NdVar::reg(Carrier, 8);
      SourceFunctionTypeHint Empty;
      Empty.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      Empty.ReturnType = NdType::makeVoid();
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(Empty, Architecture, Error));
      auto Forward = Empty;
      Forward.Parameters.push_back(
          {"input",
           NdType::makeInt(8),
           {SourceABICarrierKind::IntegerRegister, Carrier, 0, 8}});
      LowFunc F;
      F.Entry = 0x1000;
      LowBlock B;
      B.Id = 0;
      B.StartAddr = F.Entry;
      B.Ops = {op(NdOp::INT_SUB, Stack, {Stack, NdVar::scalar(FrameBytes, 8)},
                  0x1000),
               op(NdOp::STORE, {}, {Stack, Input}, 0x1004),
               op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)}, 0x1008),
               op(NdOp::LOAD, Input, {Stack}, 0x100c),
               op(NdOp::CALL, {}, {NdVar::cst(0x3000, 8)}, 0x1010),
               op(NdOp::INT_ADD, Stack, {Stack, NdVar::scalar(FrameBytes, 8)},
                  0x1014),
               op(NdOp::RETURN, {}, {}, 0x1018)};
      NativeSourceCallContract First, Second;
      First.Signature = &Empty;
      Second.Signature = &Forward;
      NativeSourceCalls Calls{{*nativeSourceCallKey(B.Ops[2]), First},
                              {*nativeSourceCallKey(B.Ops[4]), Second}};
      if (Mutation == 1)
        B.Ops[3] = op(NdOp::COPY, Input, {NdVar::scalar(0, 8)}, 0x100c);
      if (Mutation == 2)
        B.Ops[3].Output.Size = 4;
      if (Mutation == 3)
        B.Ops[1].Inputs[1].Size = 4;
      if (Mutation == 4) {
        B.Ops[3] = op(NdOp::COPY, Input, {Input}, 0x100c);
      }
      if (Mutation == 5) {
        Forward.Parameters[0].Type = NdType::makeInt(4);
        Forward.Parameters[0].Location.ValueBytes = 4;
      }
      if (Mutation == 6)
        B.Ops.insert(B.Ops.begin() + 3,
                     op(NdOp::STORE, {}, {Stack, NdVar::scalar(0, 1)}, 0x100a));
      if (Mutation == 7)
        B.Ops.insert(B.Ops.begin() + 1,
                     op(NdOp::COPY, Input, {NdVar::scalar(0, 4)}, 0x1002));
      if (Mutation == 8) {
        // An unrelated callee-save write must still invalidate restoration.
        const auto Preserved =
            Architecture == Arch::AArch64 ? a64reg::X19 : x86reg::RBX;
        B.Ops.insert(B.Ops.begin(), op(NdOp::COPY, NdVar::reg(Preserved, 8),
                                       {NdVar::scalar(0, 8)}, 0x1000));
      }
      if (TRI.LinkRegister) {
        const auto Address = NdVar::tmp(300, 8);
        const auto Link = NdVar::reg(TRI.LinkRegister, 8);
        const auto Save =
            std::find_if(B.Ops.begin(), B.Ops.end(),
                         [](const LowOp &O) { return O.Opcode == NdOp::CALL; });
        B.Ops.insert(Save, {op(NdOp::INT_ADD, Address,
                               {Stack, NdVar::scalar(16, 8)}, 0x1006),
                            op(NdOp::STORE, {}, {Address, Link}, 0x1006)});
        B.Ops.insert(
            B.Ops.end() - 2,
            {op(NdOp::INT_ADD, Address, {Stack, NdVar::scalar(16, 8)}, 0x1012),
             op(NdOp::LOAD, Link, {Address}, 0x1012)});
        B.Ops.back().addInput(Link);
      }
      F.Blocks.push_back(std::move(B));
      std::set<uint64_t> Used;
      const bool Valid =
          restoresNativeSourceState(F, Architecture, Calls, &Used);
      if (Mutation == 8) {
        EXPECT_FALSE(Valid);
      } else {
        ASSERT_TRUE(Valid);
        EXPECT_EQ(Used.count(Carrier), Mutation == 0 ? 1U : 0U);
      }
    }
}

TEST(SourceFrameAnalysis, VolatileEntryUsesMeetEveryPathAndLoopBackedge) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation < 6; ++Mutation) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Mutation);
      const auto Carrier =
          Architecture == Arch::AArch64 ? a64reg::X8 : x86reg::R10;
      const auto Input = NdVar::reg(Carrier, 8);
      SourceFunctionTypeHint ABI;
      ABI.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
      ABI.ReturnType = NdType::makeVoid();
      std::string Error;
      ASSERT_TRUE(assignDarwinScalarSourceABI(ABI, Architecture, Error));
      ABI.Parameters.push_back(
          {"input",
           NdType::makeInt(8),
           {SourceABICarrierKind::IntegerRegister, Carrier, 0, 8}});
      LowFunc F;
      F.Entry = 0x1000;
      F.Blocks.resize(5);
      for (unsigned I = 0; I < 5; ++I) {
        F.Blocks[I].Id = I;
        F.Blocks[I].StartAddr = 0x1000 + 0x100 * I;
      }
      F.Blocks[0].Succs = {1, 2};
      F.Blocks[0].Ops = {
          op(NdOp::COPY, NdVar::tmp(300, 8), {NdVar::scalar(0, 8)}, 0x1000)};
      for (unsigned I : {1, 2}) {
        F.Blocks[I].Preds = {0};
        F.Blocks[I].Succs = {3};
        F.Blocks[I].Ops = {
            op(NdOp::COPY, Input, {Input}, F.Blocks[I].StartAddr)};
      }
      F.Blocks[3].Preds = {1, 2};
      F.Blocks[3].Succs = {4};
      F.Blocks[3].Ops = {op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)}, 0x1300)};
      F.Blocks[4].Preds = {3};
      F.Blocks[4].Ops = {op(NdOp::RETURN, {}, {}, 0x1400)};
      const auto &TRI = getTargetRegInfo(Architecture);
      if (TRI.LinkRegister) {
        const auto Stack = NdVar::reg(TRI.StackPointer, 8);
        const auto Link = NdVar::reg(TRI.LinkRegister, 8);
        F.Blocks[0].Ops = {
            op(NdOp::INT_SUB, Stack, {Stack, NdVar::scalar(32, 8)}, 0x1000),
            op(NdOp::STORE, {}, {Stack, Link}, 0x1004)};
        F.Blocks[4].Ops = {
            op(NdOp::LOAD, Link, {Stack}, 0x1400),
            op(NdOp::INT_ADD, Stack, {Stack, NdVar::scalar(32, 8)}, 0x1404),
            op(NdOp::RETURN, {}, {Link}, 0x1408)};
      } else {
        const auto Stack = NdVar::reg(TRI.StackPointer, 8);
        F.Blocks[0].Ops = {
            op(NdOp::INT_SUB, Stack, {Stack, NdVar::scalar(24, 8)}, 0x1000)};
        F.Blocks[4].Ops.insert(
            F.Blocks[4].Ops.begin(),
            op(NdOp::INT_ADD, Stack, {Stack, NdVar::scalar(24, 8)}, 0x1400));
      }
      NativeSourceCallContract Call;
      Call.Signature = &ABI;
      NativeSourceCalls Calls{{*nativeSourceCallKey(F.Blocks[3].Ops[0]), Call}};
      if (Mutation == 1 || Mutation == 2)
        F.Blocks[1].Ops[0] =
            op(NdOp::COPY, NdVar::reg(Carrier, Mutation == 1 ? 8 : 4),
               {NdVar::scalar(0, Mutation == 1 ? 8 : 4)}, 0x1100);
      if (Mutation == 3) {
        F.Blocks[1].Ops[0].Inputs[0] =
            NdVar::reg(getTargetRegInfo(Architecture).IntParamRegs[0], 8);
      }
      if (Mutation == 4) {
        F.Blocks[3].Preds.push_back(3);
        F.Blocks[3].Succs.push_back(3);
      }
      if (Mutation == 5)
        std::reverse(F.Blocks.begin(), F.Blocks.end());
      std::set<uint64_t> Used;
      ASSERT_TRUE(restoresNativeSourceState(F, Architecture, Calls, &Used));
      EXPECT_EQ(Used.count(Carrier), Mutation == 0 || Mutation == 5 ? 1U : 0U);
    }
}

TEST(SourceFrameAnalysis,
     ReturnsTheOriginalDefinitionWithoutPublishingAnAddress) {
  FrameFixture F;
  const auto Result = F.query();
  ASSERT_TRUE(Result);
  EXPECT_EQ(Result->FrameOffset, -32);
  EXPECT_EQ(Result->Definition,
            (SourceFrameDefinition{7, 2, 0x1008, 0, Value}));
  EXPECT_FALSE(restoresNativeSourceState(F.Low, Arch::AArch64, {}));
  EXPECT_FALSE(sourceFrameLoadedDefinition(F.Low, Arch::X64, {}, 7, 6));
  EXPECT_FALSE(sourceFrameLoadedDefinition(F.Low, Arch::AArch64, {}, 8, 6));
  EXPECT_FALSE(sourceFrameLoadedDefinition(F.Low, Arch::AArch64, {}, 7, 99));
  EXPECT_FALSE(sourceFrameLoadedDefinition(F.Low, Arch::AArch64, {}, 7, 5));
  // Numerically equal scalar constants do not inherit an instruction's origin.
  F.Low.Blocks[0].Ops[2] =
      op(NdOp::COPY, Value, {NdVar::scalar(0x6080, 8)}, 0x1008);
  EXPECT_FALSE(F.query());
}

TEST(SourceFrameAnalysis, RejectsPartialEscapedExpiredAndUninitializedBytes) {
  for (unsigned Case = 0; Case < 18; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    switch (Case) {
    case 0:
      Ops.erase(Ops.begin() + 4);
      break;
    case 1:
      Ops[4].Inputs[1].Size = 4;
      break;
    case 2:
      Ops[6].Output.Size = 4;
      break;
    case 3:
      Ops[4].Inputs[1] = SP;
      break;
    case 4:
      F.prefix({op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 1)}, 0x1015)});
      break;
    case 5:
      F.prefix({op(NdOp::INT_ADD, SP, {SP, NdVar::scalar(64, 8)}, 0x1015),
                op(NdOp::INT_SUB, SP, {SP, NdVar::scalar(64, 8)}, 0x1016)});
      break;
    case 6:
      F.prefix({op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x1015)});
      break;
    case 7:
      F.prefix({op(NdOp::COPY, Slot, {NdVar::reg(a64reg::X20, 8)}, 0x1015)});
      break;
    case 8:
      Ops[6].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 9:
      F.prefix({op(NdOp::STORE, {}, {NdVar::reg(a64reg::X20, 8), SP}, 0x1015)});
      break;
    case 10:
      F.prefix(
          {op(NdOp::COPY, NdVar::reg(a64reg::X9, 4),
              {NdVar::reg(a64reg::SP, 4)}, 0x1015),
           op(NdOp::STORE, {}, {Slot, NdVar::reg(a64reg::X9, 8)}, 0x1016)});
      break;
    case 11:
      Ops[3].Inputs[1] = NdVar::scalar(64, 8);
      break;
    case 12:
      Ops[3].Inputs[1] = NdVar::scalar(uint64_t(-8), 8);
      break;
    case 13:
      F.prefix({op(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)}, 0x1015)});
      break;
    case 14:
      Ops[4].Inputs[0].Size = 4;
      break;
    case 15:
      Ops[6].NumInputs = 0;
      break;
    case 16:
      F.prefix({op(NdOp::INT_XOR, Slot, {Slot, NdVar::scalar(1, 8)}, 0x1015)});
      break;
    case 17:
      F.prefix({op(NdOp::COPY, NdVar::reg(a64reg::X19, 4),
                   {NdVar::scalar(0, 4)}, 0x1015)});
      break;
    }
    EXPECT_FALSE(F.query());
  }
}

TEST(SourceFrameAnalysis, RequiresAllReachingPathsIndependentOfBlockOrder) {
  FrameFixture F;
  F.diamond();
  const auto Expected = F.query();
  ASSERT_TRUE(Expected);
  std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
  EXPECT_EQ(F.query(), Expected);
  for (unsigned Case = 0; Case < 10; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture Bad;
    Bad.diamond();
    auto &B = Bad.Low.Blocks;
    switch (Case) {
    case 0:
      B[2].Ops.push_back(
          op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 8)}, 0x1204));
      break;
    case 1:
      B[2].Ops.push_back(
          op(NdOp::STORE, {}, {NdVar::reg(a64reg::X20, 8), SP}, 0x1204));
      break;
    case 2:
      B[0].Ops.erase(B[0].Ops.begin() + 4);
      B[1].Ops.push_back(op(NdOp::STORE, {}, {Slot, Value}, 0x1104));
      break;
    case 3:
      B[3].Preds.pop_back();
      break;
    case 4:
      B[3].Preds.push_back(8);
      break;
    case 5:
      B[2].Succs.push_back(9);
      B[2].Preds.push_back(9);
      break;
    case 6:
      B[3].Succs = {7};
      B[0].Preds = {10};
      break;
    case 7:
      B[1].Id = 9;
      break;
    case 8:
      B[2].Ops.clear();
      break;
    case 9:
      B[0].Succs = {8};
      B[2].Preds.clear();
      break;
    }
    // A loop which writes only an unrelated register preserves the slot.
    EXPECT_EQ(bool(Bad.query()), Case == 5);
  }
  // A loop reached only after the query is irrelevant to this prefix.
  F = FrameFixture{};
  auto &Root = F.Low.Blocks[0];
  Root.Ops.resize(7);
  Root.Succs = {8};
  LowBlock Later;
  Later.Id = 8;
  Later.StartAddr = 0x2000;
  Later.Preds = {7, 8};
  Later.Succs = {8};
  Later.Ops = {op(NdOp::BRANCH, {}, {NdVar::cst(0x2000, 8)}, 0x2000)};
  F.Low.Blocks.push_back(Later);
  EXPECT_TRUE(F.query());
}

TEST(SourceFrameAnalysis, UsesBoundedCallWritesAndPossibleReturnAliases) {
  auto Signature = signature(1, true);
  for (unsigned Case = 0; Case < 11; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture F;
    SourceFrameEffects Effects;
    Effects.WritableFrameParameters[0] = 24;
    F.prefix({op(NdOp::INT_ADD, X0, {SP, NdVar::scalar(0, 8)}, 0x1015),
              op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x1016)});
    switch (Case) {
    case 1:
      Effects.WritableFrameParameters[0] = 33;
      break;
    case 2:
      Effects = {};
      break;
    case 3:
      Effects.WritableFrameParameters[0] = 65;
      break;
    case 4:
      Effects.ReadOnlyFrameParameters[0] = 24;
      break;
    case 5:
      Effects.ReturnFrameOrExternal = {0, 24};
      break;
    case 6:
      Effects.ReturnFrameOrExternal = {0, 24};
      F.Low.Blocks[0].Ops[8].Inputs[0] = X0;
      break;
    case 7:
      Effects.ReturnFrameOrExternal = {0, 24};
      F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin() + 8,
                                 op(NdOp::STORE, {}, {Slot, X0}, 0x1017));
      break;
    case 8:
      Effects.WritableFrameParameters[0] = 0;
      break;
    case 9:
      Effects = {};
      Effects.ReadOnlyFrameParameters[0] = 40;
      break;
    case 10:
      F.Low.Blocks[0].Ops[6].Inputs[1] = NdVar::scalar(16, 8);
      break;
    }
    EXPECT_EQ(bool(F.query(callsAt(0x1016, Signature, Effects))),
              Case == 0 || Case == 5 || Case == 9);
  }
}

TEST(SourceFrameAnalysis, RetainsConditionalScratchAndLiveRecordRequirements) {
  auto Begin = signature(4);
  auto End = signature(1);
  SourceFrameEffects Initialize, Finish;
  using Scratch = SourceFrameScratchEffect;
  Initialize.WritableFrameParameters[1] = 24;
  Initialize.Scratch = {Scratch::Domain::SwiftAccess,
                        Scratch::Action::Initialize, 1, 24,
                        SourceFrameScalarCondition{2, {0, 1}}};
  Finish.ReadOnlyFrameParameters[0] = 24;
  Finish.Scratch = {Scratch::Domain::SwiftAccess, Scratch::Action::Finish, 0,
                    24, std::nullopt};
  auto Calls = callsAt(0x1034, Begin, Initialize);
  Calls.merge(callsAt(0x103c, End, Finish));
  for (unsigned Case = 0; Case < 12; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture F;
    const auto X1 = NdVar::reg(a64reg::X1, 8);
    const auto X2 = NdVar::reg(a64reg::X2, 8);
    F.prefix({op(NdOp::COPY, X0, {NdVar::scalar(0, 8)}, 0x1024),
              op(NdOp::COPY, X1, {SP}, 0x1028),
              op(NdOp::COPY, X2,
                 {NdVar::scalar(Case == 1   ? 1
                                : Case == 2 ? 32
                                            : 0,
                                8)},
                 0x102c),
              op(NdOp::COPY, NdVar::reg(a64reg::X3, 8), {NdVar::scalar(0, 8)},
                 0x1030),
              op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x1034),
              op(NdOp::COPY, X0, {SP}, 0x1038),
              op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x103c)});
    auto &Ops = F.Low.Blocks[0].Ops;
    auto Contract = Calls;
    switch (Case) {
    case 3:
      Ops[8].Inputs[0] = NdVar::reg(a64reg::X9, 8);
      break;
    case 4:
      Ops[8].Inputs[0].Size = 4;
      break;
    case 5:
      Ops.erase(Ops.begin() + 10);
      break;
    case 6:
      Ops.insert(Ops.begin() + 12,
                 op(NdOp::STORE, {}, {SP, NdVar::scalar(0, 1)}, 0x1039));
      break;
    case 7:
      Ops[11] = op(NdOp::INT_ADD, X0, {SP, NdVar::scalar(8, 8)}, 0x1038);
      break;
    case 8:
      Ops[7] = op(NdOp::INT_ADD, X1, {SP, NdVar::scalar(16, 8)}, 0x1028);
      break;
    case 9:
      Contract.begin()->second.Scratch->Condition->Parameter = 4;
      break;
    case 10:
      Ops.insert(Ops.begin() + 12,
                 op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 1)}, 0x1039));
      break;
    case 11:
      Ops.insert(Ops.begin() + 12,
                 op(NdOp::INT_ADD, SP, {SP, NdVar::scalar(16, 8)}, 0x1039));
      break;
    }
    EXPECT_EQ(bool(F.query(Contract)), Case == 0 || Case == 1);
  }
}

namespace {
FrameFixture loopFrame(bool QueryInLoop) {
  FrameFixture F;
  const auto Original = F.Low.Blocks[0];
  F.Low.Blocks.resize(4);
  for (size_t I = 0; I < 4; ++I) {
    F.Low.Blocks[I] = {};
    F.Low.Blocks[I].Id = 7 + I;
    F.Low.Blocks[I].StartAddr = 0x1000 + I * 0x100;
  }
  auto &Entry = F.Low.Blocks[0];
  Entry.Ops.assign(Original.Ops.begin(), Original.Ops.begin() + 6);
  Entry.Succs = {8};
  auto &Header = F.Low.Blocks[1];
  Header.Preds = {7, 9};
  Header.Succs = {9, 10};
  Header.Ops = {
      op(NdOp::COPY, NdVar::reg(a64reg::X9, 8), {NdVar::scalar(0, 8)}, 0x1100)};
  auto &Body = F.Low.Blocks[2];
  Body.Preds = {8};
  Body.Succs = {8};
  Body.Ops = {op(NdOp::COPY, NdVar::reg(a64reg::X10, 8), {NdVar::scalar(1, 8)},
                 0x1200)};
  auto &Exit = F.Low.Blocks[3];
  Exit.Preds = {8};
  Exit.Ops.assign(Original.Ops.begin() + 7, Original.Ops.end());
  auto &QueryOps = QueryInLoop ? Header.Ops : Exit.Ops;
  QueryOps.insert(QueryOps.begin(), Original.Ops[6]);
  return F;
}
} // namespace

TEST(SourceFrameAnalysis, PreservesOnePreloopDefinitionAcrossAllIterations) {
  for (bool Inside : {false, true}) {
    SCOPED_TRACE(Inside);
    auto F = loopFrame(Inside);
    const auto Expected = F.query();
    ASSERT_TRUE(Expected);
    EXPECT_EQ(Expected->Definition,
              (SourceFrameDefinition{7, 2, 0x1008, 0, Value}));
    EXPECT_EQ(Expected->FrameOffset, -32);
    std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
    EXPECT_EQ(F.query(), Expected);
  }
}

TEST(SourceFrameAnalysis, LoopQueryRequiresTheCompleteBackedgeEffects) {
  const auto Signature = signature();
  SourceFrameEffects ReadOnly;
  ReadOnly.ReadOnlyFrameParameters[0] = 8;
  SourceFrameEffects Written;
  Written.WritableFrameParameters[0] = 8;
  for (bool Inside : {false, true}) {
    SCOPED_TRACE(Inside);
    auto F = loopFrame(Inside);
    F.Low.Blocks[2].Ops = {op(NdOp::COPY, X0, {Slot}, 0x1200),
                           op(NdOp::CALL, {}, {NdVar::cst(0x4000, 8)}, 0x1204)};
    EXPECT_TRUE(F.query(callsAt(0x1204, Signature, ReadOnly)));
    EXPECT_FALSE(F.query());
    EXPECT_FALSE(F.query(callsAt(0x1204, Signature, {})));
    EXPECT_FALSE(F.query(callsAt(0x1204, Signature, Written)));
    // Calls after the queried LOAD still affect the next iteration.
    if (Inside) {
      F.Low.Blocks[1].Ops.insert(F.Low.Blocks[1].Ops.end(),
                                 F.Low.Blocks[2].Ops.begin(),
                                 F.Low.Blocks[2].Ops.end());
      F.Low.Blocks[2].Ops = {op(NdOp::COPY, X0, {NdVar::scalar(0, 8)}, 0x1300)};
      EXPECT_TRUE(F.query(callsAt(0x1204, Signature, ReadOnly)));
      EXPECT_FALSE(F.query());
      EXPECT_FALSE(F.query(callsAt(0x1204, Signature, Written)));
    }
  }
}

TEST(SourceFrameAnalysis,
     LoopReloadRejectsChangedExpiredEscapedOrPartialBytes) {
  for (bool Inside : {false, true})
    for (unsigned Case = 0; Case < 13; ++Case) {
      SCOPED_TRACE(Inside);
      SCOPED_TRACE(Case);
      auto F = loopFrame(Inside);
      auto &Body = F.Low.Blocks[2].Ops;
      switch (Case) {
      case 0:
        Body.push_back(
            op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 8)}, 0x1204));
        break;
      case 1:
        Body.push_back(
            op(NdOp::STORE, {}, {Slot, NdVar::scalar(0, 1)}, 0x1204));
        break;
      case 2:
        Body.push_back(op(NdOp::STORE, {}, {Slot, SP}, 0x1204));
        break;
      case 3:
        Body.push_back(
            op(NdOp::STORE, {}, {NdVar::reg(a64reg::X20, 8), Slot}, 0x1204));
        break;
      case 4:
        Body.push_back(
            op(NdOp::INT_ADD, SP, {SP, NdVar::scalar(64, 8)}, 0x1204));
        Body.push_back(
            op(NdOp::INT_SUB, SP, {SP, NdVar::scalar(64, 8)}, 0x1208));
        break;
      case 5:
        Body.push_back(
            op(NdOp::COPY, Slot, {NdVar::reg(a64reg::X20, 8)}, 0x1204));
        break;
      case 6:
        Body.push_back(op(NdOp::COPY, NdVar::reg(a64reg::X19, 4),
                          {NdVar::scalar(0, 4)}, 0x1204));
        break;
      case 7:
        Body.push_back(op(NdOp::CALL, {}, {NdVar::cst(0x5000, 8)}, 0x1204));
        break;
      case 8:
        F.Low.Blocks[0].Ops.erase(F.Low.Blocks[0].Ops.begin() + 4);
        Body.push_back(op(NdOp::STORE, {}, {Slot, Value}, 0x1204));
        break;
      case 9:
        Body.push_back(
            op(NdOp::COPY, Value, {NdVar::addressFragment(0x6080, 8)}, 0x1204));
        Body.push_back(op(NdOp::STORE, {}, {Slot, Value}, 0x1208));
        break;
      case 10:
        Body.push_back(
            op(NdOp::INT_SUB, SP, {SP, NdVar::reg(a64reg::X20, 8)}, 0x1204));
        break;
      case 11:
        F.Low.Blocks[1].Preds.pop_back();
        break;
      case 12:
        Body.push_back(op(
            NdOp::STORE, {},
            {NdVar::reg(a64reg::X20, 8), NdVar::reg(a64reg::X19, 4)}, 0x1204));
        break;
      }
      EXPECT_FALSE(F.query());
      std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
      EXPECT_FALSE(F.query());
    }
}

TEST(SourceFrameAnalysis, DoesNotEquateDifferentExecutionsOfALoopProducer) {
  for (unsigned Case = 0; Case < 3; ++Case) {
    SCOPED_TRACE(Case);
    auto F = loopFrame(true);
    auto &Header = F.Low.Blocks[1].Ops;
    std::vector<LowOp> Producer = {
        op(NdOp::LOAD, Value, {NdVar::reg(a64reg::X20, 8)}, 0x1100),
        op(NdOp::STORE, {}, {Slot, Value}, 0x1104)};
    if (Case == 1)
      Producer[0] =
          op(NdOp::COPY, Value, {NdVar::addressFragment(0x6080, 8)}, 0x1100);
    Header.insert(Header.begin(), Producer.begin(), Producer.end());
    if (Case == 2) {
      // The same static load may also execute in an earlier iteration before
      // a later query block. A definition site alone proves no value identity.
      Header.erase(Header.begin() + 2);
      F.Low.Blocks[3].Ops.insert(
          F.Low.Blocks[3].Ops.begin(),
          op(NdOp::LOAD, NdVar::tmp(16, 8), {Slot}, 0x1018));
    }
    EXPECT_FALSE(F.query());
  }
}

TEST(SourceFrameAnalysis, LoopQueryRetainsScratchConditionsAndObligations) {
  using Scratch = SourceFrameScratchEffect;
  const auto Begin = signature(4), End = signature();
  SourceFrameEffects Initialize, Finish;
  Initialize.WritableFrameParameters[1] = 24;
  Initialize.Scratch = {Scratch::Domain::SwiftAccess,
                        Scratch::Action::Initialize,
                        1,
                        24,
                        SourceFrameScalarCondition{2, {0, 1, 32, 33}},
                        {32, 33}};
  Finish.ReadOnlyFrameParameters[0] = 24;
  Finish.Scratch = {Scratch::Domain::SwiftAccess,
                    Scratch::Action::Finish,
                    0,
                    24,
                    std::nullopt,
                    {}};
  auto Calls = callsAt(0x1210, Begin, Initialize);
  Calls.merge(callsAt(0x1218, End, Finish));
  for (bool Inside : {false, true})
    for (unsigned Case = 0; Case < 6; ++Case) {
      SCOPED_TRACE(Inside);
      SCOPED_TRACE(Case);
      auto F = loopFrame(Inside);
      const uint64_t Flags = Case == 2 ? 0 : Case == 3 ? 4 : 32;
      auto &Body = F.Low.Blocks[2].Ops;
      Body = {op(NdOp::COPY, X0, {NdVar::reg(a64reg::X20, 8)}, 0x1200),
              op(NdOp::COPY, NdVar::reg(a64reg::X1, 8), {SP}, 0x1204),
              op(NdOp::COPY, NdVar::reg(a64reg::X2, 8),
                 {NdVar::scalar(Flags, 8)}, 0x1208),
              op(NdOp::COPY, NdVar::reg(a64reg::X3, 8), {NdVar::scalar(0, 8)},
                 0x120c),
              op(NdOp::CALL, {}, {NdVar::cst(0x4000, 8)}, 0x1210),
              op(NdOp::COPY, X0, {SP}, 0x1214),
              op(NdOp::CALL, {}, {NdVar::cst(0x4000, 8)}, 0x1218)};
      if (Case == 1 || Case == 2)
        Body.resize(5);
      if (Case == 4)
        Body.insert(Body.begin() + 5,
                    op(NdOp::STORE, {}, {SP, NdVar::scalar(0, 1)}, 0x1212));
      if (Case == 5)
        Body[5] = op(NdOp::INT_ADD, X0, {SP, NdVar::scalar(8, 8)}, 0x1214);
      EXPECT_EQ(bool(F.query(Calls)), Case == 0 || Case == 2);
    }
}

TEST(SourceFrameAnalysis, BoundsDefinitionAndTransferWork) {
  FrameFixture F;
  std::vector<LowOp> Prefix;
  for (size_t I = 0; I < 4097; ++I)
    Prefix.push_back(op(NdOp::COPY, Value,
                        {NdVar::addressFragment(0x6000 + I, 8)},
                        0x4000 + I * 4));
  F.Low.Blocks[0].Ops.insert(F.Low.Blocks[0].Ops.begin() + 6, Prefix.begin(),
                             Prefix.end());
  EXPECT_FALSE(F.query());
  F = FrameFixture{};
  F.Low.Blocks.resize(16385);
  EXPECT_FALSE(F.query());
}

TEST(SourceFrameAnalysis, JoinsOnlyOrderedBytesOfOneDefinition) {
  for (unsigned Case = 0; Case < 4; ++Case) {
    SCOPED_TRACE(Case);
    FrameFixture F;
    const auto LowHalf = NdVar::reg(a64reg::X10, 4);
    const auto HighHalf = NdVar::reg(a64reg::X11, 4);
    const auto NextSlot = NdVar::reg(a64reg::X12, 8);
    auto &Ops = F.Low.Blocks[0].Ops;
    Ops.erase(Ops.begin() + 4);
    Ops.insert(Ops.begin() + 4,
               {op(NdOp::SUBBYTES, LowHalf,
                   {Value, NdVar::scalar(Case == 1 ? 4 : 0, 4)}, 0x1010),
                op(NdOp::SUBBYTES, HighHalf,
                   {Value, NdVar::scalar(Case == 1 ? 0 : 4, 4)}, 0x1011),
                op(NdOp::INT_ADD, NextSlot,
                   {Slot, NdVar::scalar(Case == 2 ? 3 : 4, 8)}, 0x1012),
                op(NdOp::STORE, {}, {Slot, LowHalf}, 0x1013),
                op(NdOp::STORE, {}, {NextSlot, HighHalf}, 0x1014)});
    if (Case == 3)
      Ops.insert(
          Ops.begin() + 5,
          op(NdOp::COPY, Value, {NdVar::addressFragment(0x6080, 8)}, 0x1010));
    const auto Result = F.query();
    EXPECT_EQ(bool(Result), Case == 0);
    if (Result)
      EXPECT_EQ(Result->Definition.Instruction, 0x1008U);
  }
}

namespace {
// These LowIR tests isolate the shared lifetime transfer. Machine/import/ABI
// authentication is exercised independently by NativeSourceHints' real fixture.
struct AccessLifetimeFixture {
  LowFunc Low;
  NativeSourceCalls Calls;
  SourceFunctionTypeHint Begin = signature(4), End = signature(1),
                         Ordinary = signature(0), Borrow = signature(1);
  SourceFrameEffects Initialize, Finish;
  const NdVar X1 = NdVar::reg(a64reg::X1, 8), X2 = NdVar::reg(a64reg::X2, 8),
              X3 = NdVar::reg(a64reg::X3, 8), X9 = NdVar::reg(a64reg::X9, 8),
              X20 = NdVar::reg(a64reg::X20, 8), LR = NdVar::reg(a64reg::X30, 8);
  va_t Next = 0x1000;
  AccessLifetimeFixture() {
    using Scratch = SourceFrameScratchEffect;
    Initialize.WritableFrameParameters[1] = 24;
    Initialize.Scratch = {Scratch::Domain::SwiftAccess,
                          Scratch::Action::Initialize,
                          1,
                          24,
                          SourceFrameScalarCondition{2, {0, 1, 32, 33}},
                          {32, 33}};
    Finish.ReadOnlyFrameParameters[0] = 24;
    Finish.Scratch = {Scratch::Domain::SwiftAccess,
                      Scratch::Action::Finish,
                      0,
                      24,
                      std::nullopt,
                      {}};
    Low.Entry = Next;
    Low.Blocks.emplace_back();
    Low.Blocks[0].Id = 0;
    Low.Blocks[0].StartAddr = Next;
    add(NdOp::INT_SUB, SP, {SP, NdVar::scalar(96, 8)});
    address(X9, 80);
    add(NdOp::STORE, {}, {X9, LR});
  }
  void add(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs) {
    Low.Blocks[0].Ops.push_back(op(Opcode, Output, Inputs, Next));
    Next += 4;
  }
  void address(NdVar Output, uint64_t Offset) {
    add(NdOp::INT_ADD, Output, {SP, NdVar::scalar(Offset, 8)});
  }
  void call(const SourceFunctionTypeHint &Type,
            const SourceFrameEffects &Effects = {}) {
    add(NdOp::CALL, {}, {NdVar::cst(0x4000, 8)});
    auto &Contract = Calls[*nativeSourceCallKey(Low.Blocks[0].Ops.back())];
    Contract.Signature = &Type;
    static_cast<SourceFrameEffects &>(Contract) = Effects;
  }
  void begin(uint64_t Offset = 0, uint64_t Flags = 33) {
    add(NdOp::COPY, X0, {X20});
    address(X1, Offset);
    add(NdOp::COPY, X2, {NdVar::scalar(Flags, 8)});
    add(NdOp::COPY, X3, {NdVar::scalar(0, 8)});
    call(Begin, Initialize);
  }
  void end(uint64_t Offset = 0) {
    address(X0, Offset);
    call(End, Finish);
  }
  void epilogue() {
    address(X9, 80);
    add(NdOp::LOAD, LR, {X9});
    add(NdOp::INT_ADD, SP, {SP, NdVar::scalar(96, 8)});
    add(NdOp::RETURN, {}, {LR});
  }
  bool proves(const SourceFunctionTypeHint *Result = nullptr) const {
    return restoresNativeSourceState(Low, Arch::AArch64, Calls, nullptr,
                                     Result);
  }
};
} // namespace

TEST(SourceFrameAnalysis, RetainedScratchClosesNestedAndNonLifoLifetimes) {
  for (unsigned Flags : {0, 1, 32, 33})
    for (bool Reverse : {false, true}) {
      AccessLifetimeFixture F;
      F.begin(0, Flags);
      F.call(F.Ordinary);
      F.begin(32, Flags);
      F.end(Reverse ? 32 : 0);
      F.call(F.Ordinary);
      F.end(Reverse ? 0 : 32);
      F.epilogue();
      EXPECT_TRUE(F.proves()) << Flags << ": " << Reverse;
    }
}

TEST(SourceFrameAnalysis, RetainedScratchRejectsLostOrCorruptedLifetimes) {
  for (unsigned Case = 0; Case < 15; ++Case) {
    SCOPED_TRACE(Case);
    AccessLifetimeFixture F;
    if (Case != 0)
      F.begin();
    switch (Case) {
    case 0:
      F.end();
      break; // end before begin
    case 1:
      break; // missing end
    case 2:
      F.begin();
      F.end();
      break; // same live record twice
    case 3:
      F.end();
      F.end();
      break;
    case 4:
      F.end(8);
      break;
    case 5:
      F.begin(16);
      F.end(16);
      F.end();
      break; // overlapping records
    case 6:
    case 7:
      F.address(F.X9, Case == 6 ? 0 : 23);
      F.add(NdOp::STORE, {}, {F.X9, NdVar::scalar(0, 1)});
      F.end();
      break;
    case 8:
      F.add(NdOp::INT_ADD, SP, {SP, NdVar::scalar(16, 8)});
      F.add(NdOp::INT_SUB, SP, {SP, NdVar::scalar(16, 8)});
      F.end();
      break;
    case 9:
      F.add(NdOp::COPY, SP, {F.X20});
      F.end();
      break;
    case 10:
      F.address(X0, 0);
      F.Low.Blocks[0].Ops.back().Output.Size = 4;
      F.call(F.End, F.Finish);
      break;
    case 11:
    case 12: {
      SourceFrameEffects Effect;
      if (Case == 11)
        Effect.WritableFrameParameters[0] = 24;
      else
        Effect.ReadOnlyFrameParameters[0] = 24;
      F.address(X0, 0);
      F.call(F.Borrow, Effect);
      F.end();
      break;
    }
    case 13: // an external record could acquire our link through TLS
      F.begin(32);
      F.Low.Blocks[0].Ops[F.Low.Blocks[0].Ops.size() - 4] =
          op(NdOp::COPY, F.X1, {F.X20}, F.Next - 16);
      F.end(32);
      F.end();
      break;
    case 14: // a second untracked initialization does not unlink a live record
      F.begin(0, 0);
      F.end();
      break;
    }
    F.epilogue();
    EXPECT_FALSE(F.proves());
  }
}

TEST(SourceFrameAnalysis, RetainedScratchCannotDisappearAtJoinsOrBackedges) {
  for (unsigned Case = 0; Case < 5; ++Case)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Case);
      AccessLifetimeFixture F;
      const size_t BeforeBegin = F.Low.Blocks[0].Ops.size();
      F.begin();
      const size_t AfterBegin = F.Low.Blocks[0].Ops.size();
      F.end();
      const size_t AfterEnd = F.Low.Blocks[0].Ops.size();
      F.epilogue();
      const auto Ops = F.Low.Blocks[0].Ops;
      F.Low.Blocks.resize(4);
      for (int I = 0; I < 4; ++I) {
        F.Low.Blocks[I] = {};
        F.Low.Blocks[I].Id = I;
        F.Low.Blocks[I].StartAddr = 0x1000 + I * 0x100;
      }
      auto &Root = F.Low.Blocks[0], &Left = F.Low.Blocks[1],
           &Right = F.Low.Blocks[2], &Join = F.Low.Blocks[3];
      Root.Succs = {1, 2};
      Left.Preds = Right.Preds = {0};
      Left.Succs = Right.Succs = {3};
      Join.Preds = {1, 2};
      const auto Nop = op(NdOp::COPY, F.X9, {NdVar::scalar(0, 8)}, 0x1900);
      Left.Ops = Right.Ops = {Nop};
      if (Case < 2) {
        Root.Ops.assign(Ops.begin(), Ops.begin() + BeforeBegin);
        // A balanced access in one arm is fine. An unclosed access must not
        // disappear in the other arm's empty set at the common return.
        Left.Ops.assign(Ops.begin() + BeforeBegin,
                        Ops.begin() + (Case == 0 ? AfterEnd : AfterBegin));
        Join.Ops.assign(Ops.begin() + AfterEnd, Ops.end());
      } else {
        Root.Ops.assign(Ops.begin(), Ops.begin() + AfterBegin);
        Join.Ops.assign(Ops.begin() + AfterBegin, Ops.end());
        if (Case == 2)
          Left.Ops.assign(Ops.begin() + AfterBegin, Ops.begin() + AfterEnd);
        else {
          Left.Preds.push_back(1);
          Left.Succs.push_back(1);
          if (Case == 4)
            Left.Ops.assign(Ops.begin() + AfterBegin, Ops.begin() + AfterEnd);
        }
      }
      if (Reverse)
        std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
      EXPECT_EQ(F.proves(), Case == 0 || Case == 3);
    }
}

TEST(SourceFrameAnalysis, OpaquePointerTaintSurvivesEndMayWritesAndFrameReuse) {
  auto Result = signature(0);
  Result.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Result.ReturnType = NdType::makeInt(8, false);
  std::string Error;
  ASSERT_TRUE(assignDarwinScalarSourceABI(Result, Arch::AArch64, Error));
  for (unsigned Case = 0; Case < 12; ++Case) {
    SCOPED_TRACE(Case);
    AccessLifetimeFixture F;
    F.begin();
    F.begin(32);
    F.end(32);
    F.end();
    if (Case == 1) {
      F.begin(0, 0);
      F.end();
    } // only may overwrite
    if (Case == 2) {
      F.add(NdOp::INT_ADD, SP, {SP, NdVar::scalar(96, 8)});
      F.add(NdOp::INT_SUB, SP, {SP, NdVar::scalar(96, 8)});
    }
    if (Case == 3 || Case == 4) {
      F.address(F.X9, 0);
      F.add(NdOp::STORE, {}, {F.X9, NdVar::scalar(0, Case == 3 ? 1 : 8)});
    }
    if (Case == 5) {
      for (unsigned I : {0, 8, 16}) {
        F.address(F.X9, I);
        F.add(NdOp::STORE, {}, {F.X9, NdVar::scalar(0, 8)});
      }
    }
    if (Case == 8 || Case == 9) {
      SourceFrameEffects Effect;
      if (Case == 8)
        Effect.ReadOnlyFrameParameters[0] = 24;
      else
        Effect.WritableFrameParameters[0] = 24;
      F.address(X0, 0);
      F.call(F.Borrow, Effect);
    }
    F.address(F.X9, 16);
    F.add(NdOp::LOAD, X0, {F.X9});
    if (Case == 6)
      F.add(NdOp::STORE, {}, {F.X20, X0});
    if (Case == 7)
      F.add(NdOp::LOAD, X0, {X0}); // inexact private alias
    if (Case == 10) {
      F.add(NdOp::STORE, {}, {F.X9, NdVar::scalar(0, 8)});
      // A subsequent exact overwrite cannot sanitize the prior loaded value.
    }
    if (Case == 11)
      F.add(NdOp::COPY, X0, {NdVar::scalar(7, 8)});
    F.epilogue();
    EXPECT_EQ(F.proves(&Result), Case == 5);
  }
}

TEST(SourceFrameAnalysis, PrefixQueryRequiresRetainedRecordsAlreadyClosed) {
  for (unsigned Flags : {0, 1, 32, 33})
    for (bool Closed : {false, true}) {
      FrameFixture F;
      AccessLifetimeFixture Protocol;
      const auto X1 = NdVar::reg(a64reg::X1, 8);
      F.prefix({op(NdOp::COPY, X0, {NdVar::scalar(1, 8)}, 0x1024),
                op(NdOp::COPY, X1, {SP}, 0x1028),
                op(NdOp::COPY, Protocol.X2, {NdVar::scalar(Flags, 8)}, 0x102c),
                op(NdOp::COPY, Protocol.X3, {NdVar::scalar(0, 8)}, 0x1030),
                op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x1034)});
      auto Calls = callsAt(0x1034, Protocol.Begin, Protocol.Initialize);
      if (Closed) {
        // prefix() inserts at a fixed index; append end after the begin block.
        auto &Ops = F.Low.Blocks[0].Ops;
        Ops.insert(Ops.begin() + 11,
                   {op(NdOp::COPY, X0, {SP}, 0x1038),
                    op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x103c)});
        Calls.merge(callsAt(0x103c, Protocol.End, Protocol.Finish));
      }
      EXPECT_EQ(bool(F.query(Calls)), Closed || Flags < 32);
    }
}

namespace {
struct RecordCopyFixture {
  LowFunc Low;
  SourceFunctionTypeHint Entry, Producer, Transform, Consumer;
  NativeSourceCalls Calls;
  NativeSourceCallKey Site;
  const NdVar X1 = NdVar::reg(a64reg::X1, 8);
  const NdVar X2 = NdVar::reg(a64reg::X2, 8);
  const NdVar X8 = NdVar::reg(a64reg::X8, 8);
  const NdVar X9 = NdVar::reg(a64reg::X9, 8);
  const NdVar LR = NdVar::reg(a64reg::X30, 8);
  va_t Next = 0x2000;
  RecordCopyFixture() {
    Low.Entry = Next;
    LowBlock B;
    B.Id = 0;
    B.StartAddr = Low.Entry;
    Low.Blocks.push_back(B);
    Entry = signature(0);
    Entry.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
    Producer = signature(0);
    Producer.ReturnType =
        NdType::makeStruct(std::vector<TypeRef>(16, NdType::makeFloat(8)));
    std::string Error;
    EXPECT_TRUE(assignDarwinFixedSourceABI(Producer, Arch::AArch64, Error));
    Transform = Producer;
    Transform.Parameters = {{"input", NdType::makePtr(NdType::makeVoid())}};
    EXPECT_TRUE(assignDarwinFixedSourceABI(Transform, Arch::AArch64, Error));
    Consumer = signature(2);
    Consumer.Parameters.push_back({"record", Producer.ReturnType});
    EXPECT_TRUE(assignDarwinFixedSourceABI(Consumer, Arch::AArch64, Error));
    add(NdOp::INT_SUB, SP, {SP, NdVar::scalar(288, 8)});
    address(X9, 272);
    add(NdOp::STORE, {}, {X9, LR});
    address(X8, 128);
    SourceFrameEffects Write;
    Write.InitializesIndirectResult = true;
    call(Producer, Write);
    address(X8, 0);
    address(X0, 128);
    Write.WritableFrameParameters[0] = 128;
    Write.InitializedFrameParameters.insert(0);
    call(Transform, Write);
    add(NdOp::COPY, X0, {NdVar::scalar(0, 8)});
    add(NdOp::COPY, X1, {NdVar::scalar(0x8000, 8)});
    address(X2, 0);
    SourceFrameEffects Copy;
    Copy.ByValueFrameParameters.insert(2);
    Site = call(Consumer, Copy);
    epilogue();
  }
  void add(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs) {
    Low.Blocks[0].Ops.push_back(op(Opcode, Output, Inputs, Next));
    Next += 4;
  }
  void address(NdVar Output, unsigned Offset) {
    add(NdOp::INT_ADD, Output, {SP, NdVar::scalar(Offset, 8)});
  }
  NativeSourceCallKey call(const SourceFunctionTypeHint &ABI,
                           const SourceFrameEffects &Effects) {
    add(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)});
    const auto Key = *nativeSourceCallKey(Low.Blocks[0].Ops.back());
    Calls.merge(callsAt(Key.Instruction, ABI, Effects));
    return Key;
  }
  void epilogue() {
    address(X9, 272);
    add(NdOp::LOAD, LR, {X9});
    add(NdOp::INT_ADD, SP, {SP, NdVar::scalar(288, 8)});
    add(NdOp::RETURN, {}, {LR});
  }
  auto query() const {
    return sourceFrameByValueCopies(Low, Arch::AArch64, Calls, Site, Entry);
  }
};
} // namespace

TEST(SourceFrameAnalysis, ProvesInitializedCopiesFromIndependentResultEffects) {
  RecordCopyFixture F;
  ASSERT_TRUE(F.query());
  EXPECT_EQ(*F.query(), (std::vector<SourceFrameByValueCopy>{{2, -288, 128}}));
  EXPECT_TRUE(restoresNativeSourceState(F.Low, Arch::AArch64, F.Calls));
  EXPECT_FALSE(
      sourceFrameByValueCopies(F.Low, Arch::X64, F.Calls, F.Site, F.Entry));
  auto Wrong = F.Site;
  ++Wrong.Sequence;
  EXPECT_FALSE(
      sourceFrameByValueCopies(F.Low, Arch::AArch64, F.Calls, Wrong, F.Entry));
  // An effect is independent evidence, not an implication of the return ABI.
  F.Calls.begin()->second.InitializesIndirectResult = false;
  EXPECT_FALSE(F.query());
}

TEST(SourceFrameAnalysis, IncomingResultAddressNeedsCompleteEntryIdentity) {
  RecordCopyFixture F;
  auto Entry = F.Entry;
  Entry.Parameters = {
      {"output",
       NdType::makeInt(8),
       {SourceABICarrierKind::IntegerRegister, a64reg::X8, 0, 8}}};
  const auto Call = op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x1000);
  LowFunc Low;
  Low.Entry = 0x1000;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Low.Entry;
  Block.Ops = {Call, op(NdOp::RETURN, {}, {X0}, 0x1000)};
  Low.Blocks.push_back(Block);
  SourceFrameEffects Effect;
  Effect.InitializesIndirectResult = true;
  auto Calls = callsAt(Call.Addr, F.Producer, Effect);
  std::set<uint64_t> Used;
  ASSERT_TRUE(
      restoresNativeSourceState(Low, Arch::AArch64, Calls, &Used, &Entry));
  EXPECT_EQ(Used, (std::set<uint64_t>{a64reg::X8}));
  Entry.Parameters[0].Type = NdType::makePtr(NdType::makeVoid());
  EXPECT_TRUE(
      restoresNativeSourceState(Low, Arch::AArch64, Calls, nullptr, &Entry));
  EXPECT_FALSE(restoresNativeSourceState(Low, Arch::AArch64, Calls));
  for (unsigned Mutation = 0; Mutation != 10; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Changed = Low;
    auto ABI = Entry;
    auto Contracts = Calls;
    auto &P = ABI.Parameters[0];
    if (Mutation == 0)
      P.Location.RegisterOffset = a64reg::X7;
    if (Mutation == 1)
      P.Type = NdType::makeFloat(8);
    if (Mutation == 2)
      P.Type = NdType::makeInt(4);
    if (Mutation == 3)
      P.Location.ValueBytes = 4;
    if (Mutation == 4)
      ABI.Parameters.clear();
    if (Mutation == 5)
      Contracts.begin()->second.InitializesIndirectResult = false;
    if (Mutation >= 6) {
      const auto Write =
          Mutation == 6   ? op(NdOp::COPY, F.X8, {NdVar::scalar(0, 8)}, 0xffc)
          : Mutation == 7 ? op(NdOp::COPY, NdVar::reg(a64reg::X8, 4),
                               {NdVar::scalar(0, 4)}, 0xffc)
          : Mutation == 8
              ? op(NdOp::INT_ADD, F.X8, {F.X8, NdVar::scalar(8, 8)}, 0xffc)
              : op(NdOp::COPY, F.X8, {SP}, 0xffc);
      Changed.Blocks[0].Ops.insert(Changed.Blocks[0].Ops.begin(), Write);
    }
    EXPECT_FALSE(restoresNativeSourceState(Changed, Arch::AArch64, Contracts,
                                           nullptr, &ABI));
  }
}

TEST(SourceFrameAnalysis, CopyQueryAcceptsCompleteDeclaredEntryABI) {
  RecordCopyFixture F;
  F.Entry.Origin = SourceFunctionTypeHint::OriginKind::SwiftMangled;
  ASSERT_TRUE(F.query());
  for (unsigned Case = 0; Case < 4; ++Case) {
    SCOPED_TRACE(Case);
    auto Entry = F.Entry;
    if (Case == 0)
      Entry.Architecture = Arch::X64;
    if (Case == 1)
      Entry.Parameters.push_back(
          {"missing", NdType::makePtr(NdType::makeVoid())});
    if (Case == 2)
      Entry = F.Producer;
    if (Case == 3)
      Entry = F.Consumer;
    EXPECT_FALSE(
        sourceFrameByValueCopies(F.Low, Arch::AArch64, F.Calls, F.Site, Entry));
  }
}

TEST(SourceFrameAnalysis, CopyEffectsKeepPointerAndRecordPermissionsDistinct) {
  RecordCopyFixture F;
  const auto Valid =
      static_cast<const SourceFrameEffects &>(F.Calls.at(F.Site));
  EXPECT_TRUE(sourceFrameEffectsMatchABI(Valid, F.Consumer));
  for (unsigned Case = 0; Case < 10; ++Case) {
    SCOPED_TRACE(Case);
    auto ABI = F.Consumer;
    auto Effects = Valid;
    if (Case == 0)
      Effects.ByValueFrameParameters = {0};
    if (Case == 1)
      Effects.ByValueFrameParameters = {3};
    if (Case == 2)
      Effects.ReadOnlyFrameParameters[2] = 128;
    if (Case == 3)
      Effects.WritableFrameParameters[2] = 128;
    if (Case == 4)
      Effects.InitializedFrameParameters = {2};
    if (Case == 5)
      Effects.InitializesIndirectResult = true;
    if (Case == 6)
      ABI.Parameters[2].Location.ValueBytes = 4;
    if (Case == 7)
      ABI.Parameters[2].Type = NdType::makePtr(NdType::makeVoid());
    if (Case == 8)
      ABI.Parameters[2].IndirectByValue = false;
    if (Case == 9) {
      ABI.Parameters[2].Location.Kind = SourceABICarrierKind::Stack;
      ABI.Parameters[2].Location.RegisterOffset = 0;
    }
    EXPECT_FALSE(sourceFrameEffectsMatchABI(Effects, ABI));
  }
}

TEST(SourceFrameAnalysis, RejectsPartialExpiredAliasedAndEscapingCopyStorage) {
  for (unsigned Case = 0; Case < 14; ++Case) {
    SCOPED_TRACE(Case);
    RecordCopyFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    auto At = std::find_if(Ops.begin(), Ops.end(), [&](const auto &Op) {
      return nativeSourceCallKey(Op) == F.Site;
    });
    const auto Before = At - Ops.begin();
    std::vector<LowOp> Prefix;
    if (Case < 4) {
      Prefix.push_back(op(NdOp::INT_ADD, F.X2,
                          {SP, NdVar::scalar(Case == 0   ? 1
                                             : Case == 1 ? 8
                                             : Case == 2 ? 192
                                                         : 320,
                                             8)},
                          0x3000));
    } else if (Case == 4) {
      Prefix.push_back(
          op(NdOp::COPY, F.X2, {NdVar::scalar(0x9000, 8)}, 0x3000));
    } else if (Case == 5) {
      Prefix.push_back(op(NdOp::COPY, NdVar::reg(a64reg::X2, 4),
                          {NdVar::scalar(0, 4)}, 0x3000));
    } else if (Case == 6 || Case == 7) {
      Prefix.push_back(
          op(NdOp::STORE, {}, {Case == 6 ? F.X1 : F.X2, SP}, 0x3000));
    } else if (Case == 8) {
      Prefix.push_back(
          op(NdOp::INT_ADD, SP, {SP, NdVar::scalar(16, 8)}, 0x3000));
      Prefix.push_back(
          op(NdOp::INT_SUB, SP, {SP, NdVar::scalar(16, 8)}, 0x3004));
    } else if (Case == 9 || Case == 10) {
      // Another argument can observe an alias even with a bounded borrow.
      F.Calls.at(F.Site).ReadOnlyFrameParameters[0] = 8;
      Prefix.push_back(op(NdOp::INT_ADD, X0,
                          {SP, NdVar::scalar(Case == 9 ? 0 : 120, 8)}, 0x3000));
    } else if (Case == 11) {
      F.Calls.at(F.Site).ByValueFrameParameters.clear();
    } else if (Case == 12) {
      // One definite word cannot replace a complete 128-byte producer.
      Ops[4] = op(NdOp::STORE, {}, {F.X8, NdVar::scalar(0, 8)}, 0x2010);
    } else {
      Prefix.push_back(op(NdOp::CALL, X0, {NdVar::cst(0x9000, 8)}, 0x3000));
    }
    Ops.insert(Ops.begin() + Before, Prefix.begin(), Prefix.end());
    EXPECT_FALSE(F.query());
  }
}

TEST(SourceFrameAnalysis, ConsumedCopiesNeedNewWritesBeforeLaterReads) {
  for (unsigned Case = 0; Case < 5; ++Case) {
    SCOPED_TRACE(Case);
    RecordCopyFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    auto At = std::find_if(Ops.begin(), Ops.end(), [&](const auto &Op) {
      return nativeSourceCallKey(Op) == F.Site;
    });
    std::vector<LowOp> After{op(NdOp::COPY, F.X9, {SP}, 0x3000)};
    if (Case == 1 || Case == 2)
      After.push_back(op(NdOp::STORE, {},
                         {F.X9, NdVar::scalar(5, Case == 1 ? 4 : 8)}, 0x3004));
    if (Case != 3)
      After.push_back(op(NdOp::LOAD, X0, {F.X9}, 0x3008));
    if (Case == 4)
      After.push_back(op(NdOp::STORE, {}, {F.X9, NdVar::scalar(5, 8)}, 0x300c));
    Ops.insert(At + 1, After.begin(), After.end());
    EXPECT_EQ(bool(F.query()), Case == 2 || Case == 3);
  }
}

TEST(SourceFrameAnalysis, DefiniteResultsRespectSavedStateAndActiveExtent) {
  for (unsigned Offset : {1, 192, 272, 288}) {
    RecordCopyFixture F;
    F.Low.Blocks[0].Ops[3].Inputs[1] = NdVar::scalar(Offset, 8);
    EXPECT_FALSE(F.query()) << Offset;
  }
}

TEST(SourceFrameAnalysis, CopyInitializationIsAMustFactAcrossEveryPredecessor) {
  for (unsigned Case = 0; Case < 6; ++Case)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Case);
      RecordCopyFixture F;
      const auto Original = F.Low.Blocks[0].Ops;
      F.Low.Blocks.resize(4);
      auto &Root = F.Low.Blocks[0], &Left = F.Low.Blocks[1],
           &Right = F.Low.Blocks[2], &Join = F.Low.Blocks[3];
      for (unsigned I = 0; I < 4; ++I) {
        F.Low.Blocks[I].Id = I;
        F.Low.Blocks[I].StartAddr = 0x2000 + I * 0x100;
      }
      Root.Ops.assign(Original.begin(), Original.begin() + 8);
      Root.Succs = {1, 2};
      Left.Preds = Right.Preds = {0};
      Left.Succs = Right.Succs = {3};
      Join.Preds = {1, 2};
      Join.Ops.assign(Original.begin() + 8, Original.end());
      Left.Ops =
          Right.Ops = {op(NdOp::COPY, F.X9, {NdVar::scalar(0, 8)}, 0x3000)};
      auto Borrow = signature(1);
      if (Case) {
        SourceFrameEffects Effects;
        if (Case == 1)
          Effects.ReadOnlyFrameParameters[0] = 8;
        else
          Effects.WritableFrameParameters[0] = 8;
        Left.Ops = {op(NdOp::INT_ADD, X0,
                       {SP, NdVar::scalar(Case == 5 ? 128 : 0, 8)}, 0x3000),
                    op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x3004)};
        F.Calls.merge(callsAt(0x3004, Borrow, Effects));
        if (Case == 3 || Case == 4) {
          Left.Ops.push_back(op(NdOp::COPY, F.X9, {SP}, 0x3008));
          Left.Ops.push_back(op(NdOp::STORE, {},
                                {F.X9, NdVar::scalar(7, Case == 3 ? 8 : 4)},
                                0x300c));
        }
      }
      if (Reverse)
        std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
      EXPECT_EQ(bool(F.query()), Case != 2 && Case != 4);
    }
}

TEST(SourceFrameAnalysis,
     CopyInitializationIncludesConsumptionOnLoopBackedges) {
  for (bool Reinitialize : {false, true})
    for (bool Reverse : {false, true}) {
      RecordCopyFixture F;
      const auto Original = F.Low.Blocks[0].Ops;
      F.Low.Blocks.resize(3);
      auto &Root = F.Low.Blocks[0], &Loop = F.Low.Blocks[1],
           &Exit = F.Low.Blocks[2];
      for (unsigned I = 0; I < 3; ++I) {
        F.Low.Blocks[I].Id = I;
        F.Low.Blocks[I].StartAddr = 0x2000 + I * 0x100;
      }
      Root.Ops.assign(Original.begin(), Original.begin() + 8);
      Root.Succs = {1};
      Loop.Preds = {0, 1};
      Loop.Succs = {1, 2};
      Loop.Ops.assign(Original.begin() + 8, Original.begin() + 12);
      if (Reinitialize) {
        std::vector<LowOp> Prefix(Original.begin() + 3, Original.begin() + 8);
        for (size_t I = 0; I < Prefix.size(); ++I) {
          const auto OldSite = nativeSourceCallKey(Prefix[I]);
          Prefix[I].Addr = 0x3000 + I * 4;
          if (OldSite)
            F.Calls.emplace(*nativeSourceCallKey(Prefix[I]),
                            F.Calls.at(*OldSite));
        }
        Loop.Ops.insert(Loop.Ops.begin(), Prefix.begin(), Prefix.end());
      }
      Exit.Preds = {1};
      Exit.Ops.assign(Original.begin() + 12, Original.end());
      if (Reverse)
        std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
      EXPECT_EQ(bool(F.query()), Reinitialize);
    }
}

TEST(SourceFrameAnalysis,
     CopyAndResultEffectsPreserveRetainedScratchLifetimes) {
  for (unsigned ScratchOffset : {0, 128, 288}) {
    RecordCopyFixture F;
    AccessLifetimeFixture Protocol;
    auto &Ops = F.Low.Blocks[0].Ops;
    for (auto &Op : Ops) {
      if (Op.Output == SP && Op.NumInputs == 2 && Op.Inputs[1].isConst() &&
          Op.Inputs[1].Offset == 288)
        Op.Inputs[1] = NdVar::scalar(384, 8);
      if (Op.Output == F.X9 && Op.Opcode == NdOp::INT_ADD &&
          Op.Inputs[1].Offset == 272)
        Op.Inputs[1] = NdVar::scalar(368, 8);
    }
    Ops.insert(
        Ops.begin() + 3,
        {op(NdOp::COPY, X0, {NdVar::scalar(0x6000, 8)}, 0x3100),
         op(NdOp::INT_ADD, F.X1, {SP, NdVar::scalar(ScratchOffset, 8)}, 0x3104),
         op(NdOp::COPY, F.X2, {NdVar::scalar(32, 8)}, 0x3108),
         op(NdOp::COPY, Protocol.X3, {NdVar::scalar(0, 8)}, 0x310c),
         op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x3110)});
    F.Calls.merge(callsAt(0x3110, Protocol.Begin, Protocol.Initialize));
    auto After = std::find_if(Ops.begin(), Ops.end(), [&](const auto &Op) {
      return nativeSourceCallKey(Op) == F.Site;
    });
    Ops.insert(
        After + 1,
        {op(NdOp::INT_ADD, X0, {SP, NdVar::scalar(ScratchOffset, 8)}, 0x3120),
         op(NdOp::CALL, X0, {NdVar::cst(0x4000, 8)}, 0x3124)});
    F.Calls.merge(callsAt(0x3124, Protocol.End, Protocol.Finish));
    EXPECT_EQ(bool(F.query()), ScratchOffset == 288);
  }
}

namespace {
struct OpaqueValueFixture {
  Arch Architecture;
  LowFunc Low;
  SourceFunctionTypeHint Producer, Consumer;
  NativeSourceCalls Calls;
  NativeSourceCallKey Produce, Read, Destroy;
  NdVar Stack, A0, A1, Address, Link;
  unsigned FrameBytes;
  size_t StartProducer, StartRead, StartDestroy, StartExit;
  va_t Next = 0x4000;

  explicit OpaqueValueFixture(Arch Architecture = Arch::AArch64)
      : Architecture(Architecture) {
    const auto &TRI = getTargetRegInfo(Architecture);
    Stack = NdVar::reg(TRI.StackPointer, 8);
    Address =
        NdVar::reg(Architecture == Arch::AArch64 ? a64reg::X9 : x86reg::R10, 8);
    Link = NdVar::reg(TRI.LinkRegister, 8);
    FrameBytes = Architecture == Arch::AArch64 ? 96 : 88;
    Producer.Origin = SourceFunctionTypeHint::OriginKind::DarwinRuntime;
    Producer.ReturnType = NdType::makeVoid();
    Producer.Parameters = {{"source", NdType::makePtr(NdType::makeVoid())},
                           {"output", NdType::makePtr(NdType::makeVoid())}};
    std::string Error;
    EXPECT_TRUE(assignDarwinScalarSourceABI(Producer, Architecture, Error));
    Consumer = Producer;
    Consumer.Parameters.resize(1);
    EXPECT_TRUE(assignDarwinScalarSourceABI(Consumer, Architecture, Error));
    A0 = NdVar::reg(Producer.Parameters[0].Location.RegisterOffset, 8);
    A1 = NdVar::reg(Producer.Parameters[1].Location.RegisterOffset, 8);
    Low.Entry = Next;
    LowBlock B;
    B.Id = 0;
    B.StartAddr = Next;
    Low.Blocks.push_back(B);
    add(NdOp::INT_SUB, Stack, {Stack, NdVar::scalar(FrameBytes, 8)});
    if (TRI.LinkRegister) {
      address(Address, 80);
      add(NdOp::STORE, {}, {Address, Link});
    }
    StartProducer = Low.Blocks[0].Ops.size();
    add(NdOp::COPY, A0, {NdVar::scalar(0x9000, 8)});
    address(A1, 8);
    SourceFrameEffects Output;
    Output.WritableFrameParameters[1] = 40;
    Output.OpaqueValueParameters[1] = {
        SourceFrameValueEffect::Action::Initialize, "opaque-test-value", 40};
    Produce = call(Producer, Output);
    StartRead = Low.Blocks[0].Ops.size();
    address(A0, 8);
    SourceFrameEffects Input;
    Input.ReadOnlyFrameParameters[0] = 40;
    Input.OpaqueValueParameters[0] = {SourceFrameValueEffect::Action::Read,
                                      "opaque-test-value", 40};
    Read = call(Consumer, Input);
    StartDestroy = Low.Blocks[0].Ops.size();
    address(A0, 8);
    Input.ReadOnlyFrameParameters.clear();
    Input.WritableFrameParameters[0] = 40;
    Input.OpaqueValueParameters[0].TheAction =
        SourceFrameValueEffect::Action::Destroy;
    Destroy = call(Consumer, Input);
    StartExit = Low.Blocks[0].Ops.size();
    if (TRI.LinkRegister) {
      address(Address, 80);
      add(NdOp::LOAD, Link, {Address});
    }
    add(NdOp::INT_ADD, Stack, {Stack, NdVar::scalar(FrameBytes, 8)});
    add(NdOp::RETURN, {}, {});
    if (TRI.LinkRegister)
      Low.Blocks[0].Ops.back().addInput(Link);
  }
  void add(NdOp Code, NdVar Out, std::initializer_list<NdVar> Inputs) {
    Low.Blocks[0].Ops.push_back(op(Code, Out, Inputs, Next));
    Next += 4;
  }
  void address(NdVar Out, unsigned Offset) {
    add(NdOp::INT_ADD, Out, {Stack, NdVar::scalar(Offset, 8)});
  }
  NativeSourceCallKey call(const SourceFunctionTypeHint &ABI,
                           const SourceFrameEffects &Effects) {
    add(NdOp::CALL, {}, {NdVar::cst(0x5000, 8)});
    const auto Key = *nativeSourceCallKey(Low.Blocks[0].Ops.back());
    NativeSourceCallContract Contract;
    Contract.Signature = &ABI;
    static_cast<SourceFrameEffects &>(Contract) = Effects;
    Calls.emplace(Key, Contract);
    return Key;
  }
  bool valid() const {
    return restoresNativeSourceState(Low, Architecture, Calls);
  }
};
} // namespace

TEST(SourceFrameAnalysis, OpaqueValueLifecycleDoesNotInitializePadding) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    OpaqueValueFixture F(Architecture);
    ASSERT_TRUE(F.valid());
    auto &Output = F.Calls.at(F.Produce);
    Output.OpaqueValueParameters.clear();
    EXPECT_FALSE(F.valid());
    Output.OpaqueValueParameters[1] = {
        SourceFrameValueEffect::Action::Initialize, "opaque-test-value", 40};
    // A value read is valid while an arbitrary raw byte read is not.
    F.Calls.at(F.Read).OpaqueValueParameters.clear();
    F.Calls.at(F.Read).InitializedFrameParameters.insert(0);
    EXPECT_FALSE(F.valid());
  }
}

TEST(SourceFrameAnalysis,
     OpaqueValueEffectsValidateIdentityAndWholePointerABI) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Case = 0; Case < 14; ++Case) {
      SCOPED_TRACE(Case);
      OpaqueValueFixture F(Architecture);
      auto ABI = F.Producer;
      auto Effect = static_cast<SourceFrameEffects &>(F.Calls.at(F.Produce));
      ASSERT_TRUE(sourceFrameEffectsMatchABI(Effect, ABI));
      if (Case == 0)
        Effect.WritableFrameParameters.clear();
      if (Case == 1) {
        Effect.WritableFrameParameters.clear();
        Effect.ReadOnlyFrameParameters[1] = 40;
      }
      if (Case == 2)
        Effect.OpaqueValueParameters[2] = Effect.OpaqueValueParameters.at(1);
      if (Case == 3)
        Effect.WritableFrameParameters[1] = 0;
      if (Case == 4)
        Effect.WritableFrameParameters[1] = (1U << 20) + 1;
      if (Case == 5)
        ABI.Parameters[1].Location.ValueBytes = 4;
      if (Case == 6)
        ABI.Parameters[1].Type = NdType::makeInt(4);
      if (Case == 7)
        ABI.Parameters[1].Type = NdType::makeFloat(8);
      if (Case == 8)
        ABI.Parameters[1].Location.Kind =
            SourceABICarrierKind::FloatingRegister;
      if (Case == 9)
        Effect.OpaqueValueParameters.at(1).Identity.clear();
      if (Case == 10)
        Effect.Scratch = {SourceFrameScratchEffect::Domain::SwiftAccess,
                          SourceFrameScratchEffect::Action::Initialize,
                          1,
                          40,
                          std::nullopt,
                          {}};
      if (Case == 11)
        ABI.Parameters[1].Location.RegisterOffset =
            ABI.Parameters[0].Location.RegisterOffset;
      if (Case == 12)
        Effect.OpaqueValueParameters.at(1).TheAction =
            SourceFrameValueEffect{}.TheAction;
      if (Case == 13)
        Effect.OpaqueValueParameters.at(1).Identity = std::string(257, 'x');
      EXPECT_FALSE(sourceFrameEffectsMatchABI(Effect, ABI));
    }
}

TEST(SourceFrameAnalysis, OpaqueValuesKeepExtentAliasAndRestorationChecks) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Case = 0; Case < 13; ++Case) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Case);
      OpaqueValueFixture F(Architecture);
      auto &Ops = F.Low.Blocks[0].Ops;
      auto &Effect = F.Calls.at(F.Produce);
      std::vector<LowOp> Before, After;
      if (Case == 0 || Case == 1)
        Ops[F.StartProducer + 1].Inputs[1] =
            NdVar::scalar(Case == 0 ? F.FrameBytes - 8 : F.FrameBytes, 8);
      if (Case == 2)
        Ops[F.StartProducer + 1].Output.Size = 4;
      if (Case == 3)
        Ops[F.StartProducer + 1] =
            op(NdOp::COPY, F.A1, {NdVar::scalar(0x9000, 8)}, 0x5000);
      if (Case == 4 || Case == 5) {
        Effect.ReadOnlyFrameParameters[0] = 40;
        Ops[F.StartProducer] =
            op(NdOp::INT_ADD, F.A0,
               {F.Stack, NdVar::scalar(Case == 4 ? 8 : 40, 8)}, 0x5000);
      }
      if (Case == 6)
        Before.push_back(op(NdOp::STORE, {}, {F.A0, F.A1}, 0x5000));
      if (Case == 7) {
        const auto Saved = NdVar::reg(
            Architecture == Arch::AArch64 ? a64reg::X19 : x86reg::RBX, 8);
        Before = {op(NdOp::STORE, {}, {F.A1, Saved}, 0x5000),
                  op(NdOp::COPY, Saved, {NdVar::scalar(0, 8)}, 0x5004)};
        After = {op(NdOp::INT_ADD, F.Address, {F.Stack, NdVar::scalar(8, 8)},
                    0x5010),
                 op(NdOp::LOAD, Saved, {F.Address}, 0x5014)};
      }
      if (Case == 8)
        After = {
            op(NdOp::INT_ADD, F.Stack, {F.Stack, NdVar::scalar(48, 8)}, 0x5010),
            op(NdOp::INT_SUB, F.Stack, {F.Stack, NdVar::scalar(48, 8)},
               0x5014)};
      if (Case == 9) {
        After = {
            op(NdOp::INT_ADD, F.A0, {F.Stack, NdVar::scalar(8, 8)}, 0x5010),
            op(NdOp::CALL, {}, {NdVar::cst(0x5000, 8)}, 0x5014)};
        auto MayWrite = F.Calls.at(F.Destroy);
        MayWrite.InitializedFrameParameters.clear();
        MayWrite.OpaqueValueParameters.clear();
        F.Calls.emplace(*nativeSourceCallKey(After.back()), MayWrite);
      }
      if (Case == 10) {
        F.Producer.ReturnType = NdType::makePtr(NdType::makeVoid());
        std::string Error;
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(F.Producer, Architecture, Error));
        Effect.ReturnFrameOrExternal = {1, 40};
        After = {op(NdOp::COPY, F.A1,
                    {NdVar::reg(F.Producer.ReturnLocation.RegisterOffset, 8)},
                    0x5010),
                 op(NdOp::COPY, F.A0, {NdVar::scalar(0x9000, 8)}, 0x5014),
                 op(NdOp::CALL, {}, {NdVar::cst(0x5000, 8)}, 0x5018)};
        F.Calls.emplace(*nativeSourceCallKey(After.back()), Effect);
      }
      if (Case == 11) {
        Before.push_back(op(
            NdOp::COPY, F.Stack,
            {NdVar::reg(
                Architecture == Arch::AArch64 ? a64reg::X12 : x86reg::R11, 8)},
            0x5000));
      }
      if (Case == 12) {
        // A missing producer on the first path cannot be repaired by a read.
        Effect.OpaqueValueParameters.clear();
        Before.push_back(
            op(NdOp::STORE, {}, {F.A1, NdVar::scalar(0, 8)}, 0x5000));
      }
      Ops.insert(Ops.begin() + F.StartRead, After.begin(), After.end());
      Ops.insert(Ops.begin() + F.StartRead - 1, Before.begin(), Before.end());
      EXPECT_FALSE(F.valid());
    }
}

TEST(SourceFrameAnalysis, OpaqueValueDestructionDoesNotInitializeBytes) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Bytes : {0, 39, 40}) {
      OpaqueValueFixture F(Architecture);
      auto &Ops = F.Low.Blocks[0].Ops;
      std::vector<LowOp> After;
      for (unsigned I = 0; I < Bytes; ++I) {
        After.push_back(op(NdOp::INT_ADD, F.Address,
                           {F.Stack, NdVar::scalar(8 + I, 8)}, 0x5000 + I * 8));
        After.push_back(op(NdOp::STORE, {}, {F.Address, NdVar::scalar(I, 1)},
                           0x5004 + I * 8));
      }
      After.push_back(
          op(NdOp::INT_ADD, F.A0, {F.Stack, NdVar::scalar(8, 8)}, 0x6000));
      After.push_back(op(NdOp::CALL, {}, {NdVar::cst(0x5000, 8)}, 0x6004));
      auto RawRead = F.Calls.at(F.Read);
      RawRead.OpaqueValueParameters.clear();
      RawRead.InitializedFrameParameters.insert(0);
      F.Calls.emplace(*nativeSourceCallKey(After.back()), RawRead);
      Ops.insert(Ops.begin() + F.StartExit, After.begin(), After.end());
      EXPECT_EQ(F.valid(), Bytes == 40);
    }
}

TEST(SourceFrameAnalysis, OpaqueValueLifetimesMeetAllPathsAndLoopIterations) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Case = 0; Case < 5; ++Case)
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(Case);
        SCOPED_TRACE(Reverse);
        OpaqueValueFixture F(Architecture);
        const auto Original = F.Low.Blocks[0].Ops;
        F.Low.Blocks.resize(4);
        for (unsigned I = 0; I < 4; ++I) {
          F.Low.Blocks[I] = {};
          F.Low.Blocks[I].Id = I;
          F.Low.Blocks[I].StartAddr = 0x4000 + I * 0x100;
        }
        auto &Root = F.Low.Blocks[0], &Left = F.Low.Blocks[1],
             &Right = F.Low.Blocks[2], &Join = F.Low.Blocks[3];
        Root.Ops.assign(Original.begin(), Original.begin() + F.StartProducer);
        Left.Ops.assign(Original.begin() + F.StartProducer,
                        Original.begin() + F.StartRead);
        if (Case < 2) {
          Root.Succs = {1, 2};
          Left.Preds = Right.Preds = {0};
          Left.Succs = Right.Succs = {3};
          Join.Preds = {1, 2};
          Join.Ops.assign(Original.begin() + F.StartRead, Original.end());
          Right.Ops = Left.Ops;
          for (auto &Op : Right.Ops) {
            const auto Key = nativeSourceCallKey(Op);
            Op.Addr += 0x1000;
            if (Key) {
              F.Calls.emplace(*nativeSourceCallKey(Op), F.Calls.at(*Key));
              if (Case == 1)
                F.Calls.at(*nativeSourceCallKey(Op))
                    .OpaqueValueParameters.clear();
            }
          }
        } else {
          Root.Succs = {1,
                        3}; // The zero-iteration exit never reads the buffer.
          Left.Preds = {0};
          Left.Succs = {2};
          Right.Preds = {1, 2};
          Right.Succs = {2, 3};
          Right.Ops.assign(Original.begin() +
                               (Case == 2 ? F.StartRead : F.StartProducer),
                           Original.begin() + F.StartExit);
          // Loop producers need their own occurrence, not the preloop key.
          for (auto &Op : Right.Ops) {
            const auto Key = nativeSourceCallKey(Op);
            Op.Addr += 0x1000;
            if (Key)
              F.Calls.emplace(*nativeSourceCallKey(Op), F.Calls.at(*Key));
          }
          if (Case == 3)
            Left.Ops.clear();
          if (Case == 4) {
            // A late write after a read cannot initialize the first iteration.
            Left.Ops.clear();
            auto Producer =
                std::vector<LowOp>(Right.Ops.begin(), Right.Ops.begin() + 3);
            Right.Ops.erase(Right.Ops.begin(), Right.Ops.begin() + 3);
            Right.Ops.insert(Right.Ops.end(), Producer.begin(), Producer.end());
          }
          if (Left.Ops.empty())
            Left.Ops.push_back(
                op(NdOp::COPY, F.Address, {NdVar::scalar(0, 8)}, 0x7000));
          Join.Preds = {0, 2};
          Join.Ops.assign(Original.begin() + F.StartExit, Original.end());
        }
        if (Reverse)
          std::reverse(F.Low.Blocks.begin(), F.Low.Blocks.end());
        EXPECT_EQ(F.valid(), Case == 0 || Case == 3);
      }
}

TEST(SourceFrameAnalysis,
     OpaqueLifetimesRejectMissingStaleAndDuplicateOperations) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Case = 0; Case < 12; ++Case) {
      SCOPED_TRACE(Case);
      OpaqueValueFixture F(Architecture);
      auto &Ops = F.Low.Blocks[0].Ops;
      if (Case == 0)
        F.Calls.at(F.Read).OpaqueValueParameters.at(0).Identity = "other-value";
      if (Case == 1)
        F.Calls.at(F.Destroy).OpaqueValueParameters.at(0).Identity =
            "other-value";
      if (Case == 2)
        F.Calls.at(F.Read).OpaqueValueParameters.clear();
      if (Case == 3)
        F.Calls.at(F.Destroy).OpaqueValueParameters.clear();
      if (Case == 4)
        Ops.erase(Ops.begin() + F.StartDestroy, Ops.begin() + F.StartExit);
      if (Case == 5)
        Ops.erase(Ops.begin() + F.StartProducer, Ops.begin() + F.StartRead);
      if (Case == 6 || Case == 7 || Case == 8) {
        std::vector<LowOp> Extra;
        if (Case == 6)
          Extra.assign(Ops.begin() + F.StartProducer,
                       Ops.begin() + F.StartRead);
        else
          Extra.assign(Ops.begin() + (Case == 7 ? F.StartRead : F.StartDestroy),
                       Ops.begin() +
                           (Case == 7 ? F.StartDestroy : F.StartExit));
        for (auto &Op : Extra) {
          const auto Key = nativeSourceCallKey(Op);
          Op.Addr += 0x1000;
          if (Key)
            F.Calls.emplace(*nativeSourceCallKey(Op), F.Calls.at(*Key));
        }
        Ops.insert(Ops.begin() + (Case == 6 ? F.StartRead : F.StartExit),
                   Extra.begin(), Extra.end());
      }
      if (Case == 9 || Case == 10) {
        const std::vector<LowOp> Raw = {
            op(NdOp::INT_ADD, F.Address, {F.Stack, NdVar::scalar(8, 8)},
               0x6000),
            Case == 9 ? op(NdOp::LOAD, F.A0, {F.Address}, 0x6004)
                      : op(NdOp::STORE, {}, {F.Address, NdVar::scalar(0, 1)},
                           0x6004)};
        Ops.insert(Ops.begin() + F.StartRead, Raw.begin(), Raw.end());
      }
      if (Case == 11) {
        // A result that may alias the destroyed frame must not hide the read.
        F.Consumer.ReturnType = NdType::makePtr(NdType::makeVoid());
        std::string Error;
        ASSERT_TRUE(
            assignDarwinScalarSourceABI(F.Consumer, Architecture, Error));
        F.Calls.at(F.Destroy).ReturnFrameOrExternal = {0, 40};
        Ops.insert(Ops.begin() + F.StartExit,
                   op(NdOp::LOAD, F.Address,
                      {NdVar::reg(F.Consumer.ReturnLocation.RegisterOffset, 8)},
                      0x6000));
      }
      EXPECT_FALSE(F.valid());
    }
}

TEST(SourceFrameAnalysis, OpaqueValuesRetainScratchAndPaddingTaintRules) {
  for (unsigned Case = 0; Case < 4; ++Case) {
    SCOPED_TRACE(Case);
    OpaqueValueFixture F;
    AccessLifetimeFixture Protocol;
    auto &Ops = F.Low.Blocks[0].Ops;
    const unsigned Offset = Case == 0 ? 48 : 8;
    std::vector<LowOp> Prefix = {
        op(NdOp::COPY, X0, {NdVar::scalar(0x9000, 8)}, 0x6000),
        op(NdOp::INT_ADD, F.A1, {F.Stack, NdVar::scalar(Offset, 8)}, 0x6004),
        op(NdOp::COPY, Protocol.X2, {NdVar::scalar(32, 8)}, 0x6008),
        op(NdOp::COPY, Protocol.X3, {NdVar::scalar(0, 8)}, 0x600c),
        op(NdOp::CALL, {}, {NdVar::cst(0x4000, 8)}, 0x6010)};
    F.Calls.merge(callsAt(0x6010, Protocol.Begin, Protocol.Initialize));
    std::vector<LowOp> Finish = {
        op(NdOp::INT_ADD, X0, {F.Stack, NdVar::scalar(Offset, 8)}, 0x6020),
        op(NdOp::CALL, {}, {NdVar::cst(0x4000, 8)}, 0x6024)};
    F.Calls.merge(callsAt(0x6024, Protocol.End, Protocol.Finish));
    if (Case >= 2) {
      Prefix.insert(Prefix.end(), Finish.begin(), Finish.end());
      if (Case == 3)
        for (unsigned I = 0; I < 24; I += 8) {
          Prefix.push_back(op(NdOp::INT_ADD, F.Address,
                              {F.Stack, NdVar::scalar(Offset + I, 8)},
                              0x6100 + I));
          Prefix.push_back(op(NdOp::STORE, {}, {F.Address, NdVar::scalar(0, 8)},
                              0x6200 + I));
        }
    } else {
      Ops.insert(Ops.begin() + F.StartExit, Finish.begin(), Finish.end());
    }
    Ops.insert(Ops.begin() + F.StartProducer, Prefix.begin(), Prefix.end());
    // Ending a linked scratch lifetime does not erase pointers in padding.
    EXPECT_EQ(F.valid(), Case == 0 || Case == 3);
  }
}

TEST(SourceFrameAnalysis,
     OpaqueReadAllowsSameValueButWritesRequireDisjointness) {
  for (unsigned Case = 0; Case < 3; ++Case) {
    OpaqueValueFixture F;
    auto &Ops = F.Low.Blocks[0].Ops;
    auto Effect = F.Calls.at(F.Read);
    Effect.Signature = &F.Producer;
    Effect.ReadOnlyFrameParameters[1] = 40;
    Effect.OpaqueValueParameters[1] = Effect.OpaqueValueParameters.at(0);
    std::vector<LowOp> Extra = {
        op(NdOp::INT_ADD, F.A0, {F.Stack, NdVar::scalar(8, 8)}, 0x6000),
        op(NdOp::COPY, F.A1, {F.A0}, 0x6004),
        op(NdOp::CALL, {}, {NdVar::cst(0x5000, 8)}, 0x6008)};
    if (Case) {
      Effect.ReadOnlyFrameParameters.erase(1);
      Effect.WritableFrameParameters[1] = 40;
      Effect.OpaqueValueParameters[1].TheAction =
          Case == 1 ? SourceFrameValueEffect::Action::Initialize
                    : SourceFrameValueEffect::Action::Destroy;
    }
    F.Calls.emplace(*nativeSourceCallKey(Extra.back()), Effect);
    Ops.insert(Ops.begin() + F.StartRead, Extra.begin(), Extra.end());
    EXPECT_EQ(F.valid(), Case == 0);
  }
}

namespace {
struct SwiftErrorStateFixture {
  Arch Architecture;
  SourceFunctionTypeHint Signature;
  LowFunc Function;
  NativeSourceCalls Calls;
  NdVar Error, Stack;
  SwiftErrorStateFixture(Arch Architecture) : Architecture(Architecture) {
    const auto &TRI = getTargetRegInfo(Architecture);
    Error = NdVar::reg(
        Architecture == Arch::AArch64 ? a64reg::X21 : x86reg::R12, 8);
    Stack = NdVar::reg(TRI.StackPointer, 8);
    const auto Pointer = NdType::makePtr(NdType::makeVoid());
    Signature.ReturnType = NdType::makeVoid();
    Signature.Parameters = {{"context", Pointer},
                            {"error", NdType::makePtr(Pointer)}};
    Signature.Parameters[0].TheRole =
        SourceParameterTypeHint::Role::SwiftContext;
    Signature.Parameters[1].TheRole =
        SourceParameterTypeHint::Role::SwiftErrorResult;
    std::string Diagnostic;
    EXPECT_TRUE(
        assignDarwinSwiftSourceABI(Signature, Architecture, Diagnostic));
    Function.Entry = 0x1000;
    LowBlock B;
    B.Id = 0;
    B.StartAddr = Function.Entry;
    const auto Frame =
        NdVar::scalar(Architecture == Arch::AArch64 ? 32 : 24, 8);
    B.Ops = {op(NdOp::INT_SUB, Stack, {Stack, Frame}, 0x1000),
             op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)}, 0x1010),
             op(NdOp::INT_ADD, Stack, {Stack, Frame}, 0x1020),
             op(NdOp::RETURN, {}, {}, 0x1024)};
    if (TRI.LinkRegister) {
      const auto Link = NdVar::reg(TRI.LinkRegister, 8);
      B.Ops.insert(B.Ops.begin() + 1,
                   op(NdOp::STORE, {}, {Stack, Link}, 0x1004));
      B.Ops.insert(B.Ops.end() - 2, op(NdOp::LOAD, Link, {Stack}, 0x101c));
      B.Ops.back().addInput(Link);
    }
    NativeSourceCallContract Call;
    Call.Signature = &Signature;
    Calls.emplace(*nativeSourceCallKey(*std::find_if(
                      B.Ops.begin(), B.Ops.end(),
                      [](const LowOp &O) { return O.Opcode == NdOp::CALL; })),
                  Call);
    Function.Blocks.push_back(std::move(B));
  }
  bool restores() const {
    return restoresNativeSourceState(Function, Architecture, Calls);
  }
};
} // namespace

TEST(SourceFrameAnalysis, SwiftErrorOutputDoesNotInheritCalleeSaveIdentity) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftErrorStateFixture F(Architecture);
    EXPECT_FALSE(F.restores());
    // A real private save and complete restoration still closes the state.
    auto &Ops = F.Function.Blocks.front().Ops;
    const auto Address = NdVar::tmp(80, 8);
    auto Call = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &O) {
      return O.Opcode == NdOp::CALL;
    });
    Ops.insert(Call, {op(NdOp::INT_ADD, Address, {F.Stack, NdVar::scalar(8, 8)},
                         0x1008),
                      op(NdOp::STORE, {}, {Address, F.Error}, 0x1008)});
    Ops.insert(Ops.end() - 2, {op(NdOp::INT_ADD, Address,
                                  {F.Stack, NdVar::scalar(8, 8)}, 0x101e),
                               op(NdOp::LOAD, F.Error, {Address}, 0x101e)});
    EXPECT_TRUE(F.restores());
    (Ops.end() - 3)->Output.Size = 4;
    EXPECT_FALSE(F.restores());
  }
}

TEST(SourceFrameAnalysis, SwiftErrorTailOutputIsNotAnUnchangedLeaf) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftErrorStateFixture F(Architecture);
    auto &Ops = F.Function.Blocks.front().Ops;
    const auto Call = *std::find_if(Ops.begin(), Ops.end(), [](const LowOp &O) {
      return O.Opcode == NdOp::CALL;
    });
    Ops = {Call, op(NdOp::RETURN, {}, {}, Call.Addr)};
    if (getTargetRegInfo(Architecture).LinkRegister)
      Ops.back().addInput(NdVar::reg(a64reg::X30, 8));
    EXPECT_FALSE(
        preservesNativeSourceLeafState(F.Function, Architecture, F.Calls));
    EXPECT_FALSE(F.restores());
  }
}

TEST(SourceFrameAnalysis, SwiftLogicalErrorSlotIsNotPhysicalBorrowedMemory) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftErrorStateFixture F(Architecture);
    for (bool Writable : {false, true}) {
      SourceFrameEffects Effects;
      auto &Borrow = Writable ? Effects.WritableFrameParameters
                              : Effects.ReadOnlyFrameParameters;
      Borrow.emplace(1, 8);
      EXPECT_FALSE(sourceFrameEffectsMatchABI(Effects, F.Signature));
      // The ordinary pointer role retains the established independent
      // memory-effect contract; the logical error-slot address is special.
      F.Signature.Parameters[1].TheRole =
          SourceParameterTypeHint::Role::Ordinary;
      std::string Diagnostic;
      ASSERT_TRUE(
          assignDarwinSwiftSourceABI(F.Signature, Architecture, Diagnostic));
      EXPECT_TRUE(sourceFrameEffectsMatchABI(Effects, F.Signature));
      F.Signature.Parameters[1].TheRole =
          SourceParameterTypeHint::Role::SwiftErrorResult;
      ASSERT_TRUE(
          assignDarwinSwiftSourceABI(F.Signature, Architecture, Diagnostic));
    }
  }
}

TEST(SourceFrameAnalysis, UnchangedErrorResultRequiresAnExplicitContract) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftErrorStateFixture F(Architecture);
    F.Calls.begin()->second.PreservesSwiftErrorResult = true;
    EXPECT_TRUE(F.restores());
    auto &Ops = F.Function.Blocks.front().Ops;
    const auto Call = *std::find_if(Ops.begin(), Ops.end(), [](const LowOp &O) {
      return O.Opcode == NdOp::CALL;
    });
    Ops = {Call, op(NdOp::RETURN, {}, {}, Call.Addr)};
    if (getTargetRegInfo(Architecture).LinkRegister)
      Ops.back().addInput(NdVar::reg(a64reg::X30, 8));
    EXPECT_TRUE(
        preservesNativeSourceLeafState(F.Function, Architecture, F.Calls));
    EXPECT_TRUE(F.restores());
    // An unchanged-output certificate is not meaningful without that exact
    // logical in/out role. It must not become a general preserve-register flag.
    F.Signature.Parameters[1].TheRole = SourceParameterTypeHint::Role::Ordinary;
    std::string Diagnostic;
    ASSERT_TRUE(
        assignDarwinSwiftSourceABI(F.Signature, Architecture, Diagnostic));
    EXPECT_FALSE(F.restores());
    EXPECT_FALSE(
        preservesNativeSourceLeafState(F.Function, Architecture, F.Calls));
    F.Calls.begin()->second.PreservesSwiftErrorResult = false;
    EXPECT_TRUE(F.restores());
    EXPECT_TRUE(
        preservesNativeSourceLeafState(F.Function, Architecture, F.Calls));
  }
}

TEST(SourceFrameAnalysis, ErrorResultEffectsMeetAllPathsAndLoopBackedges) {
  for (auto Architecture : {Arch::AArch64, Arch::X64})
    for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
      SCOPED_TRACE(static_cast<unsigned>(Architecture));
      SCOPED_TRACE(Mutation);
      SwiftErrorStateFixture F(Architecture);
      const auto Original = F.Function.Blocks.front();
      const auto Call =
          std::find_if(Original.Ops.begin(), Original.Ops.end(),
                       [](const LowOp &O) { return O.Opcode == NdOp::CALL; });
      F.Function.Blocks.resize(4);
      auto &Blocks = F.Function.Blocks;
      for (unsigned I = 0; I < 4; ++I) {
        Blocks[I] = {};
        Blocks[I].Id = I;
        Blocks[I].StartAddr = 0x1000 + I * 0x100;
      }
      Blocks[0].Ops.assign(Original.Ops.begin(), Call);
      Blocks[0].Succs = {1, 2};
      F.Calls.clear();
      for (unsigned I : {1, 2}) {
        auto Current = *Call;
        Current.Addr = Blocks[I].StartAddr;
        Blocks[I].Ops = {Current};
        Blocks[I].Preds = {0};
        Blocks[I].Succs = {3};
        NativeSourceCallContract Contract;
        Contract.Signature = &F.Signature;
        Contract.PreservesSwiftErrorResult = Mutation != I;
        F.Calls.emplace(*nativeSourceCallKey(Current), Contract);
      }
      Blocks[3].Ops.assign(Call + 1, Original.Ops.end());
      Blocks[3].Preds = {1, 2};
      if (Mutation >= 3) {
        Blocks[1].Preds.push_back(1);
        Blocks[1].Succs.push_back(1);
      }
      if (Mutation == 4)
        F.Calls.begin()->second.PreservesSwiftErrorResult = false;
      if (Mutation == 5)
        std::reverse(Blocks.begin(), Blocks.end());
      EXPECT_EQ(F.restores(), Mutation == 0 || Mutation == 3 || Mutation == 5);
    }
}

TEST(SourceFrameAnalysis, ErrorOutputCannotRestoreAnotherPreservedRegister) {
  for (auto Architecture : {Arch::AArch64, Arch::X64}) {
    SwiftErrorStateFixture F(Architecture);
    const auto Saved = NdVar::reg(
        Architecture == Arch::AArch64 ? a64reg::X19 : x86reg::RBX, 8);
    const auto Address = NdVar::tmp(80, 8);
    auto &Ops = F.Function.Blocks.front().Ops;
    const auto Call = std::find_if(Ops.begin(), Ops.end(), [](const LowOp &O) {
      return O.Opcode == NdOp::CALL;
    });
    Ops.insert(Call, {op(NdOp::INT_ADD, Address, {F.Stack, NdVar::scalar(8, 8)},
                         0x1008),
                      op(NdOp::STORE, {}, {Address, F.Error}, 0x1008),
                      op(NdOp::COPY, F.Error, {Saved}, 0x100c)});
    Ops.insert(Ops.end() - 2, {op(NdOp::COPY, Saved, {F.Error}, 0x1018),
                               op(NdOp::INT_ADD, Address,
                                  {F.Stack, NdVar::scalar(8, 8)}, 0x101e),
                               op(NdOp::LOAD, F.Error, {Address}, 0x101e)});
    EXPECT_FALSE(F.restores());
    F.Calls.begin()->second.PreservesSwiftErrorResult = true;
    EXPECT_TRUE(F.restores());
  }
}

TEST(SourceFrameAnalysis, ErrorOutputCannotSupplyAnEarlierFrameDefinition) {
  FrameFixture F;
  SwiftErrorStateFixture Error(Arch::AArch64);
  auto &Ops = F.Low.Blocks.front().Ops;
  Ops[4] = op(NdOp::CALL, {}, {NdVar::cst(0x4000, 8)}, 0x1010);
  Ops[5] = op(NdOp::STORE, {}, {Slot, Value}, 0x1014);
  NativeSourceCallContract Contract;
  Contract.Signature = &Error.Signature;
  NativeSourceCalls Calls{{*nativeSourceCallKey(Ops[4]), Contract}};
  EXPECT_FALSE(F.query(Calls));
  Calls.begin()->second.PreservesSwiftErrorResult = true;
  const auto Definition = F.query(Calls);
  ASSERT_TRUE(Definition);
  EXPECT_EQ(Definition->Definition.Instruction, 0x1008U);
  EXPECT_EQ(Definition->FrameOffset, -32);
}

namespace {
SourceFunctionTypeHint completeOutputSignature() {
  SourceFunctionTypeHint Result;
  Result.Origin = SourceFunctionTypeHint::OriginKind::NativeAnalysis;
  Result.ReturnType = NdType::makeVoid();
  Result.Parameters = {{"out", NdType::makePtr(NdType::makeVoid())}};
  std::string Error;
  EXPECT_TRUE(assignDarwinScalarSourceABI(Result, Arch::AArch64, Error))
      << Error;
  Result.Parameters[0].Location.RegisterOffset = a64reg::X8;
  return Result;
}
LowFunc completeOutputFixture() {
  LowFunc F;
  F.Entry = 0x1000;
  LowBlock B;
  B.Id = 0;
  B.StartAddr = F.Entry;
  const auto Pointer = NdVar::reg(a64reg::X8, 8);
  const auto Address = NdVar::tmp(0x10000, 8);
  const auto FP =
      NdVar::reg(getTargetRegInfo(Arch::AArch64).FPParamRegs[0], 16);
  for (unsigned I = 0; I != 3; ++I) {
    B.Ops.push_back(op(NdOp::INT_ADD, Address,
                       {Pointer, NdVar::scalar(I * 16, 8)}, F.Entry));
    B.Ops.push_back(op(NdOp::STORE, {}, {Address, FP}, F.Entry));
  }
  B.Ops.push_back(
      op(NdOp::RETURN, {}, {NdVar::reg(a64reg::X30, 8)}, F.Entry + 4));
  F.Blocks.push_back(std::move(B));
  return F;
}
} // namespace

TEST(SourceFrameAnalysis,
     CompleteOutputPrefixRequiresEveryWrittenByteAndNoEscape) {
  const auto Signature = completeOutputSignature();
  auto Query = [&](const LowFunc &F) {
    return sourceFrameCompleteOutputBytes(F, Arch::AArch64, {}, Signature, 0);
  };
  EXPECT_EQ(Query(completeOutputFixture()), 48U);
  // A complete smaller prefix proves its own bound, never a 48-byte record.
  auto Smaller = completeOutputFixture();
  Smaller.Blocks[0].Ops.erase(Smaller.Blocks[0].Ops.begin() + 4,
                              Smaller.Blocks[0].Ops.begin() + 6);
  EXPECT_EQ(Query(Smaller), 32U);
  for (unsigned Mutation = 0; Mutation != 16; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = completeOutputFixture();
    auto &Ops = F.Blocks[0].Ops;
    switch (Mutation) {
    case 0:
      Ops[3].Inputs[1].Size = 15;
      break;
    case 1:
      Ops[0].Inputs[1] = NdVar::scalar(UINT64_MAX, 8);
      break;
    case 2:
      Ops[4].Inputs[1] = NdVar::scalar(256, 8);
      break;
    case 3:
      Ops[4].Inputs[1] = NdVar::scalar(INT64_MAX, 8);
      break;
    case 4:
      Ops[3].Inputs[1] = NdVar::reg(a64reg::SP, 16);
      break;
    case 5:
      Ops[3].Inputs[1] = NdVar::reg(a64reg::X8, 8);
      break;
    case 6:
      Ops.insert(Ops.begin(), op(NdOp::LOAD, NdVar::reg(a64reg::X0, 8),
                                 {NdVar::reg(a64reg::X8, 8)}, F.Entry));
      break;
    case 7:
      Ops.insert(Ops.begin(),
                 op(NdOp::STORE, {},
                    {NdVar::reg(a64reg::X0, 8), NdVar::scalar(1, 8)}, F.Entry));
      break;
    case 8:
      Ops[0].Output.Size = 4;
      break;
    case 9:
      Ops[0].Inputs[0].Size = 4;
      break;
    case 10:
      Ops[0].Opcode = NdOp::INT_XOR;
      break;
    case 11:
      Ops[1].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    case 12:
      Ops.back().Inputs[0] = NdVar::reg(a64reg::X0, 8);
      break;
    case 13:
      Ops.insert(Ops.end() - 1, op(NdOp::COPY, NdVar::reg(a64reg::X19, 8),
                                   {NdVar::scalar(0, 8)}, F.Entry));
      break;
    case 14:
      F.Blocks[0].Preds = {0};
      F.Blocks[0].Succs = {0};
      break;
    case 15:
      Ops.insert(Ops.begin() + 1, op(NdOp::LOAD, NdVar::reg(a64reg::X0, 8),
                                     {NdVar::tmp(0x10000, 8)}, F.Entry));
      break;
    }
    EXPECT_FALSE(Query(F));
  }
  EXPECT_FALSE(sourceFrameCompleteOutputBytes(completeOutputFixture(),
                                              Arch::X64, {}, Signature, 0));
}

TEST(SourceFrameAnalysis,
     CompleteOutputPrefixMeetsAllReturnsAndValidatesSDKTailWrites) {
  const auto Signature = completeOutputSignature();
  auto F = completeOutputFixture();
  F.Blocks[0].Ops = {op(NdOp::COND_BR, {},
                        {NdVar::cst(0x1010, 8), NdVar::scalar(1, 1)}, F.Entry)};
  F.Blocks[0].Succs = {1, 2};
  for (int I : {1, 2}) {
    auto B = completeOutputFixture().Blocks[0];
    B.Id = I;
    B.StartAddr = F.Entry + 16 * I;
    B.Preds = {0};
    F.Blocks.push_back(std::move(B));
  }
  auto Query = [&](const LowFunc &Fn,
                   const NativeSourceCalls &Calls = NativeSourceCalls{}) {
    return sourceFrameCompleteOutputBytes(Fn, Arch::AArch64, Calls, Signature,
                                          0);
  };
  EXPECT_EQ(Query(F), 48U);
  std::swap(F.Blocks[1], F.Blocks[2]);
  EXPECT_EQ(Query(F), 48U);
  F.Blocks[2].Ops.erase(F.Blocks[2].Ops.begin() + 4,
                        F.Blocks[2].Ops.begin() + 6);
  EXPECT_FALSE(Query(F));
  auto Tail = completeOutputFixture();
  Tail.Blocks[0].Ops = {op(NdOp::CALL, X0, {NdVar::cst(0x2000, 8)}, Tail.Entry),
                        op(NdOp::RETURN, {}, {X0}, Tail.Entry)};
  SourceFunctionTypeHint Producer;
  Producer.Origin = SourceFunctionTypeHint::OriginKind::ExplicitSource;
  Producer.ReturnType =
      NdType::makeStruct(std::vector<TypeRef>(6, NdType::makeFloat(8)));
  std::string Error;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Producer, Arch::AArch64, Error))
      << Error;
  NativeSourceCallContract Contract;
  Contract.Signature = &Producer;
  Contract.InitializesIndirectResult = true;
  NativeSourceCalls Calls{
      {*nativeSourceCallKey(Tail.Blocks[0].Ops[0]), Contract}};
  EXPECT_EQ(Query(Tail, Calls), 48U);
  Calls.begin()->second.InitializesIndirectResult = false;
  EXPECT_FALSE(Query(Tail, Calls));
}

TEST(SourceFrameAnalysis,
     CompleteOutputCertificateFeedsOnlyItsPrivateWrittenRange) {
  const auto Producer = completeOutputSignature();
  SourceFunctionTypeHint Consumer, Entry;
  Consumer.ReturnType = Entry.ReturnType = NdType::makeVoid();
  Consumer.Parameters = {{"value", NdType::makeStruct(std::vector<TypeRef>(
                                       6, NdType::makeFloat(8)))}};
  std::string Error;
  ASSERT_TRUE(assignDarwinFixedSourceABI(Consumer, Arch::AArch64, Error))
      << Error;
  ASSERT_TRUE(Consumer.Parameters[0].IndirectByValue);
  ASSERT_TRUE(assignDarwinScalarSourceABI(Entry, Arch::AArch64, Error))
      << Error;
  const auto LR = NdVar::reg(a64reg::X30, 8),
             Address = NdVar::reg(a64reg::X9, 8),
             Output = NdVar::reg(a64reg::X8, 8);
  for (unsigned Mutation = 0; Mutation != 12; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto P = Producer;
    LowFunc F;
    F.Entry = 0x1000;
    LowBlock B;
    B.Id = 0;
    B.StartAddr = F.Entry;
    B.Ops = {op(NdOp::INT_SUB, SP, {SP, NdVar::scalar(80, 8)}, 0x1000),
             op(NdOp::INT_ADD, Address, {SP, NdVar::scalar(64, 8)}, 0x1004),
             op(NdOp::STORE, {}, {Address, LR}, 0x1008),
             op(NdOp::COPY, Output, {SP}, 0x100c),
             op(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)}, 0x1010),
             op(NdOp::COPY, X0, {SP}, 0x1014),
             op(NdOp::CALL, {}, {NdVar::cst(0x3000, 8)}, 0x1018),
             op(NdOp::INT_ADD, Address, {SP, NdVar::scalar(64, 8)}, 0x101c),
             op(NdOp::LOAD, LR, {Address}, 0x1020),
             op(NdOp::INT_ADD, SP, {SP, NdVar::scalar(80, 8)}, 0x1024),
             op(NdOp::RETURN, {}, {LR}, 0x1028)};
    NativeSourceCallContract Write, Read;
    Write.Signature = &P;
    Write.WritableFrameParameters[0] = 48;
    Write.InitializesFrameParameters.insert(0);
    Read.Signature = &Consumer;
    Read.ByValueFrameParameters.insert(0);
    NativeSourceCalls Calls;
    if (Mutation == 1)
      Write.InitializesFrameParameters.clear();
    if (Mutation == 2)
      Write.WritableFrameParameters[0] = 47;
    if (Mutation == 3)
      Write.WritableFrameParameters[0] = 32;
    if (Mutation == 4)
      Write.WritableFrameParameters.clear();
    if (Mutation == 5 || Mutation == 6 || Mutation == 7)
      B.Ops[3] = op(NdOp::INT_ADD, Output,
                    {SP, NdVar::scalar(Mutation == 5   ? 1
                                       : Mutation == 6 ? 40
                                                       : 24,
                                       8)},
                    0x100c);
    if (Mutation == 8)
      B.Ops[3].Output.Size = 4;
    if (Mutation == 9) {
      P.Parameters.push_back({"alias", NdType::makePtr(NdType::makeVoid())});
      P.Parameters.back().Location = {};
      P.Parameters.back().Location.Kind = SourceABICarrierKind::IntegerRegister;
      P.Parameters.back().Location.RegisterOffset = a64reg::X0;
      P.Parameters.back().Location.ValueBytes = 8;
      Write.ReadOnlyFrameParameters[1] = 8;
      B.Ops.insert(B.Ops.begin() + 4, op(NdOp::COPY, X0, {SP}, 0x100e));
    }
    if (Mutation == 10 || Mutation == 11) {
      auto Other = Write;
      Other.InitializesFrameParameters.clear();
      if (Mutation == 11)
        Other.OpaqueValueParameters[0] = {
            SourceFrameValueEffect::Action::Initialize, "opaque", 48};
      const auto Call = op(NdOp::CALL, {}, {NdVar::cst(0x4000, 8)},
                           Mutation == 10 ? 0x1013 : 0x100e);
      Calls.emplace(*nativeSourceCallKey(Call), Other);
      B.Ops.insert(B.Ops.begin() + 4, Call);
      // A later ordinary may-write invalidates the complete initializer;
      // a live opaque value cannot be overwritten by a byte certificate.
      if (Mutation == 10) {
        auto &Ops = B.Ops;
        std::swap(Ops[4], Ops[5]);
        Ops.insert(Ops.begin() + 5, op(NdOp::COPY, Output, {SP}, 0x1012));
      } else
        B.Ops.insert(B.Ops.begin() + 5, op(NdOp::COPY, Output, {SP}, 0x100f));
    }
    for (const auto &O : B.Ops) {
      if (O.Addr == 0x1010)
        Calls.emplace(*nativeSourceCallKey(O), Write);
      if (O.Addr == 0x1018)
        Calls.emplace(*nativeSourceCallKey(O), Read);
    }
    F.Blocks = {B};
    const auto Site =
        std::find_if(B.Ops.begin(), B.Ops.end(),
                     [](const LowOp &O) { return O.Addr == 0x1018; });
    ASSERT_NE(Site, B.Ops.end());
    const auto Copies = sourceFrameByValueCopies(
        F, Arch::AArch64, Calls, *nativeSourceCallKey(*Site), Entry);
    EXPECT_EQ(bool(Copies), Mutation == 0);
    if (Copies) {
      ASSERT_EQ(Copies->size(), 1U);
      EXPECT_EQ(Copies->front().Bytes, 48U);
      EXPECT_EQ(Copies->front().FrameOffset, -80);
    }
  }
}

TEST(SourceFrameAnalysis,
     CompleteOutputEffectsRequireAnOrdinaryWritablePointer) {
  const auto Signature = completeOutputSignature();
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto S = Signature;
    SourceFrameEffects E;
    E.WritableFrameParameters[0] = 48;
    E.InitializesFrameParameters.insert(0);
    if (Mutation == 1)
      E.WritableFrameParameters.clear();
    if (Mutation == 2)
      S.Parameters[0].Type = NdType::makeInt(8);
    if (Mutation == 3)
      S.Parameters[0].TheRole = SourceParameterTypeHint::Role::SwiftContext;
    if (Mutation == 4)
      E.OpaqueValueParameters[0] = {SourceFrameValueEffect::Action::Initialize,
                                    "opaque", 48};
    if (Mutation == 5)
      E.ReturnFrameOrExternal = SourceFrameReturnAlias{0, 48};
    if (Mutation == 6)
      E.Scratch = SourceFrameScratchEffect{
          SourceFrameScratchEffect::Domain::SwiftAccess,
          SourceFrameScratchEffect::Action::Initialize, 0, 48};
    EXPECT_EQ(sourceFrameEffectsMatchABI(E, S), Mutation == 0);
  }
}
