//===- SourceImageBytesTests.cpp - Conditional byte observations ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check conditional byte observations and rejection of unauthenticated inputs.

#include "../../../lib/ir/low/LowScalarTargets.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/CallRegisterEffects.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/ir/low/SourceImageBytes.h"
#include "neverd/loader/BinaryImage.h"

#include <algorithm>

using namespace neverd;

namespace {
LowOp operation(NdOp Opcode, NdVar Output, std::initializer_list<NdVar> Inputs,
                va_t Address) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.Addr = Address;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  return Op;
}

struct Fixture {
  BinaryImage Image;
  LowFunc Function;
  Fixture(Arch Architecture = Arch::AArch64,
          BinaryFormat Format = BinaryFormat::ELF) {
    Image.Arch = Architecture;
    Image.Bits = Bitness::Bits64;
    Image.Format = Format;
    Segment Data;
    Data.Name = ".data";
    Data.VA = 0x4000;
    Data.Size = Data.FileSz = 0x100;
    Data.Data.resize(0x100);
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Image.Segments.push_back(std::move(Data));
    Function.Entry = 0x1000;
    Function.Blocks.resize(2);
    auto &Init = Function.Blocks[0];
    Init.Id = 0;
    Init.StartAddr = 0x1000;
    Init.Ops = {operation(NdOp::STORE, {},
                          {NdVar::cst(0x4000, 8), NdVar::cst(0x004e4e47, 4)},
                          0x1000)};
    auto &Decode = Function.Blocks[1];
    Decode.Id = 1;
    Decode.StartAddr = 0x1100;
    Decode.Ops = {operation(NdOp::LOAD, NdVar::reg(0, 4),
                            {NdVar::cst(0x4000, 8)}, 0x1100),
                  operation(NdOp::INT_XOR, NdVar::reg(8, 4),
                            {NdVar::reg(0, 4), NdVar::cst(0x00212121, 4)},
                            0x1104),
                  operation(NdOp::STORE, {},
                            {NdVar::cst(0x4020, 8), NdVar::reg(8, 4)}, 0x1108)};
  }
  SourceImageByteObservations observe() const {
    return observeSourceImageBytes(Function, Image);
  }
};

// Each operation below is a separate original instruction. A shared suffix
// must receive each predecessor's physical registers, never a merged value.
void directEdge(LowBlock &From, LowBlock &To) {
  const va_t At = From.Ops.back().Addr + 4;
  From.Ops.push_back(
      operation(NdOp::BRANCH, {}, {NdVar::cst(To.StartAddr, 8)}, At));
  From.EndAddr = At + 4;
  From.Succs = {To.Id};
  To.Preds.push_back(From.Id);
  From.InstructionBoundaries.clear();
  for (size_t I = 0; I < From.Ops.size(); ++I) {
    LowInstructionBoundary Boundary;
    Boundary.Address = From.Ops[I].Addr;
    Boundary.Size = 4;
    Boundary.FirstOp = I;
    Boundary.OpCount = 1;
    if (I + 1 == From.Ops.size()) {
      Boundary.Control = LowInstructionControl::Branch;
      Boundary.Immediate = To.StartAddr;
    }
    From.InstructionBoundaries.push_back(Boundary);
  }
}

Fixture sharedTail(Arch Architecture, BinaryFormat Format) {
  Fixture F(Architecture, Format);
  F.Function.Blocks.resize(5);
  auto &First = F.Function.Blocks[1];
  First.Ops.back() =
      operation(NdOp::COPY, NdVar::reg(16, 8), {NdVar::cst(0x4020, 8)}, 0x1108);
  auto &Second = F.Function.Blocks[2];
  Second = First;
  Second.Id = 2;
  Second.StartAddr = 0x1150;
  for (auto &Op : Second.Ops)
    Op.Addr += 0x50;
  Second.Ops[1].Inputs[1] = NdVar::cst(0x003c2f25, 4);
  auto &Middle = F.Function.Blocks[3];
  Middle.Id = 3;
  Middle.StartAddr = 0x1200;
  Middle.Ops = {
      operation(NdOp::STORE, {}, {NdVar::reg(16, 8), NdVar::reg(8, 2)}, 0x1200),
      operation(NdOp::INT_RIGHT, NdVar::reg(8, 4),
                {NdVar::reg(8, 4), NdVar::cst(16, 4)}, 0x1204),
      operation(NdOp::INT_ADD, NdVar::reg(16, 8),
                {NdVar::reg(16, 8), NdVar::cst(2, 8)}, 0x1208)};
  auto &End = F.Function.Blocks[4];
  End.Id = 4;
  End.StartAddr = 0x1300;
  End.Ops = {operation(NdOp::STORE, {}, {NdVar::reg(16, 8), NdVar::reg(8, 2)},
                       0x1300)};
  directEdge(First, Middle);
  directEdge(Second, Middle);
  directEdge(Middle, End);
  return F;
}

Fixture inheritedAddresses(Arch Architecture, BinaryFormat Format,
                           bool Loop = false) {
  Fixture F(Architecture, Format);
  auto Decode = F.Function.Blocks[1];
  F.Function.Blocks.resize(4);
  auto &Entry = F.Function.Blocks[0];
  Entry.Ops.push_back(operation(NdOp::COPY, NdVar::reg(16, 8),
                                {NdVar::cst(0x4020, 8)}, 0x1004));
  Entry.Ops.push_back(operation(NdOp::COPY, NdVar::reg(24, 8),
                                {NdVar::cst(0x4000, 8)}, 0x1008));
  auto &First = F.Function.Blocks[1];
  First.Id = 1;
  First.StartAddr = 0x1100;
  First.Ops = {operation(NdOp::COPY, NdVar::reg(88, 8), {NdVar::reg(80, 8)},
                         First.StartAddr)};
  auto &Second = F.Function.Blocks[2];
  Second = First;
  Second.Id = 2;
  Second.StartAddr = Second.Ops[0].Addr = 0x1010;
  auto &Join = F.Function.Blocks[3];
  Join = Decode;
  Join.Id = 3;
  Join.StartAddr = 0x1300;
  for (auto &Op : Join.Ops)
    Op.Addr += 0x200;
  Join.Ops.front().Inputs[0] = NdVar::reg(24, 8);
  Join.Ops.back().Inputs[0] = NdVar::reg(16, 8);
  directEdge(Entry, First);
  Entry.Ops.back().Opcode = NdOp::COND_BR;
  Entry.Ops.back().addInput(NdVar::reg(96, 1));
  Entry.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Branch |
      LowInstructionControlFlag::Conditional;
  Entry.Succs.push_back(Second.Id);
  Second.Preds.push_back(Entry.Id);
  directEdge(First, Join);
  directEdge(Second, Join);
  if (Loop)
    directEdge(Join, Join);
  return F;
}
} // namespace

TEST(SourceImageBytes, CommonEntryAddressesReachConditionalJoinsAndLoops) {
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Loop : {false, true}) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(Loop);
        auto F = inheritedAddresses(Architecture, Format, Loop);
        if (Architecture == Arch::AArch64)
          F.Function.Blocks.front().Ops[2].Inputs[0] =
              NdVar::addressFragment(0x4000, 8);
        const auto Result = F.observe();
        ASSERT_TRUE(Result.Complete);
        const auto Found =
            std::find_if(Result.Values.begin(), Result.Values.end(),
                         [](const SourceImageBytes &Value) {
                           return Value.Block == 0x1300;
                         });
        ASSERT_NE(Found, Result.Values.end());
        EXPECT_EQ(Found->Address, 0x4020u);
        EXPECT_EQ(Found->Bytes, (std::vector<uint8_t>{'f', 'o', 'o', 0}));
        EXPECT_EQ(Found->Inputs.size(), 4u);
        EXPECT_EQ(Found->EntryRegisters,
                  (std::vector<SourceImageRegisterInput>{{16, 8, 0x4020},
                                                         {24, 8, 0x4000}}));
      }
}

TEST(SourceImageBytes, EntryAddressProofRejectsUnresolvedIncomingPaths) {
  for (unsigned Kind = 0; Kind < 7; ++Kind) {
    SCOPED_TRACE(Kind);
    auto F = inheritedAddresses(Arch::AArch64, BinaryFormat::ELF);
    auto &Arm = F.Function.Blocks[2];
    auto &Join = F.Function.Blocks[3];
    if (Kind == 0) {
      Arm.Ops[0].Output = NdVar::reg(16, 8);
      Arm.Ops[0].Inputs[0] = NdVar::cst(0x4040, 8);
    }
    if (Kind == 1)
      Arm.Ops[0].Output = NdVar::reg(16, 8);
    if (Kind == 2) {
      Arm.Ops[0].Opcode = NdOp::INTRINSIC;
      Arm.Ops[0].Output = {};
      Arm.Ops[0].Inputs[0] = NdVar::cst(0x2000, 8);
    }
    if (Kind == 3)
      F.Function.ModuleAnalysisRoots.insert(Join.StartAddr);
    if (Kind == 4)
      Join.ExceptionalPreds.emplace_back();
    if (Kind == 5)
      F.Function.TruncatedPathAddresses.push_back(0x1400);
    if (Kind == 6)
      F.Function.UnsafeIndirectBranchAddresses.insert(0x1200);
    const auto Result = F.observe();
    EXPECT_TRUE(Result.Complete);
    EXPECT_TRUE(std::none_of(
        Result.Values.begin(), Result.Values.end(),
        [](const SourceImageBytes &Value) { return Value.Block == 0x1300; }));
  }
}

TEST(SourceImageBytes, CallCandidatesDoNotPromoteBodyOrABIContracts) {
  constexpr size_t Count = 273;
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (unsigned Mode = 0; Mode != 11; ++Mode) {
        SCOPED_TRACE(static_cast<int>(Architecture));
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(Mode);
        auto F = inheritedAddresses(Architecture, Format, true);
        std::map<va_t, LocalRegisterEffect> Effects;
        for (size_t I = 0; I != Count; ++I) {
          LowFunc Callee;
          Callee.Entry = 0x8000 + I * 16;
          Callee.Blocks.resize(1);
          auto &Block = Callee.Blocks.front();
          Block.StartAddr = Callee.Entry;
          if (I + 1 < Count)
            Block.Ops.push_back(operation(NdOp::CALL, {},
                                          {NdVar::cst(Callee.Entry + 16, 8)},
                                          Callee.Entry));
          else if (Mode == 1 || Mode == 10)
            Block.Ops.push_back(operation(NdOp::COPY,
                                          NdVar::reg(Mode == 1 ? 27 : 52, 1),
                                          {NdVar::cst(0, 1)}, Callee.Entry));
          else if (Mode == 2 || Mode == 4)
            Block.Ops.push_back(operation(
                NdOp::CALL, {}, {NdVar::cst(0x100000, 8)}, Callee.Entry));
          else if (Mode == 3)
            Block.Ops.push_back(operation(NdOp::INTRINSIC, {},
                                          {NdVar::cst(0, 8)}, Callee.Entry));
          else
            Block.Ops.push_back(operation(
                NdOp::STORE, {}, {NdVar::cst(0x4000, 8), NdVar::cst(0, 4)},
                Callee.Entry));
          Block.Ops.push_back(
              operation(NdOp::RETURN, {}, {}, Callee.Entry + 4));
          const std::map<va_t, GPRReadWidths> Contracts{{Callee.Entry, {}}};
          Effects.emplace(Callee.Entry,
                          localRegisterEffect(F.Image, Callee, nullptr,
                                              Mode == 4 && I + 1 == Count
                                                  ? &Contracts
                                                  : nullptr));
        }
        const auto Calls = solveCallRegisterEffects(Effects, 1, 0);
        if (Mode == 4) {
          // A fixed argument contract retains the existing ABI summary,
          // but supplies no original-body preservation proof.
          ASSERT_TRUE(Calls.MayWrite.contains(0x8000));
          EXPECT_FALSE(Calls.BodyMayWrite.contains(0x8000));
        }
        for (int Arm : {1, 2}) {
          auto &Call = F.Function.Blocks[Arm].Ops.front();
          Call = operation(Mode == 6   ? NdOp::INDIR_CALL
                           : Mode == 7 ? NdOp::INTRINSIC
                                       : NdOp::CALL,
                           {}, {NdVar::cst(0x8000, 8)}, Call.Addr);
          if (Mode == 8)
            Call.Inputs[0] = NdVar::addressFragment(0x8000, 8);
        }
        if (Mode == 5) {
          // The ARM64 mask does not cover X16. The x64 offset is outside
          // the GPR bank too; neither architecture may invent preservation.
          F.Function.Blocks.front().Ops[2].Output = NdVar::reg(128, 8);
          F.Function.Blocks.back().Ops.front().Inputs[0] = NdVar::reg(128, 8);
        }
        if (Mode % 2)
          std::reverse(F.Function.Blocks.begin(), F.Function.Blocks.end());
        const auto *Decode = F.Function.blockFor(0x1300);
        ASSERT_NE(Decode, nullptr);
        const LowScalarCallEffects ExactCalls{Architecture, Calls.BodyMayWrite};
        const auto Exact = finiteScalarBlockEntryValues(
            F.Function,
            {{Decode,
              {NdVar::reg(16, 8), NdVar::reg(Mode == 5 ? 128 : 24, 8)}}},
            nullptr, Mode == 9 ? nullptr : &ExactCalls);
        ASSERT_TRUE(Exact);
        const bool BothExact =
            Exact->contains(Decode) && Exact->at(Decode).size() == 2;
        EXPECT_EQ(BothExact, Mode == 0 || Mode == 10);
        const auto Result = observeSourceImageBytes(
            F.Function, F.Image, Mode == 9 ? nullptr : &Calls.BodyMayWrite);
        ASSERT_TRUE(Result.Complete);
        const auto Found =
            std::find_if(Result.Values.begin(), Result.Values.end(),
                         [](const SourceImageBytes &Value) {
                           return Value.Block == 0x1300;
                         });
        const bool Expected = Mode != 7;
        ASSERT_EQ(Found != Result.Values.end(), Expected);
        if (Expected) {
          EXPECT_EQ(Found->Bytes, (std::vector<uint8_t>{'f', 'o', 'o', 0}));
          // Both unknown and definitely writing callees can contradict these
          // entry hypotheses. Even the complete no-register-write leaf writes
          // the memory in mode 0: every assumed byte must remain explicit.
          EXPECT_EQ(Found->Inputs.size(), 4u);
          EXPECT_EQ(Found->EntryRegisters,
                    (std::vector<SourceImageRegisterInput>{
                        {16, 8, 0x4020}, {Mode == 5 ? 128u : 24u, 8, 0x4000}}));
        }
      }
}

TEST(SourceImageBytes, EntryInitializersInvalidateConflictingLocalText) {
  auto F = inheritedAddresses(Arch::AArch64, BinaryFormat::ELF);
  auto &Join = F.Function.Blocks.back();
  Join.Ops = {operation(NdOp::STORE, {}, {NdVar::reg(24, 8), NdVar::cst(0, 4)},
                        0x1300)};
  Fixture Local;
  auto Decode = Local.Function.Blocks[1];
  Decode.Id = 4;
  Decode.StartAddr = 0x1400;
  for (auto &Op : Decode.Ops)
    Op.Addr += 0x300;
  F.Function.Blocks.push_back(std::move(Decode));
  const auto Result = F.observe();
  EXPECT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Values.empty());
}

TEST(SourceImageBytes, ConditionalCallAddressesRecoverInitializerAndTransform) {
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(Reverse);
      auto F = inheritedAddresses(Architecture, BinaryFormat::ELF);
      F.Function.Blocks[0].Ops[0] =
          operation(NdOp::COPY, NdVar::reg(88, 8), {NdVar::reg(80, 8)}, 0x1000);
      for (int I : {1, 2})
        F.Function.Blocks[I].Ops[0] =
            operation(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)},
                      F.Function.Blocks[I].StartAddr);
      auto Decode = F.Function.Blocks.back();
      Decode.Id = 4;
      Decode.StartAddr = 0x1400;
      Decode.Preds.clear();
      for (auto &Op : Decode.Ops)
        Op.Addr += 0x100;
      F.Function.Blocks.push_back(std::move(Decode));
      auto &Init = F.Function.Blocks[3];
      Init.Ops = {operation(NdOp::STORE, {},
                            {NdVar::reg(24, 8), NdVar::cst(0x004e4e47, 4)},
                            0x1300)};
      directEdge(Init, F.Function.Blocks[4]);
      if (Reverse)
        std::reverse(F.Function.Blocks.begin(), F.Function.Blocks.end());
      const auto Result = F.observe();
      ASSERT_TRUE(Result.Complete);
      const auto Found = std::find_if(
          Result.Values.begin(), Result.Values.end(),
          [](const SourceImageBytes &V) { return V.Block == 0x1400; });
      ASSERT_NE(Found, Result.Values.end());
      EXPECT_EQ(Found->Bytes, (std::vector<uint8_t>{'f', 'o', 'o', 0}));
      EXPECT_EQ(Found->EntryRegisters, (std::vector<SourceImageRegisterInput>{
                                           {16, 8, 0x4020}, {24, 8, 0x4000}}));
      ASSERT_EQ(Found->Inputs.size(), 4u);
      const std::vector<uint8_t> Required{'G', 'N', 'N', 0};
      for (size_t I = 0; I < Required.size(); ++I) {
        EXPECT_EQ(Found->Inputs[I].Address, 0x4000u + I);
        EXPECT_EQ(Found->Inputs[I].Initializer, 0x1300u);
        EXPECT_EQ(Found->Inputs[I].Value, Required[I]);
      }
    }
}

TEST(SourceImageBytes, ConditionalAddressesCannotWithdrawExactInitializers) {
  auto F = inheritedAddresses(Arch::AArch64, BinaryFormat::ELF);
  for (int I : {1, 2})
    F.Function.Blocks[I].Ops[0] =
        operation(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)},
                  F.Function.Blocks[I].StartAddr);
  F.Function.Blocks.back().Ops = {operation(
      NdOp::STORE, {}, {NdVar::reg(24, 8), NdVar::cst(0, 4)}, 0x1300)};
  Fixture Local;
  auto Decode = Local.Function.Blocks[1];
  Decode.Id = 4;
  Decode.StartAddr = 0x1400;
  for (auto &Op : Decode.Ops)
    Op.Addr += 0x300;
  F.Function.Blocks.push_back(std::move(Decode));
  const auto Result = F.observe();
  ASSERT_EQ(Result.Values.size(), 1u);
  EXPECT_EQ(Result.Values.front().Bytes,
            (std::vector<uint8_t>{'f', 'o', 'o', 0}));
  for (const auto &Input : Result.Values.front().Inputs)
    EXPECT_EQ(Input.Initializer, 0x1000u);
}

TEST(SourceImageBytes, EntryInitializersFeedOtherwiseLocalTransforms) {
  auto F = inheritedAddresses(Arch::AArch64, BinaryFormat::ELF);
  auto &Entry = F.Function.Blocks.front();
  // Keep the instruction in place so branch provenance remains unchanged.
  Entry.Ops.front() =
      operation(NdOp::COPY, NdVar::reg(88, 8), {NdVar::reg(80, 8)}, 0x1000);
  auto &Join = F.Function.Blocks.back();
  Join.Ops = {operation(
      NdOp::STORE, {}, {NdVar::reg(24, 8), NdVar::cst(0x004e4e47, 4)}, 0x1300)};
  Fixture Local;
  auto Decode = Local.Function.Blocks[1];
  Decode.Id = 4;
  Decode.StartAddr = 0x1400;
  for (auto &Op : Decode.Ops)
    Op.Addr += 0x300;
  F.Function.Blocks.push_back(std::move(Decode));
  const auto Result = F.observe();
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Values.size(), 1u);
  EXPECT_EQ(Result.Values.front().Block, 0x1400u);
  EXPECT_EQ(Result.Values.front().Bytes,
            (std::vector<uint8_t>{'f', 'o', 'o', 0}));
  EXPECT_TRUE(Result.Values.front().EntryRegisters.empty());
  for (const auto &Input : Result.Values.front().Inputs)
    EXPECT_EQ(Input.Initializer, 0x1300u);
}

TEST(SourceImageBytes, EntryExtensionAndLocalTextBothComplete) {
  auto F = inheritedAddresses(Arch::AArch64, BinaryFormat::ELF);
  Fixture Local;
  auto Decode = Local.Function.Blocks[1];
  Decode.Id = 4;
  Decode.StartAddr = 0x1400;
  for (auto &Op : Decode.Ops)
    Op.Addr += 0x300;
  F.Function.Blocks.push_back(std::move(Decode));
  const auto Complete = F.observe();
  ASSERT_TRUE(Complete.Complete);
  ASSERT_GE(Complete.Values.size(), 2u);
  EXPECT_TRUE(std::any_of(Complete.Values.begin(), Complete.Values.end(),
                          [](const SourceImageBytes &Value) {
                            return Value.Block == 0x1400 &&
                                   Value.Bytes ==
                                       std::vector<uint8_t>{'f', 'o', 'o', 0};
                          }));
}

TEST(SourceImageBytes, ConditionalInputsRemainExplicitAcrossTargets) {
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(static_cast<int>(Format));
      Fixture F(Architecture, Format);
      const auto Result = F.observe();
      ASSERT_TRUE(Result.Complete);
      ASSERT_EQ(Result.Values.size(), 1u);
      const auto &Value = Result.Values.front();
      EXPECT_EQ(Value.Block, 0x1100u);
      EXPECT_EQ(Value.Instruction, 0x1108u);
      EXPECT_EQ(Value.Address, 0x4020u);
      EXPECT_EQ(Value.Bytes, (std::vector<uint8_t>{'f', 'o', 'o', 0}));
      ASSERT_EQ(Value.Inputs.size(), 4u);
      for (size_t I = 0; I < 4; ++I) {
        EXPECT_EQ(Value.Inputs[I].Address, 0x4000u + I);
        EXPECT_EQ(Value.Inputs[I].Initializer, 0x1000u);
        EXPECT_EQ(Value.Inputs[I].Value, (0x004e4e47u >> (8 * I)) & 255);
      }
      // The initializer need not reach the transform: the annotation states
      // its byte condition instead of inventing a path through the CFG.
      std::reverse(F.Function.Blocks.begin(), F.Function.Blocks.end());
      EXPECT_EQ(F.observe().Values, Result.Values);
    }
}

TEST(SourceImageBytes, UnknownInputsEffectsAndConflictsSupplyNoText) {
  for (unsigned Mode = 0; Mode < 6; ++Mode) {
    SCOPED_TRACE(Mode);
    Fixture F;
    auto &Decode = F.Function.Blocks[1].Ops;
    switch (Mode) {
    case 0: // missing initializer
      F.Function.Blocks[0].Ops.clear();
      break;
    case 1: // competing byte values are not resolved by scanning order
      F.Function.Blocks[0].Ops.push_back(operation(
          NdOp::STORE, {}, {NdVar::cst(0x4000, 8), NdVar::cst(0, 1)}, 0x1004));
      break;
    case 2: // call between input and output
      Decode.insert(Decode.begin() + 1,
                    operation(NdOp::CALL, NdVar::tmp(8, 8),
                              {NdVar::cst(0x2000, 8)}, 0x1102));
      break;
    case 3: // an unknown alias after the store invalidates its bytes
      Decode.push_back(operation(
          NdOp::STORE, {}, {NdVar::reg(80, 8), NdVar::cst(0, 4)}, 0x110c));
      break;
    case 4: // unknown arithmetic operand
      Decode[1].Inputs[1] = NdVar::tmp(9, 4);
      break;
    case 5: // ordered memory is outside this ordinary-byte contract
      Decode[0].MemoryOrdering = NdMemoryOrdering::Acquire;
      break;
    }
    const auto Result = F.observe();
    EXPECT_TRUE(Result.Complete);
    EXPECT_TRUE(Result.Values.empty());
  }
}

TEST(SourceImageBytes, EveryIndependentTransformIsRetained) {
  Fixture F;
  const auto Decode = F.Function.Blocks[1];
  constexpr unsigned Count = 5000;
  F.Function.Blocks.resize(Count + 1);
  for (unsigned I = 0; I < Count; ++I) {
    auto &Block = F.Function.Blocks[I + 1];
    Block = Decode;
    Block.Id = I + 1;
    Block.StartAddr = Decode.StartAddr + 16 * I;
    for (auto &Op : Block.Ops)
      Op.Addr += 16 * I;
  }
  const auto Result = F.observe();
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Values.size(), Count);
  for (const auto &Value : Result.Values)
    EXPECT_EQ(Value.Bytes, (std::vector<uint8_t>{'f', 'o', 'o', 0}));
}

TEST(SourceImageBytes, TerminatedTextKeepsItsWholeTransformedRange) {
  Fixture F;
  auto &Data = F.Image.Segments.front();
  Data.Size = Data.FileSz = 0x4000;
  Data.Data.resize(Data.Size);
  auto &Init = F.Function.Blocks[0];
  auto &Decode = F.Function.Blocks[1];
  Init.Ops.clear();
  Decode.Ops.clear();
  Decode.StartAddr = 0x2000;
  constexpr size_t Length = 2048;
  for (size_t I = 0; I <= Length; I += 8) {
    const uint64_t Encoded = I == Length ? 0 : 0x5959595959595959;
    const uint64_t Key = I == Length ? 0 : 0x2121212121212121;
    Init.Ops.push_back(operation(
        NdOp::STORE, {}, {NdVar::cst(0x4000 + I, 8), NdVar::cst(Encoded, 8)},
        Init.StartAddr + 4 * (I / 8)));
    const va_t At = Decode.StartAddr + 12 * (I / 8);
    Decode.Ops.push_back(operation(NdOp::LOAD, NdVar::reg(0, 8),
                                   {NdVar::cst(0x4000 + I, 8)}, At));
    Decode.Ops.push_back(operation(NdOp::INT_XOR, NdVar::reg(8, 8),
                                   {NdVar::reg(0, 8), NdVar::cst(Key, 8)},
                                   At + 4));
    Decode.Ops.push_back(
        operation(NdOp::STORE, {},
                  {NdVar::cst(0x6000 + I, 8), NdVar::reg(8, 8)}, At + 8));
  }
  const auto Result = F.observe();
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Values.size(), 1u);
  EXPECT_EQ(Result.Values.front().Address, 0x6000u);
  auto Expected = std::vector<uint8_t>(Length, 'x');
  Expected.push_back(0);
  EXPECT_EQ(Result.Values.front().Bytes, Expected);
}

TEST(SourceImageBytes, IncompleteInstructionsCannotProposeOrPublishBytes) {
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (bool Initializing : {false, true})
        for (unsigned Kind = 0; Kind < 4; ++Kind) {
          SCOPED_TRACE(static_cast<int>(Architecture));
          SCOPED_TRACE(static_cast<int>(Format));
          SCOPED_TRACE(Initializing);
          SCOPED_TRACE(Kind);
          Fixture F(Architecture, Format);
          auto &Ops = F.Function.Blocks[Initializing ? 0 : 1].Ops;
          const va_t Store = Ops.back().Addr;
          LowOp Tail;
          switch (Kind) {
          case 0:
            Tail = operation(NdOp::INTRINSIC, {}, {}, Store);
            break;
          case 1:
            Tail = operation(NdOp::LOAD, NdVar::reg(80, 1),
                             {NdVar::cst(0x4040, 8)}, Store);
            Tail.MemoryOrdering = NdMemoryOrdering::Acquire;
            break;
          case 2:
            Tail = operation(NdOp::COPY, NdVar::reg(80, 4),
                             {NdVar::tmp(512, 4)}, Store);
            break;
          case 3:
            Tail = operation(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)}, Store);
            break;
          }
          Ops.push_back(Tail);
          const auto Incomplete = F.observe();
          EXPECT_TRUE(Incomplete.Complete);
          EXPECT_TRUE(Incomplete.Values.empty());
          // Stopping at the next instruction still retains the completed
          // store's initializer proposal or the completed transform prefix.
          Ops.back().Addr += 4;
          const auto Complete = F.observe();
          ASSERT_TRUE(Complete.Complete);
          ASSERT_EQ(Complete.Values.size(), 1u);
          EXPECT_EQ(Complete.Values.front().Bytes,
                    (std::vector<uint8_t>{'f', 'o', 'o', 0}));
        }
}

TEST(SourceImageBytes, IncompleteInitializerCannotConflictWithCompleteOne) {
  Fixture F;
  auto &Bad = F.Function.Blocks[0];
  LowBlock Good = Bad;
  Good.Id = 2;
  Good.StartAddr = Good.Ops[0].Addr = 0x1080;
  Bad.Ops[0].Inputs[1] = NdVar::cst(0, 4);
  Bad.Ops.push_back(operation(NdOp::INTRINSIC, {}, {}, 0x1000));
  F.Function.Blocks.push_back(std::move(Good));
  const auto Result = F.observe();
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Values.size(), 1u);
  EXPECT_EQ(Result.Values.front().Bytes,
            (std::vector<uint8_t>{'f', 'o', 'o', 0}));
  for (const auto &Input : Result.Values.front().Inputs)
    EXPECT_EQ(Input.Initializer, 0x1080u);
}

TEST(SourceImageBytes, EarlyControlTransferDoesNotCompleteAnInstruction) {
  for (bool Initializing : {false, true})
    for (auto Opcode : {NdOp::BRANCH, NdOp::COND_BR, NdOp::RETURN}) {
      SCOPED_TRACE(Initializing);
      SCOPED_TRACE(static_cast<int>(Opcode));
      Fixture F;
      auto &Ops = F.Function.Blocks[Initializing ? 0 : 1].Ops;
      const va_t Store = Ops.back().Addr;
      Ops.push_back(operation(Opcode, {}, {NdVar::cst(0x2000, 8)}, Store));
      if (Opcode == NdOp::COND_BR)
        Ops.back().addInput(NdVar::reg(96, 1));
      ASSERT_EQ(F.observe().Values.size(), 1u);
      Ops.push_back(operation(
          NdOp::STORE, {}, {NdVar::cst(0x4000, 8), NdVar::cst(0, 4)}, Store));
      EXPECT_TRUE(F.observe().Values.empty());
    }
}

TEST(SourceImageBytes, ImmutableVectorInitializersKeepTheirEntireLoad) {
  for (auto Architecture : {Arch::X64, Arch::AArch64}) {
    Fixture F(Architecture);
    Segment Pool;
    Pool.Name = "read_only_mapping";
    Pool.VA = 0x8000;
    Pool.Size = Pool.FileSz = 16;
    Pool.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Pool.Data = {'G',  'N',  'N', 0, 0xa1, 0xb2, 0xc3, 0xd4,
                 0xe5, 0xf6, 7,   8, 9,    10,   11,   12};
    F.Image.Segments.push_back(Pool);
    Section Writable;
    Writable.Name = ".data";
    Writable.VA = 0x4000;
    Writable.Size = Writable.FileSz = 0x100;
    Writable.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    F.Image.Sections.push_back(Writable);
    Section Section;
    Section.Name = ".rodata";
    Section.VA = Pool.VA;
    Section.Size = Section.FileSz = Pool.Size;
    Section.Flags = SegmentFlags::Readable;
    F.Image.Sections.push_back(Section);
    auto &Init = F.Function.Blocks[0].Ops;
    Init = {operation(NdOp::LOAD, NdVar::reg(32, 16), {NdVar::cst(0x8000, 8)},
                      0x1000),
            operation(NdOp::STORE, {},
                      {NdVar::cst(0x4000, 8), NdVar::reg(32, 16)}, 0x1004)};
    auto &Decode = F.Function.Blocks[1].Ops;
    Decode[0].Output.Size = 16;
    Decode[1].Output.Size = 16;
    Decode[1].Inputs[0].Size = 16;
    Decode[1].Inputs[1].Size = 16;
    Decode[2].Inputs[1].Size = 16;
    const auto Result = F.observe();
    ASSERT_TRUE(Result.Complete);
    ASSERT_EQ(Result.Values.size(), 1u);
    EXPECT_EQ(Result.Values.front().Bytes,
              (std::vector<uint8_t>{'f', 'o', 'o', 0}));
    ASSERT_EQ(Result.Values.front().Inputs.size(), 16u);
    for (size_t I = 0; I < 16; ++I)
      EXPECT_EQ(Result.Values.front().Inputs[I].Value, Pool.Data[I]);
    F.Image.Segments.back().Flags = SegmentFlags::Readable |
                                    SegmentFlags::Writable |
                                    SegmentFlags::Executable;
    EXPECT_TRUE(F.observe().Values.empty());
    F.Image.Segments.back().ReadOnlyAfterRelocations = true;
    EXPECT_EQ(F.observe().Values, Result.Values);
    // Permission alone grants no byte evidence: an overlapping fixup still
    // rejects the entire vector, including a conflict outside its text prefix.
    F.Image.Relocations.emplace_back();
    F.Image.Relocations.back().Address = 0x8008;
    EXPECT_TRUE(F.observe().Values.empty());
    F.Image.Relocations.clear();
    F.Image.Sections.back().Flags =
        SegmentFlags::Readable | SegmentFlags::Executable;
    EXPECT_TRUE(F.observe().Values.empty());
  }
}

TEST(SourceImageBytes, SharedTailsKeepEachPrefixesRegistersAndPartialStores) {
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      auto F = sharedTail(Architecture, Format);
      const auto Result = F.observe();
      ASSERT_TRUE(Result.Complete);
      ASSERT_EQ(Result.Values.size(), 2u);
      for (size_t I = 0; I < 2; ++I) {
        const auto &Value = Result.Values[I];
        EXPECT_EQ(Value.Block, I ? 0x1150u : 0x1100u);
        EXPECT_EQ(Value.Instruction, 0x1300u);
        EXPECT_EQ(Value.Address, 0x4020u);
        EXPECT_EQ(Value.Bytes, I ? (std::vector<uint8_t>{'b', 'a', 'r', 0})
                                 : (std::vector<uint8_t>{'f', 'o', 'o', 0}));
        ASSERT_EQ(Value.Inputs.size(), 4u);
        for (size_t J = 0; J < 4; ++J) {
          EXPECT_EQ(Value.Inputs[J].Address, 0x4000u + J);
          EXPECT_EQ(Value.Inputs[J].Initializer, 0x1000u);
        }
      }
      std::reverse(F.Function.Blocks.begin(), F.Function.Blocks.end());
      auto Reversed = F.observe();
      ASSERT_TRUE(Reversed.Complete);
      std::reverse(Reversed.Values.begin(), Reversed.Values.end());
      EXPECT_EQ(Reversed.Values, Result.Values);
    }
}

TEST(SourceImageBytes, CrossBlockEffectsAndUnprovedEdgesDoNotSupplyText) {
  for (unsigned Mode = 0; Mode < 8; ++Mode) {
    SCOPED_TRACE(Mode);
    auto F = sharedTail(Arch::AArch64, BinaryFormat::ELF);
    auto &Middle = F.Function.Blocks[3];
    auto &End = F.Function.Blocks[4];
    switch (Mode) {
    case 0:
      Middle.Ops.front() =
          operation(NdOp::CALL, {}, {NdVar::cst(0x2000, 8)}, 0x1200);
      break;
    case 1:
      End.Ops.push_back(operation(
          NdOp::STORE, {}, {NdVar::reg(80, 8), NdVar::cst(0, 4)}, 0x1304));
      break;
    case 2:
      Middle.Ops.front().MemoryOrdering = NdMemoryOrdering::Release;
      break;
    case 3:
      Middle.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Conditional;
      break;
    case 4:
      Middle.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Indirect;
      break;
    case 5:
      Middle.InstructionBoundaries.clear();
      break;
    case 6:
      Middle.InstructionBoundaries.back().Immediate = End.StartAddr + 4;
      break;
    case 7:
      Middle.Succs = {Middle.Id};
      Middle.InstructionBoundaries.back().Immediate = Middle.StartAddr;
      Middle.Ops.back().Inputs[0].Offset = Middle.StartAddr;
      break;
    }
    const auto Result = F.observe();
    EXPECT_TRUE(Result.Complete);
    EXPECT_TRUE(Result.Values.empty());
  }
}

TEST(SourceImageBytes, FallthroughTailsAndInitializerAddressesKeepTheirState) {
  auto F = sharedTail(Arch::AArch64, BinaryFormat::ELF);
  auto &Middle = F.Function.Blocks[3];
  auto &End = F.Function.Blocks[4];
  Middle.Ops.pop_back();
  Middle.InstructionBoundaries.pop_back();
  Middle.EndAddr -= 4;
  End.StartAddr = End.Ops.front().Addr = Middle.EndAddr;
  EXPECT_EQ(F.observe().Values.size(), 2u);

  Fixture G;
  G.Function.Blocks.resize(3);
  auto &Init = G.Function.Blocks[0];
  Init.Ops = {operation(NdOp::COPY, NdVar::reg(16, 8), {NdVar::cst(0x4000, 8)},
                        0x1000)};
  auto &Store = G.Function.Blocks[2];
  Store.Id = 2;
  Store.StartAddr = 0x1080;
  Store.Ops = {operation(
      NdOp::STORE, {}, {NdVar::reg(16, 8), NdVar::cst(0x004e4e47, 4)}, 0x1080)};
  directEdge(Init, Store);
  const auto Result = G.observe();
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Values.size(), 1u);
  EXPECT_EQ(Result.Values.front().Bytes,
            (std::vector<uint8_t>{'f', 'o', 'o', 0}));
  for (const auto &Input : Result.Values.front().Inputs)
    EXPECT_EQ(Input.Initializer, 0x1080u);
}

TEST(SourceImageBytes, TemporaryLifetimeIsNotAnEntryValue) {
  Fixture F;
  auto &Ops = F.Function.Blocks[1].Ops;
  Ops[0].Output = Ops[1].Inputs[0] = NdVar::tmp(0, 4);
  Ops[1].Output = Ops[2].Inputs[1] = NdVar::tmp(8, 4);
  EXPECT_TRUE(F.observe().Values.empty());
  F.Function.FunctionTemporaries = {{0, 4}, {8, 4}};
  EXPECT_EQ(F.observe().Values.size(), 1u);
  F.Function.FunctionTemporaries.clear();
  Ops[1].Addr = Ops[2].Addr = Ops[0].Addr;
  EXPECT_EQ(F.observe().Values.size(), 1u);
}

TEST(SourceImageBytes, ContinuationChainsKeepLocalObservations) {
  Fixture F;
  const size_t ChainLength = 32;
  F.Function.Blocks.resize(2 + ChainLength);
  for (size_t I = 0; I < ChainLength; ++I) {
    auto &Block = F.Function.Blocks[2 + I];
    Block.Id = 2 + I;
    Block.StartAddr = 0x2000 + 8 * I;
    Block.Ops = {operation(NdOp::COPY, NdVar::reg(80, 8), {NdVar::cst(1, 8)},
                           Block.StartAddr)};
  }
  for (size_t I = 0; I + 1 < ChainLength; ++I)
    directEdge(F.Function.Blocks[2 + I], F.Function.Blocks[3 + I]);
  const auto Result = F.observe();
  EXPECT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Values.size(), 1u);
  EXPECT_EQ(Result.Values.front().Bytes,
            (std::vector<uint8_t>{'f', 'o', 'o', 0}));
}

TEST(SourceImageBytes, ContinuedInitializersInvalidateConflictingProposals) {
  Fixture F;
  F.Function.Blocks.resize(4);
  auto &Address = F.Function.Blocks[2];
  Address.Id = 2;
  Address.StartAddr = 0x1200;
  Address.Ops = {operation(NdOp::COPY, NdVar::reg(16, 8),
                           {NdVar::cst(0x4000, 8)}, 0x1200)};
  auto &Store = F.Function.Blocks[3];
  Store.Id = 3;
  Store.StartAddr = 0x1300;
  Store.Ops = {operation(NdOp::STORE, {}, {NdVar::reg(16, 8), NdVar::cst(0, 4)},
                         0x1300)};
  directEdge(Address, Store);
  const auto Result = F.observe();
  EXPECT_TRUE(Result.Complete);
  EXPECT_TRUE(Result.Values.empty());
}

TEST(SourceImageBytes, TemporaryDefinitionsKeepPartialLifetimesAcrossWords) {
  Fixture F;
  auto &Ops = F.Function.Blocks[1].Ops;
  Ops[0].Output = Ops[1].Inputs[0] = NdVar::tmp(63, 4);
  F.Function.FunctionTemporaries = {{64, 2}};
  EXPECT_TRUE(F.observe().Values.empty());
  // Only the middle two bytes live across the instruction boundary. Defining
  // the other two bytes in the new instruction completes the four-byte read.
  Ops.insert(
      Ops.begin() + 1,
      {operation(NdOp::COPY, NdVar::tmp(63, 1), {NdVar::cst('G', 1)}, 0x1104),
       operation(NdOp::COPY, NdVar::tmp(66, 1), {NdVar::cst(0, 1)}, 0x1104)});
  const auto Result = F.observe();
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Values.size(), 1u);
  EXPECT_EQ(Result.Values.front().Bytes,
            (std::vector<uint8_t>{'f', 'o', 'o', 0}));
  F.Function.FunctionTemporaries = {{63, 4}};
  Ops.erase(Ops.begin() + 1, Ops.begin() + 3);
  EXPECT_EQ(F.observe().Values.size(), 1u);
}

TEST(SourceImageBytes, PackedTemporariesKeepIndependentTransforms) {
  Fixture F;
  for (unsigned I = 0; I < 64; ++I) {
    LowBlock Block;
    Block.Id = 2 + I;
    Block.StartAddr = 0x2000 + 4 * I;
    Block.Ops = {operation(NdOp::COPY, NdVar::tmp(63, 16), {NdVar::cst(0, 16)},
                           Block.StartAddr),
                 operation(NdOp::COPY, NdVar::reg(80, 16), {NdVar::tmp(63, 16)},
                           Block.StartAddr)};
    F.Function.Blocks.push_back(std::move(Block));
  }
  const auto Result = F.observe();
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.Values.size(), 1u);
  EXPECT_EQ(Result.Values.front().Bytes,
            (std::vector<uint8_t>{'f', 'o', 'o', 0}));
}

TEST(SourceImageBytes, WritableLoadsRemainConditionalWithUnrelatedFixups) {
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Architecture));
      SCOPED_TRACE(static_cast<int>(Format));
      Fixture F(Architecture, Format);
      Section Data;
      Data.Name = ".data";
      Data.VA = 0x4000;
      Data.Size = Data.FileSz = 0x100;
      Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
      F.Image.Sections.push_back(Data);
      // The immutable reader rejects this writable mapping before visiting
      // any fixups. Unrelated fixups must not prevent conditional observations.
      F.Image.Relocations.resize(1024);
      for (size_t I = 0; I < F.Image.Relocations.size(); ++I)
        F.Image.Relocations[I].Address = 0x8000 + 8 * I;
      const auto Result = F.observe();
      ASSERT_TRUE(Result.Complete);
      ASSERT_EQ(Result.Values.size(), 1u);
      EXPECT_EQ(Result.Values.front().Bytes,
                (std::vector<uint8_t>{'f', 'o', 'o', 0}));
      EXPECT_EQ(Result.Values.front().Inputs.size(), 4u);
      // The early rejection must not turn the image's initial writable bytes
      // into constants or erase the explicit initializer conditions.
      F.Function.Blocks[0].Ops.clear();
      EXPECT_TRUE(F.observe().Values.empty());
    }
}
