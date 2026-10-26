//===- SBFCallXContextTests.cpp - Conditional CALLX proofs ---------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Checks pre-call arguments, typed scratch spills, distinct frames, invalid
/// targets and conservative dynamic fallback at the CALLX context boundary.
///
//===----------------------------------------------------------------------===//

#include "SBFSolanaTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFCallXContexts.h"
#include "neverd/sbf/analysis/SBFDataflow.h"
#include "neverd/sbf/image/SBFProgramImage.h"
#include "neverd/sbf/runtime/SBFInterpreter.h"
#include "neverd/sbf/runtime/SBFSemantics.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"

#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

using test::encode;

// Preserve the original direct-caller assertions while root-entry evidence is
// now reported separately by the complete-CFG query.
std::vector<CallXContextEvidence>
callerContexts(const CallXContextReport &Report) {
  std::vector<CallXContextEvidence> Result;
  for (const auto &Context : Report.Contexts)
    if (!Context.IsRootContext && !Context.EntryPrefixOnly) {
      const auto It = std::find_if(
          Report.Summaries.begin(), Report.Summaries.end(),
          [&](const auto &Summary) { return Summary.ID == Context.SummaryID; });
      if (It != Report.Summaries.end() &&
          (!It->GeneralizedInputs ||
           Context.InputGeneralization ==
               CallXInputGeneralization::EquivalentGenericInputs))
        Result.push_back(Context);
    }
  return Result;
}

llvm::Expected<SBFProgram> makeTableProgram(unsigned CallerCount = 1) {
  std::vector<test::EncodedInstruction> Text;
  const size_t Callee = CallerCount * 7;
  const size_t Target = Callee + 6;
  std::vector<uint8_t> Tables(CallerCount * sizeof(uint64_t));
  for (unsigned Caller = 0; Caller < CallerCount; ++Caller) {
    Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
    Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
    Text.push_back(encode(Opcode::ST_DW_IMM, 1, 0, 0, 123));
    Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0, Caller * 8));
    Text.push_back(encode(Opcode::ST_DW_REG, 1, 3, 8));
    Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, Callee - Text.size() - 1));
    Text.push_back(
        encode(Caller + 1 == CallerCount ? Opcode::EXIT : Opcode::JA));
    llvm::support::endian::write64le(Tables.data() + Caller * sizeof(uint64_t),
                                     kBytecodeStart + (Target + Caller * 2) *
                                                          kInstructionSize);
  }
  Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
  Text.push_back(encode(Opcode::LD_DW_REG, 1, 6, 8));
  Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
  Text.push_back(encode(Opcode::LD_DW_REG, 1, 6));
  Text.push_back(encode(Opcode::CALL_REG, 2));
  Text.push_back(encode(Opcode::EXIT));
  for (unsigned Caller = 0; Caller < CallerCount; ++Caller) {
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42 + Caller));
    Text.push_back(encode(Opcode::EXIT));
  }
  return analyze(test::makeImage(Text, Tables));
}

TEST(SBFCallXContexts, RecoversNestedTableLoadsFromPreCallArguments) {
  auto Program = makeTableProgram();
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_FALSE(Program->RegisteredSyscallHashes.empty());
  // Ordinary function-entry analysis cannot assume one caller's arguments.
  EXPECT_EQ(Program->Low.Instructions[11].Call, CallKind::Indirect);
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  ASSERT_EQ(callerContexts(*Report).size(), 1u);
  const auto Proof = callerContexts(*Report).front();
  EXPECT_EQ(Proof.CallerSlot, 5u);
  EXPECT_EQ(Proof.CalleeEntrySlot, 7u);
  EXPECT_EQ(Proof.CallXSlot, 11u);
  EXPECT_EQ(Proof.Result, CallXContextResult::ProvenTarget);
  EXPECT_EQ(Proof.TargetSlot, 13u);
  EXPECT_EQ(Program->Low.Instructions[11].Call, CallKind::Indirect);
  EXPECT_TRUE(*callXContextReportJSON(*Program, *Report)
                   .getBoolean("dynamic_fallback_required"));
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 42u);
  bool SawCall = false;
  for (size_t I = 0; I + 1 < Raw->Trace.size(); ++I)
    if (Raw->Trace[I].Slot == Proof.CallXSlot) {
      SawCall = true;
      EXPECT_EQ(Raw->Trace[I + 1].Slot, Proof.TargetSlot);
    }
  EXPECT_TRUE(SawCall);
}

TEST(SBFCallXContexts, KeepsDifferentCallerTargetsAndOpenWorldFallback) {
  auto Program = makeTableProgram(2);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  ASSERT_EQ(callerContexts(*Report).size(), 2u);
  EXPECT_EQ(callerContexts(*Report)[0].TargetSlot, 20u);
  EXPECT_EQ(callerContexts(*Report)[1].TargetSlot, 22u);
  EXPECT_EQ(callerContexts(*Report)[0].CallXSlot,
            callerContexts(*Report)[1].CallXSlot);
  EXPECT_EQ(Program->Low.Instructions[18].Call, CallKind::Indirect);
}

TEST(SBFCallXContexts, UnknownRootsDoNotPreemptFourKnownCallerVariants) {
  const size_t Count = kCallXContextSpecializationsPerEntry;
  auto Program = makeTableProgram(Count);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  const auto Contexts = callerContexts(*Report);
  for (size_t Caller = 0; Caller < Count; ++Caller) {
    const auto It = std::find_if(Contexts.begin(), Contexts.end(),
                                 [&](const auto &Context) {
                                   return Context.CallerSlot == Caller * 7 + 5;
                                 });
    ASSERT_NE(It, Contexts.end()) << Caller;
    EXPECT_EQ(It->TargetSlot, Count * 7 + 6 + Caller * 2);
  }
  EXPECT_FALSE(Report->SpecializationBudgetExhausted);
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 42u + Count - 1);
}

TEST(SBFCallXContexts, RegisteredLegacyRuntimeCannotBeBypassed) {
  auto Program = makeTableProgram();
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  for (auto &Instruction : Program->Med.Instructions)
    if (Instruction.Slot == 5) {
      Instruction.Dispatch = CallDispatchPolicy::LegacyRuntimeThenFunction;
      Instruction.Immediate = Program->RegisteredSyscallHashes.front();
    }
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  ASSERT_EQ(callerContexts(*Report).size(), 1u);
  EXPECT_EQ(callerContexts(*Report)[0].Result,
            CallXContextResult::LegacyRuntimeMayMutateArguments);
  EXPECT_FALSE(callerContexts(*Report)[0].TargetSlot);
}

TEST(SBFCallXContexts, UnknownOrPartiallyOverwrittenScratchIsNotATarget) {
  auto Program = makeTableProgram();
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  for (auto &Instruction : Program->Med.Instructions)
    if (Instruction.Slot == 4) {
      Instruction.Width = kWordBitWidth;
      Instruction.Semantics.MemoryWidth = kWordBitWidth;
    }
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  ASSERT_EQ(callerContexts(*Report).size(), 1u);
  EXPECT_EQ(callerContexts(*Report)[0].Result,
            CallXContextResult::UnknownTarget);
  EXPECT_EQ(callerContexts(*Report)[0].Missing.Kind,
            CallXMissingFactKind::ScratchWord);
  EXPECT_EQ(callerContexts(*Report)[0].Missing.Slot, 8u);
  EXPECT_EQ(callerContexts(*Report)[0].Missing.Coordinate,
            MemoryCoordinate::FrameRelative);
}

TEST(SBFCallXContexts, RawUnalignedAndOutOfRangeTargetsStayDistinguishable) {
  for (const uint64_t Target :
       {kBytecodeStart + 13 * kInstructionSize + 7, UINT64_MAX}) {
    std::array<uint8_t, sizeof(uint64_t)> Bytes;
    llvm::support::endian::write64le(Bytes.data(), Target);
    auto Program = makeTableProgram();
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Image = createProgramImage(Program->text(), Program->Low.TextAddress,
                                    Bytes, kRodataStartV3, false, Version::V3);
    ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
    Program->ExecutableImage = std::move(*Image);
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    ASSERT_EQ(callerContexts(*Report).size(), 1u);
    EXPECT_EQ(callerContexts(*Report)[0].TargetAddress, Target);
    EXPECT_EQ(callerContexts(*Report)[0].Result,
              Target == UINT64_MAX ? CallXContextResult::InvalidTarget
                                   : CallXContextResult::ProvenTarget);
    if (Target != UINT64_MAX)
      EXPECT_EQ(callerContexts(*Report)[0].TargetSlot, 13u);
  }
}

TEST(SBFCallXContexts, FollowsCallerPointerSpillsAcrossBasicBlocks) {
  std::array<uint8_t, 8> Table;
  llvm::support::endian::write64le(Table.data(),
                                   kBytecodeStart + 16 * kInstructionSize);
  auto Program = analyze(test::makeImage(
      {encode(Opcode::MOV64_REG, 1, 10),
       encode(Opcode::ADD64_IMM, 1, 0, 0, -16), encode(Opcode::MOV64_IMM, 3),
       encode(Opcode::ST_DW_REG, 1, 3, 8),
       encode(Opcode::ST_DW_REG, 10, 1, -24), encode(Opcode::JA),
       encode(Opcode::LD_DW_REG, 1, 10, -24), encode(Opcode::JA),
       encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
       encode(Opcode::MOV64_REG, 6, 1), encode(Opcode::LD_DW_REG, 1, 6, 8),
       encode(Opcode::LD_DW_REG, 2, 1), encode(Opcode::LD_DW_REG, 1, 6),
       encode(Opcode::CALL_REG, 2), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)},
      Table));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  ASSERT_EQ(callerContexts(*Report).size(), 1u);
  EXPECT_EQ(callerContexts(*Report)[0].TargetSlot, 16u);
  EXPECT_EQ(Program->Low.Instructions[14].Call, CallKind::Indirect);
}

TEST(SBFCallXContexts, AutomaticCalleeFrameDoesNotAliasCallerScratch) {
  auto Text = std::vector<test::EncodedInstruction>{
      encode(Opcode::MOV64_REG, 1, 10), encode(Opcode::ADD64_IMM, 1, 0, 0, -8)};
  auto Target = test::loadImm64(3, kBytecodeStart + 12 * kInstructionSize);
  Text.insert(Text.end(), Target.begin(), Target.end());
  for (const auto &Instruction :
       {encode(Opcode::ST_DW_REG, 1, 3), encode(Opcode::CALL_IMM, 0, 1, 0, 2),
        encode(Opcode::EXIT), encode(Opcode::EXIT),
        encode(Opcode::ST_DW_IMM, 10, 0, -8), encode(Opcode::LD_DW_REG, 2, 1),
        encode(Opcode::CALL_REG, 0, 0, 0, 2), encode(Opcode::EXIT),
        encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)})
    Text.push_back(Instruction);
  auto Image = test::makeImage(Text, {});
  Image.SBF->Version = Version::V0;
  Image.SBF->ELFFlags = 0;
  Image.SBF->StrictLayout = false;
  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  ASSERT_EQ(callerContexts(*Report).size(), 1u);
  EXPECT_EQ(callerContexts(*Report)[0].TargetSlot, 12u);
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 42u);
}

TEST(SBFCallXContexts, PropagatesNestedReturnsAndMemoryBeyondTheEntryBlock) {
  std::array<uint8_t, 8> Table;
  llvm::support::endian::write64le(Table.data(),
                                   kBytecodeStart + 19 * kInstructionSize);
  auto Program =
      analyze(test::makeImage({encode(Opcode::MOV64_REG, 1, 10),
                               encode(Opcode::ADD64_IMM, 1, 0, 0, -32),
                               encode(Opcode::MOV64_REG, 6, 1),
                               encode(Opcode::CALL_IMM, 0, 1, 0, 4),
                               encode(Opcode::LD_DW_REG, 2, 6),
                               encode(Opcode::CALL_REG, 2),
                               encode(Opcode::EXIT),
                               encode(Opcode::EXIT),
                               encode(Opcode::MOV64_IMM, 6, 0, 0, 7),
                               encode(Opcode::CALL_IMM, 0, 1, 0, 5),
                               encode(Opcode::MOV64_REG, 2, 0),
                               encode(Opcode::JA),
                               encode(Opcode::CALL_REG, 2),
                               encode(Opcode::EXIT),
                               encode(Opcode::EXIT),
                               encode(Opcode::MOV64_IMM, 3),
                               encode(Opcode::LD_DW_REG, 0, 3),
                               encode(Opcode::ST_DW_REG, 1, 0),
                               encode(Opcode::EXIT),
                               encode(Opcode::MOV64_IMM, 0, 0, 0, 42),
                               encode(Opcode::EXIT)},
                              Table));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  bool Nested = false, MemoryReturned = false;
  for (const auto &Context : Report->Contexts) {
    if (Context.CallXSlot == 12 && Context.TargetSlot == 19) {
      Nested = true;
      EXPECT_TRUE(Context.BeyondEntryBlock);
    }
    if (Context.CallXSlot == 5 && Context.TargetSlot == 19)
      MemoryReturned = true;
  }
  EXPECT_TRUE(Nested);
  EXPECT_TRUE(MemoryReturned);
  EXPECT_GT(Report->SummaryCacheHits, 0u);
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 42u);
}

TEST(SBFCallXContexts, CalleeCFGMeetsConflictingTargetsAcrossBranches) {
  for (int32_t Alternate : {0, 8}) {
    std::array<uint8_t, 16> Table;
    llvm::support::endian::write64le(Table.data(), kBytecodeStart + 9 * 8);
    llvm::support::endian::write64le(Table.data() + 8, kBytecodeStart + 11 * 8);
    auto Program = analyze(test::makeImage(
        {encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
         encode(Opcode::JEQ64_IMM, 5, 0, 2), encode(Opcode::MOV64_IMM, 2),
         encode(Opcode::JA, 0, 0, 1),
         encode(Opcode::MOV64_IMM, 2, 0, 0, Alternate),
         encode(Opcode::LD_DW_REG, 2, 2), encode(Opcode::CALL_REG, 2),
         encode(Opcode::EXIT), encode(Opcode::MOV64_IMM, 0, 0, 0, 42),
         encode(Opcode::EXIT), encode(Opcode::MOV64_IMM, 0, 0, 0, 43),
         encode(Opcode::EXIT)},
        Table));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    if (Alternate == 0) {
      EXPECT_TRUE(Report->UnresolvedCallXSlots.empty());
      EXPECT_EQ(Program->Low.Instructions[7].CallTarget, 9u);
      continue;
    }
    ASSERT_FALSE(Report->Contexts.empty());
    for (const auto &Context : Report->Contexts) {
      EXPECT_EQ(Context.CallXSlot, 7u);
      EXPECT_TRUE(Context.BeyondEntryBlock);
      EXPECT_EQ(Context.TargetSlot,
                Alternate == 0 ? std::optional<size_t>(9) : std::nullopt);
    }
  }
}

TEST(SBFCallXContexts, DiscardsProvisionalLoopTargets) {
  std::array<uint8_t, 16> Table;
  llvm::support::endian::write64le(Table.data(), kBytecodeStart + 8 * 8);
  llvm::support::endian::write64le(Table.data() + 8, kBytecodeStart + 10 * 8);
  auto Program = analyze(test::makeImage(
      {encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 2), encode(Opcode::LD_DW_REG, 2, 2),
       encode(Opcode::CALL_REG, 2), encode(Opcode::MOV64_IMM, 2, 0, 0, 8),
       encode(Opcode::JNE64_IMM, 1, 0, -4), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 43), encode(Opcode::EXIT)},
      Table));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  ASSERT_FALSE(Report->Contexts.empty());
  for (const auto &Context : Report->Contexts) {
    EXPECT_EQ(Context.CallXSlot, 4u);
    EXPECT_FALSE(Context.TargetSlot);
  }
}

TEST(SBFCallXContexts, JoinsTableBasesBeforeLoadingTheirCommonTarget) {
  for (int32_t Selector : {0, 1}) {
    std::array<uint8_t, 16> Tables;
    llvm::support::endian::write64le(Tables.data(), kBytecodeStart + 16 * 8);
    llvm::support::endian::write64le(Tables.data() + 8,
                                     kBytecodeStart + 16 * 8);
    auto Program = analyze(test::makeImage(
        {encode(Opcode::MOV64_REG, 6, 10),
         encode(Opcode::ADD64_IMM, 6, 0, 0, -16),
         encode(Opcode::JEQ64_IMM, 5, 0, 2, Selector),
         encode(Opcode::MOV64_IMM, 3), encode(Opcode::JA, 0, 0, 1),
         encode(Opcode::MOV64_IMM, 3, 0, 0, 8),
         encode(Opcode::ST_DW_REG, 6, 3, 8), encode(Opcode::MOV64_REG, 1, 6),
         encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
         encode(Opcode::JA), encode(Opcode::LD_DW_REG, 1, 1, 8),
         encode(Opcode::JA), encode(Opcode::LD_DW_REG, 2, 1),
         encode(Opcode::CALL_REG, 2), encode(Opcode::EXIT),
         encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)},
        Tables));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    bool Proven = false;
    for (const auto &Proof : Report->Contexts)
      if (Proof.CallXSlot == 14 && Proof.CallerSlot == 8 && Proof.TargetSlot) {
        EXPECT_EQ(Proof.TargetSlot, 16u);
        EXPECT_TRUE(Proof.BeyondEntryBlock);
        Proven = true;
      }
    EXPECT_TRUE(Proven);
    auto Raw = executeRaw(*Program);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, 42u);
  }
}

TEST(SBFCallXContexts, KeepsScratchFactsAcrossProvenBranchOutcomes) {
  for (unsigned Variant = 0; Variant < 5; ++Variant) {
    SCOPED_TRACE(Variant);
    std::vector<test::EncodedInstruction> Text{
        encode(Opcode::MOV64_REG, 6, 10),
        encode(Opcode::ADD64_IMM, 6, 0, 0, -16),
        encode(Opcode::ST_DW_IMM, 6),
        encode(Opcode::CALL_IMM, 0, 1, 0, 7),
        encode(Opcode::JEQ64_IMM, 0, 0, 1),
        encode(Opcode::JA, 0, 0, 1),
        encode(Opcode::ST_DW_IMM, 5, 0, 0, 123),
        encode(Opcode::LD_DW_REG, 1, 6),
        encode(Opcode::LD_DW_REG, 2, 1),
        encode(Opcode::CALL_REG, 2),
        encode(Opcode::EXIT)};
    if (Variant < 2 || Variant == 4) {
      Text.push_back(Variant == 0
                         ? encode(Opcode::MOV64_IMM, 0, 0, 0, 1)
                         : encode(Opcode::MOV64_REG, 0, Variant == 4 ? 10 : 5));
      Text.push_back(encode(Opcode::EXIT));
    } else {
      for (const auto &I :
           {encode(Opcode::JEQ64_IMM, 5, 0, 2),
            encode(Opcode::MOV64_IMM, 0, 0, 0, Variant == 2 ? 2 : 0),
            encode(Opcode::EXIT), encode(Opcode::MOV64_IMM, 0, 0, 0, 1),
            encode(Opcode::EXIT)})
        Text.push_back(I);
    }
    const size_t Target = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    std::array<uint8_t, 8> Table;
    llvm::support::endian::write64le(
        Table.data(), kBytecodeStart + Target * kInstructionSize);
    auto Program = analyze(test::makeImage(Text, Table));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    ASSERT_EQ(Program->Low.Instructions[9].Call, CallKind::Indirect);
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    const bool Proven = std::any_of(
        Report->Contexts.begin(), Report->Contexts.end(), [&](const auto &C) {
          return C.CallXSlot == 9 && C.TargetSlot == Target;
        });
    EXPECT_EQ(Proven, Variant == 0 || Variant == 2);
    EXPECT_EQ(Program->Low.Instructions[9].Call, CallKind::Indirect);
    auto Raw = executeRaw(*Program);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    if (Variant == 1) {
      EXPECT_NE(Raw->Status, ExecutionStatus::Returned);
    } else {
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 42u);
      EXPECT_TRUE(
          std::none_of(Raw->Trace.begin(), Raw->Trace.end(),
                       [](const auto &Step) { return Step.Slot == 6; }));
    }
  }
}

TEST(SBFCallXContexts, ExcludesTargetsOnProvenImpossibleCallerPaths) {
  std::array<uint8_t, 16> Table;
  llvm::support::endian::write64le(Table.data(), kBytecodeStart + 13 * 8);
  llvm::support::endian::write64le(Table.data() + 8, kBytecodeStart + 15 * 8);
  auto Program = analyze(test::makeImage(
      {encode(Opcode::MOV64_IMM, 1), encode(Opcode::MOV64_IMM, 2),
       encode(Opcode::CALL_IMM, 0, 1, 0, 5), encode(Opcode::MOV64_IMM, 1),
       encode(Opcode::MOV64_IMM, 2, 0, 0, 8),
       encode(Opcode::CALL_IMM, 0, 1, 0, 2), encode(Opcode::EXIT),
       encode(Opcode::EXIT), encode(Opcode::JEQ64_IMM, 1, 0, 3),
       encode(Opcode::LD_DW_REG, 2, 2), encode(Opcode::CALL_REG, 2),
       encode(Opcode::EXIT), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 43), encode(Opcode::EXIT)},
      Table));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  ASSERT_EQ(Program->Low.Instructions[10].Call, CallKind::Indirect);
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  ASSERT_FALSE(Report->Contexts.empty());
  for (const auto &Proof : Report->Contexts) {
    EXPECT_FALSE(Proof.TargetSlot);
    if (!Proof.GeneralizedInputs) {
      EXPECT_NE(Proof.CallerSlot, 2u);
      EXPECT_NE(Proof.CallerSlot, 5u);
    }
  }
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 0u);
  EXPECT_TRUE(std::none_of(Raw->Trace.begin(), Raw->Trace.end(),
                           [](const auto &Step) { return Step.Slot == 10; }));
}

TEST(SBFCallXContexts, ProjectsUnusedInputsWithoutConsumingVariants) {
  const size_t Count = kCallXContextSpecializationsPerEntry + 2;
  const size_t Leaf = Count * 3 + 4;
  const size_t Target = Leaf + 2;
  std::vector<test::EncodedInstruction> Text;
  for (size_t I = 0; I < Count; ++I) {
    Text.push_back(encode(Opcode::MOV64_IMM, 1));
    Text.push_back(encode(Opcode::MOV64_IMM, 6, 0, 0, I));
    Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Text.size() - 1));
  }
  for (const auto &I :
       {encode(Opcode::LD_DW_REG, 2, 0), encode(Opcode::CALL_REG, 2),
        encode(Opcode::ADD64_REG, 0, 6), encode(Opcode::EXIT),
        encode(Opcode::MOV64_REG, 0, 1), encode(Opcode::EXIT),
        encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)})
    Text.push_back(I);
  std::array<uint8_t, 8> Table;
  llvm::support::endian::write64le(Table.data(), kBytecodeStart + Target * 8);
  auto Program = analyze(test::makeImage(Text, Table));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  size_t SpecializedEdges = 0;
  std::set<size_t> SpecializedIDs;
  for (const auto &Edge : Report->SummaryEdges)
    if (Edge.CalleeEntrySlot == Leaf && !Edge.GeneralizedInputs) {
      ++SpecializedEdges;
      SpecializedIDs.insert(*Edge.CalleeSummaryID);
    }
  EXPECT_EQ(SpecializedEdges, Count);
  EXPECT_EQ(SpecializedIDs.size(), 1u);
  EXPECT_GT(Report->ProjectedRegisters, 0u);
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 42u + Count - 1);
}

TEST(SBFCallXContexts, GenericFrameWritesPreserveOnlyProvenOlderFrames) {
  for (int16_t StoreOffset : {-8, -8200}) {
    const size_t Count = kCallXContextSpecializationsPerEntry + 2;
    const size_t Leaf = 4 + Count * 2 + 3;
    const size_t Target = Leaf + 2;
    auto Text = test::loadImm64(3, kBytecodeStart + Target * 8);
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -8));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    for (size_t I = 0; I < Count; ++I) {
      Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, I));
      Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Text.size() - 1));
    }
    for (const auto &I :
         {encode(Opcode::LD_DW_REG, 2, 10, -8),
          encode(Opcode::CALL_REG, 0, 0, 0, 2), encode(Opcode::EXIT),
          encode(Opcode::ST_DW_REG, 10, 1, StoreOffset), encode(Opcode::EXIT),
          encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)})
      Text.push_back(I);
    auto Image = test::makeImage(Text, {});
    Image.SBF->Version = Version::V0;
    Image.SBF->ELFFlags = 0;
    Image.SBF->StrictLayout = false;
    auto Program = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    bool Proof = false, Generalized = false, Preserved = false;
    for (const auto &Context : Report->Contexts)
      Proof |= Context.CallXSlot == Leaf - 2 && Context.TargetSlot == Target;
    for (const auto &Edge : Report->SummaryEdges)
      if (Edge.CalleeEntrySlot == Leaf) {
        Generalized |= Edge.InputGeneralization ==
                       CallXInputGeneralization::SpecializationLimit;
        Preserved |= Edge.PreservedOlderFrames;
      }
    EXPECT_TRUE(Generalized);
    EXPECT_EQ(Proof, StoreOffset == -8);
    EXPECT_EQ(Preserved, StoreOffset == -8);
    if (StoreOffset == -8) {
      auto Raw = executeRaw(*Program);
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 42u);
    }
  }
}

TEST(SBFStoredRegisterMemory, FiniteJoinsAreCompleteAndUnknownIsAbsorbing) {
  const RegisterValue One{RegisterValue::Kind::Constant, 1, 0};
  ProvenValueSet Values(One);
  for (size_t I = 2; I <= kCallXContextValueAlternatives; ++I)
    Values.join(RegisterValue{RegisterValue::Kind::Constant, I, 0});
  ASSERT_EQ(Values.values().size(), kCallXContextValueAlternatives);
  EXPECT_EQ(Values.singleton().ValueKind, RegisterValue::Kind::Unknown);
  StoredRegisterMemory Left, Right;
  Left.writeValues(kStackStart, Values);
  Right.write(kStackStart, One);
  Left.joinValues(Right);
  EXPECT_EQ(Left.readValues(kStackStart), Values);
  Left.writeValues(kStackStart, Left.readValues(kStackStart));
  EXPECT_EQ(Left.readValues(kStackStart), Values);
  StoredRegisterMemory Copied;
  Copied.copyFrom(Left, kStackStart, kStackStart + 8, 8);
  EXPECT_EQ(Copied.readValues(kStackStart + 8), Values);
  Copied.invalidate(kStackStart + 9, 1);
  EXPECT_TRUE(Copied.empty());
  Left.meet(Right);
  EXPECT_TRUE(Left.empty());
  Values.join(RegisterValue{RegisterValue::Kind::Constant, 99, 0});
  EXPECT_TRUE(Values.unknown());
  Values.join(One);
  EXPECT_TRUE(Values.unknown());
}

TEST(SBFCallXContexts, ComposesParameterizedWritesAndReturnAddresses) {
  for (int16_t OutputOffset : {-16, -8, 0}) {
    const size_t Count = kCallXContextSpecializationsPerEntry + 2;
    const size_t Middle = 3 + Count * 4 + 3;
    const size_t Leaf = Middle + 5;
    const size_t Target = Leaf + 2;
    auto Text = test::loadImm64(3, kBytecodeStart + Target * 8);
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -8));
    for (size_t I = 0; I < Count; ++I) {
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(OutputOffset == 0
                         ? encode(Opcode::LD_DW_REG, 1, 1, -24)
                         : encode(Opcode::ADD64_IMM, 1, 0, 0, OutputOffset));
      Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, I));
      Text.push_back(
          encode(Opcode::CALL_IMM, 0, 1, 0, Middle - Text.size() - 1));
    }
    for (const auto &I :
         {encode(Opcode::LD_DW_REG, 2, 0), encode(Opcode::CALL_REG, 2),
          encode(Opcode::EXIT), encode(Opcode::MOV64_REG, 6, 1),
          encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - (Middle + 1) - 1),
          encode(Opcode::MOV64_REG, 0, 6),
          encode(Opcode::ADD64_IMM, 0, 0, 0, 8), encode(Opcode::EXIT),
          encode(Opcode::ST_DW_REG, 1, 2), encode(Opcode::EXIT),
          encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)})
      Text.push_back(I);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    bool Proven = false, Effects = false, Expression = false;
    for (const auto &Proof : Report->Contexts)
      Proven |= Proof.CallXSlot == Middle - 2 && Proof.TargetSlot == Target;
    for (const auto &Edge : Report->SummaryEdges)
      Effects |= Edge.CalleeEntrySlot == Middle && Edge.AppliedMemoryEffects;
    for (const auto &Summary : Report->Summaries)
      if (Summary.EntrySlot == Middle && Summary.GeneralizedInputs) {
        ASSERT_TRUE(Summary.ReturnInputExpression);
        EXPECT_EQ(Summary.ReturnInputExpression->first, 1u);
        EXPECT_EQ(Summary.ReturnInputExpression->second, 8);
        ASSERT_EQ(Summary.MemoryWrites.size(), 1u);
        EXPECT_EQ(Summary.MemoryWrites[0].Base,
                  CallXMemoryWriteBase::InputRegister);
        EXPECT_EQ(Summary.MemoryWrites[0].Register, 1u);
        EXPECT_EQ(Summary.MemoryWrites[0].Size, 8u);
        EXPECT_FALSE(Summary.MayWriteUnknownMemory);
        Expression = true;
      }
    EXPECT_TRUE(Effects);
    EXPECT_TRUE(Expression);
    EXPECT_EQ(Proven, OutputOffset == -16);
    if (OutputOffset == -16) {
      auto Raw = executeRaw(*Program);
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 42u);
    }
  }
}

TEST(SBFCallXContexts, SyscallArgumentsDoNotDependOnUnexposedVMRegisters) {
  const size_t Count = kCallXContextSpecializationsPerEntry + 2;
  std::vector<test::EncodedInstruction> Text;
  std::vector<size_t> Calls, CallXs;
  for (size_t I = 0; I < Count; ++I) {
    for (unsigned R = kFirstArgumentRegister;
         R < kFirstArgumentRegister + kArgumentRegisterCount; ++R)
      Text.push_back(encode(Opcode::MOV64_IMM, R));
    Text.push_back(encode(Opcode::MOV64_IMM, 7, 0, 0, I));
    Calls.push_back(Text.size());
    Text.push_back({});
    CallXs.push_back(Text.size());
    Text.push_back(encode(Opcode::CALL_REG, 0));
  }
  Text.push_back(encode(Opcode::EXIT));
  const size_t Leaf = Text.size();
  Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
  Text.push_back(
      encode(Opcode::CALL_IMM, 0, 0, 0, getSyscallInfo(Syscall::Log64)->Hash));
  Text.push_back(encode(Opcode::JEQ64_IMM, 6, 0, 2, 0));
  Text.push_back(encode(Opcode::MOV64_IMM, 0));
  Text.push_back(encode(Opcode::EXIT));
  const size_t LiteralSlot = Text.size();
  Text.resize(Text.size() + 2);
  Text.push_back(encode(Opcode::EXIT));
  const size_t Target = Text.size();
  Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
  Text.push_back(encode(Opcode::EXIT));
  const auto Literal = test::loadImm64(0, kBytecodeStart + Target * 8);
  std::copy(Literal.begin(), Literal.end(), Text.begin() + LiteralSlot);
  for (size_t Slot : Calls)
    Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Slot - 1);
  auto Program = analyze(test::makeImage(Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  std::set<size_t> Proven;
  for (const auto &Proof : Report->Contexts)
    if (std::find(CallXs.begin(), CallXs.end(), Proof.CallXSlot) !=
            CallXs.end() &&
        Proof.TargetSlot) {
      EXPECT_EQ(Proof.TargetSlot, Target);
      Proven.insert(Proof.CallXSlot);
    }
  EXPECT_EQ(Proven.size(), Count);
  size_t Variants = 0;
  for (const auto &Summary : Report->Summaries)
    if (Summary.EntrySlot == Leaf) {
      EXPECT_EQ(Summary.InputRegisterMask &
                    ((1u << 0) | (1u << 7) | (1u << kFramePointerRegister)),
                0u);
      Variants += !Summary.GeneralizedInputs;
    }
  EXPECT_EQ(Variants, 1u);
  ExecutionEnvironment Environment;
  Environment.Syscall =
      [](uint32_t, const SyscallArguments &) -> std::optional<uint64_t> {
    return 0;
  };
  auto Raw = executeRaw(*Program, std::move(Environment));
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 42u);
}

TEST(SBFStoredRegisterMemory, ScalarALUDoesNotEvaluateSymbolicAddresses) {
  auto Program = analyze(test::makeImage({encode(Opcode::EXIT)}, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  RegisterState State{};
  const auto Apply = [&](Opcode Op, uint8_t Src = 0, int32_t Immediate = 0,
                         Version V = Version::V0) {
    const auto &Info = *getOpcodeInfo(Op);
    MedInstruction I;
    I.Op = Info.Op;
    I.Form = Info.Form;
    I.Width = Info.Width;
    I.Src = Src;
    I.Immediate = static_cast<uint64_t>(Immediate);
    I.Semantics = semanticTraits(Info, V);
    applyRegisterTransfer(I, State, Program->ExecutableImage);
  };
  for (Version V : {Version::V0, Version::V2}) {
    State[0] = {RegisterValue::Kind::Constant, 0x40000000, 0};
    Apply(Opcode::MUL32_IMM, 0, 3, V);
    EXPECT_EQ(State[0].ValueKind, RegisterValue::Kind::Constant);
    EXPECT_EQ(State[0].Value, V == Version::V0 ? UINT64_C(0xffffffffc0000000)
                                               : UINT64_C(0xc0000000));
  }
  State[0] = {RegisterValue::Kind::StackAddress, kStackStart, 64};
  State[1] = {RegisterValue::Kind::Constant, 8, 0};
  Apply(Opcode::ADD64_REG, 1);
  EXPECT_EQ(State[0], (RegisterValue{RegisterValue::Kind::StackAddress,
                                     kStackStart, 72}));
  Apply(Opcode::SUB64_REG, 1);
  EXPECT_EQ(State[0].Offset, 64);
  Apply(Opcode::MUL64_IMM, 0, 1);
  EXPECT_EQ(State[0].ValueKind, RegisterValue::Kind::Unknown);
  State[0] = {RegisterValue::Kind::Constant, 5, 0};
  State[1] = {};
  Apply(Opcode::OR64_REG, 1);
  EXPECT_EQ(State[0].ValueKind, RegisterValue::Kind::Unknown);
  State[0] = {RegisterValue::Kind::Constant, 7, 0};
  Apply(Opcode::SUB64_IMM, 0, 10, Version::V2);
  EXPECT_EQ(State[0].Value, 3u);
}

TEST(SBFCallXContexts, FollowsLengthArithmeticIntoWordCopyOrMemcpy) {
  // The length calculation mirrors a compiler-emitted copy dispatcher. Both
  // its shifted/multiplied length and its register-indexed word addresses
  // must stay known. Unknown arithmetic must not authorize a target.
  for (int32_t Length : {8, 127}) {
    SCOPED_TRACE(Length);
    std::vector<test::EncodedInstruction> Text{
        encode(Opcode::MOV64_REG, 1, 10),
        encode(Opcode::ADD64_IMM, 1, 0, 0, -256)};
    const auto Literal = test::loadImm64(2, kBytecodeStart + 33 * 8);
    Text.insert(Text.end(), Literal.begin(), Literal.end());
    const std::vector<test::EncodedInstruction> Body{
        encode(Opcode::ST_DW_REG, 1, 2),
        encode(Opcode::MOV64_REG, 2, 1),
        encode(Opcode::MOV64_REG, 1, 10),
        encode(Opcode::ADD64_IMM, 1, 0, 0, -512),
        encode(Opcode::MOV64_IMM, 3, 0, 0, Length),
        encode(Opcode::CALL_IMM, 0, 1, 0, 3),
        encode(Opcode::LD_DW_REG, 2, 0),
        encode(Opcode::CALL_REG, 2),
        encode(Opcode::EXIT),
        encode(Opcode::MOV64_REG, 6, 1),
        encode(Opcode::MOV64_REG, 4, 3),
        encode(Opcode::RSH64_IMM, 4, 0, 0, 3),
        encode(Opcode::MOV64_REG, 1, 4),
        encode(Opcode::MUL64_IMM, 1, 0, 0, -7),
        encode(Opcode::ADD64_REG, 1, 3),
        encode(Opcode::JGT64_IMM, 1, 0, 9, 15),
        encode(Opcode::MOV64_IMM, 1),
        encode(Opcode::MOV64_REG, 0, 6),
        encode(Opcode::ADD64_REG, 0, 1),
        encode(Opcode::MOV64_REG, 7, 2),
        encode(Opcode::ADD64_REG, 7, 1),
        encode(Opcode::LD_DW_REG, 7, 7),
        encode(Opcode::ST_DW_REG, 0, 7),
        encode(Opcode::MOV64_REG, 0, 6),
        encode(Opcode::EXIT),
        encode(Opcode::MOV64_REG, 1, 6),
        encode(Opcode::CALL_IMM, 0, 0, 0,
               getSyscallInfo(Syscall::Memcpy)->Hash),
        encode(Opcode::MOV64_REG, 0, 6),
        encode(Opcode::EXIT),
        encode(Opcode::MOV64_IMM, 0, 0, 0, 42),
        encode(Opcode::EXIT)};
    Text.insert(Text.end(), Body.begin(), Body.end());
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    bool Proven = false;
    for (const auto &Proof : Report->Contexts)
      if (Proof.CallXSlot == 11 && Proof.TargetSlot) {
        EXPECT_EQ(Proof.TargetSlot, 33u);
        Proven = true;
      }
    EXPECT_TRUE(Proven);
    EXPECT_GT(Report->SkippedConstantBranchSuccessors, 0u);
    if (Length == 8) {
      auto Raw = executeRaw(*Program);
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 42u);
    }
  }
}

TEST(SBFCallXContexts, InstantiatesMustStoreValuesThroughGeneralizedWrappers) {
  // Vary both the output address and target past the four retained variants.
  // The generic leaf and wrapper must express the store using entry inputs,
  // rather than borrowing a previously specialized caller's memory snapshot.
  enum class StoreCase {
    Argument,
    Constant,
    Absolute,
    Logged,
    Partial,
    Aliased,
    Conditional,
    Conflicting
  };
  for (const auto Case :
       {StoreCase::Argument, StoreCase::Constant, StoreCase::Absolute,
        StoreCase::Logged, StoreCase::Partial, StoreCase::Aliased,
        StoreCase::Conditional, StoreCase::Conflicting}) {
    SCOPED_TRACE(static_cast<unsigned>(Case));
    const size_t Count = kCallXContextSpecializationsPerEntry + 2;
    std::vector<test::EncodedInstruction> Text;
    std::vector<size_t> Calls, CallXs, Literals;
    const bool Branches =
        Case == StoreCase::Conditional || Case == StoreCase::Conflicting;
    const va_t AbsoluteOutput = kHeapStart + 16;
    if (Branches)
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    for (size_t I = 0; I < Count; ++I) {
      if (Case == StoreCase::Absolute) {
        const auto Literal = test::loadImm64(1, AbsoluteOutput);
        Text.insert(Text.end(), Literal.begin(), Literal.end());
      } else {
        Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0,
                              -static_cast<int32_t>((I + 1) * 16)));
      }
      Literals.push_back(Text.size());
      const auto Literal = test::loadImm64(2, 0);
      Text.insert(Text.end(), Literal.begin(), Literal.end());
      Text.push_back(encode(Opcode::MOV64_REG, 3, 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 4, 0, 0, 0));
      if (Branches)
        Text.push_back(encode(Opcode::LD_DW_REG, 5, 6));
      Calls.push_back(Text.size());
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 0));
      CallXs.push_back(Text.size());
      Text.push_back(encode(Opcode::CALL_REG, 2));
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Middle = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, 2));
    Text.push_back(encode(Opcode::MOV64_REG, 0, 6));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Leaf = Text.size();
    std::optional<size_t> ConstantLiteral;
    if (Case == StoreCase::Constant) {
      ConstantLiteral = Text.size();
      const auto Literal = test::loadImm64(2, 0);
      Text.insert(Text.end(), Literal.begin(), Literal.end());
    }
    if (Case == StoreCase::Absolute) {
      const auto Literal = test::loadImm64(3, AbsoluteOutput);
      Text.insert(Text.end(), Literal.begin(), Literal.end());
    }
    if (Case == StoreCase::Conditional)
      Text.push_back(encode(Opcode::JEQ64_IMM, 5, 0, 1, 0));
    if (Case == StoreCase::Conflicting)
      Text.push_back(encode(Opcode::JEQ64_IMM, 5, 0, 2, 0));
    Text.push_back(
        encode(Opcode::ST_DW_REG, Case == StoreCase::Absolute ? 3 : 1, 2));
    if (Case == StoreCase::Conflicting) {
      Text.push_back(encode(Opcode::JA, 0, 0, 1));
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 4));
    }
    if (Case == StoreCase::Logged)
      Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                            getSyscallInfo(Syscall::Log64)->Hash));
    if (Case == StoreCase::Partial)
      Text.push_back(encode(Opcode::ST_B_IMM, 1, 0, 0, 0));
    if (Case == StoreCase::Aliased)
      Text.push_back(encode(Opcode::ST_DW_REG, 3, 4));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Targets = Text.size();
    for (size_t I = 0; I < Count; ++I) {
      const auto Literal =
          test::loadImm64(2, kBytecodeStart + (Targets + I * 2) * 8);
      std::copy(Literal.begin(), Literal.end(), Text.begin() + Literals[I]);
      Text[Calls[I]] = encode(Opcode::CALL_IMM, 0, 1, 0, Middle - Calls[I] - 1);
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 100 + I));
      Text.push_back(encode(Opcode::EXIT));
    }
    if (ConstantLiteral) {
      const auto Literal = test::loadImm64(2, kBytecodeStart + Targets * 8);
      std::copy(Literal.begin(), Literal.end(),
                Text.begin() + *ConstantLiteral);
    }
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    const bool MustStore =
        Case == StoreCase::Argument || Case == StoreCase::Constant ||
        Case == StoreCase::Absolute || Case == StoreCase::Logged;
    std::set<size_t> Proven;
    for (const auto &Proof : Report->Contexts) {
      const auto Found =
          std::find(CallXs.begin(), CallXs.end(), Proof.CallXSlot);
      if (Found == CallXs.end() || !Proof.TargetSlot)
        continue;
      EXPECT_TRUE(MustStore);
      const size_t I = Found - CallXs.begin();
      EXPECT_EQ(Proof.TargetSlot,
                Targets + (Case == StoreCase::Constant ? 0 : I * 2));
      Proven.insert(Proof.CallXSlot);
    }
    EXPECT_EQ(Proven.size(), MustStore ? Count : 0u);
    bool GenericStore = false;
    for (const auto &Edge : Report->SummaryEdges)
      GenericStore |= Edge.CalleeEntrySlot == Middle &&
                      Edge.GeneralizedInputs && Edge.AppliedStoredValues != 0;
    EXPECT_EQ(GenericStore, MustStore || Case == StoreCase::Aliased);
    if (MustStore) {
      bool LeafStore = false, WrapperStore = false;
      for (const auto &Summary : Report->Summaries)
        if (Summary.GeneralizedInputs && Summary.OutputParameterizedWords) {
          LeafStore |= Summary.EntrySlot == Leaf;
          WrapperStore |= Summary.EntrySlot == Middle;
        }
      EXPECT_TRUE(LeafStore);
      EXPECT_TRUE(WrapperStore);
      ExecutionEnvironment Environment;
      if (Case == StoreCase::Absolute)
        Environment.Memory.push_back(
            {AbsoluteOutput, std::vector<uint8_t>(8), true, "output"});
      Environment.Syscall =
          [](uint32_t, const SyscallArguments &) -> std::optional<uint64_t> {
        return 0;
      };
      auto Raw = executeRaw(*Program, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue,
                Case == StoreCase::Constant ? 100u : 100u + Count - 1);
    }
  }
}

TEST(SBFCallXContexts, RedundantPublishedTargetsDoNotDisplaceNewTargets) {
  constexpr uint32_t TableAddress = 0x10000;
  for (bool KnownFirstTarget : {false, true})
    for (bool NewImageArgument : {false, true})
      for (bool DistinctDescendants : {false, true})
        for (bool OpaqueReturn : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << "known=" << KnownFirstTarget
                       << ", image=" << NewImageArgument << ", descendants="
                       << DistinctDescendants << ", opaque=" << OpaqueReturn);
          std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
          std::vector<size_t> Entries, Calls;
          for (size_t I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
            Entries.push_back(Text.size());
            Text.push_back(encode(Opcode::MOV64_IMM, 0));
            Text.push_back(encode(
                Opcode::MOV64_IMM, 1, 0, 0,
                TableAddress + (I == kCallXContextSpecializationsPerEntry &&
                                        NewImageArgument
                                    ? 8
                                    : 0)));
            Text.push_back(
                encode(Opcode::MOV64_IMM, 3, 0, 0,
                       I == kCallXContextSpecializationsPerEntry ? 1 : 0));
            auto Input = test::loadImm64(4, kInputStart);
            Text.insert(Text.end(), Input.begin(), Input.end());
            Text.push_back(encode(Opcode::MOV64_IMM, 5));
            Text.push_back(
                encode(Opcode::MOV64_IMM, 9, 0, 0, TableAddress + 16 + I * 8));
            Calls.push_back(Text.size());
            Text.push_back(encode(Opcode::CALL_IMM, 0, 1));
            Text.push_back(encode(Opcode::EXIT));
          }
          const size_t Leaf = Text.size();
          for (size_t Call : Calls)
            Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Call - 1);
          for (const auto &I :
               {encode(Opcode::MOV64_REG, 6, 1),
                encode(Opcode::MOV64_REG, 7, 4),
                encode(Opcode::MOV64_REG, 8, 9),
                encode(Opcode::JEQ64_IMM, 3, 0, 3),
                encode(Opcode::LD_DW_REG, 2, 6), encode(Opcode::CALL_REG, 2),
                encode(Opcode::JA, 0, 0, 2),
                encode(Opcode::LD_DW_REG, 2, KnownFirstTarget ? 6 : 7),
                encode(Opcode::CALL_REG, 2),
                encode(Opcode::LD_DW_REG, 3, 7, 8)})
            Text.push_back(I);
          if (OpaqueReturn)
            Text.push_back(encode(Opcode::MOV64_IMM, 0));
          Text.push_back(encode(Opcode::ST_DW_IMM, 3));
          Text.push_back(encode(Opcode::EXIT));
          const size_t Targets = Text.size();
          if (DistinctDescendants) {
            Text.push_back(encode(Opcode::LD_DW_REG, 2, 9));
            Text.push_back(encode(Opcode::CALL_REG, 2));
          }
          for (const auto &I :
               {encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT),
                encode(Opcode::MOV64_IMM, 0, 0, 0, 43), encode(Opcode::EXIT)})
            Text.push_back(I);
          const size_t SecondTarget = Targets + (DistinctDescendants ? 4 : 2);
          std::vector<uint8_t> Table(16 + Entries.size() * 8);
          llvm::support::endian::write64le(Table.data(),
                                           kBytecodeStart + Targets * 8);
          llvm::support::endian::write64le(Table.data() + 8,
                                           kBytecodeStart + SecondTarget * 8);
          for (size_t I = 0; I < Entries.size(); ++I) {
            llvm::support::endian::write64le(Table.data() + 16 + I * 8,
                                             kBytecodeStart + Text.size() * 8);
            Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 100 + I));
            Text.push_back(encode(Opcode::EXIT));
          }
          auto Image = test::makeImage(Text, Table);
          Image.Sections[1].VA = TableAddress;
          Image.SBF->RodataVM.Address = TableAddress;
          auto Program = analyze(Image);
          ASSERT_TRUE(static_cast<bool>(Program))
              << llvm::toString(Program.takeError());
          auto Report = recoverCallXContexts(*Program);
          ASSERT_TRUE(static_cast<bool>(Report))
              << llvm::toString(Report.takeError());
          bool FirstTarget = false, LaterTarget = false;
          for (const auto &Site : Report->Contexts) {
            FirstTarget |=
                Site.CallXSlot == Leaf + 8 && Site.TargetSlot == Targets;
            LaterTarget |=
                Site.CallXSlot == Leaf + 5 &&
                Site.TargetSlot == (NewImageArgument ? SecondTarget : Targets);
          }
          EXPECT_EQ(FirstTarget, KnownFirstTarget);
          // Replacing a published summary must preserve the scalar result used
          // by its callers. Equal CALLX targets alone do not authorize
          // replacement.
          const bool Replaced =
              NewImageArgument &&
              (!KnownFirstTarget || (OpaqueReturn && !DistinctDescendants));
          EXPECT_EQ(LaterTarget, Replaced);
          EXPECT_EQ(Report->GeneralizedPublishedSummaries > 0, Replaced);
          size_t Variants = 0;
          for (const auto &Summary : Report->Summaries)
            Variants += Summary.EntrySlot == Leaf && !Summary.GeneralizedInputs;
          EXPECT_LE(Variants, kCallXContextSpecializationsPerEntry);
          for (const auto &Edge : Report->SummaryEdges)
            if (Edge.CalleeSummaryID)
              EXPECT_TRUE(std::any_of(
                  Report->Summaries.begin(), Report->Summaries.end(),
                  [&](const auto &Summary) {
                    return Summary.ID == *Edge.CalleeSummaryID;
                  }));
          for (size_t I = 0; I < Entries.size(); ++I) {
            Image.Entry = kBytecodeStart + Entries[I] * kInstructionSize;
            auto Selected = analyze(Image);
            ASSERT_TRUE(static_cast<bool>(Selected))
                << llvm::toString(Selected.takeError());
            ExecutionEnvironment Environment;
            std::vector<uint8_t> Input(16);
            llvm::support::endian::write64le(Input.data(),
                                             kBytecodeStart + Targets * 8);
            llvm::support::endian::write64le(Input.data() + 8, kHeapStart);
            Environment.Memory.push_back(
                {kInputStart, Input, false, "pointer"});
            Environment.Memory.push_back(
                {kHeapStart, std::vector<uint8_t>(8), true, "output"});
            auto Raw = executeRaw(*Selected, std::move(Environment));
            ASSERT_TRUE(static_cast<bool>(Raw))
                << llvm::toString(Raw.takeError());
            EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
            EXPECT_EQ(Raw->ReturnValue,
                      OpaqueReturn                                  ? 0u
                      : I + 1 == Entries.size() && NewImageArgument ? 43u
                                                                    : 42u);
          }
        }
}

TEST(SBFCallXContexts, ReclaimsSolverOnlyVariantsAfterLoopInputsWiden) {
  std::array<uint8_t, 8> Table;
  llvm::support::endian::write64le(Table.data(), kBytecodeStart + 11 * 8);
  auto Program = analyze(test::makeImage(
      {encode(Opcode::MOV64_IMM, 6), encode(Opcode::MOV64_IMM, 1),
       encode(Opcode::MOV64_REG, 2, 6), encode(Opcode::CALL_IMM, 0, 1, 0, 3),
       encode(Opcode::ADD64_IMM, 6, 0, 0, 1),
       encode(Opcode::JNE64_IMM, 6, 0, -5, 6), encode(Opcode::EXIT),
       encode(Opcode::ST_DW_REG, 10, 2, -8), encode(Opcode::LD_DW_REG, 2, 1),
       encode(Opcode::CALL_REG, 2), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)},
      Table));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  EXPECT_GT(Report->SummaryCacheEvictions, 0u);
  bool Proven = false;
  for (const auto &Proof : Report->Contexts)
    if (!Proof.EntryPrefixOnly && Proof.CallerSlot == 3 &&
        !Proof.GeneralizedInputs) {
      EXPECT_EQ(Proof.TargetSlot, 11u);
      Proven = true;
    }
  EXPECT_TRUE(Proven);
  for (const auto &Edge : Report->SummaryEdges)
    if (Edge.CalleeSummaryID)
      EXPECT_TRUE(std::any_of(Report->Summaries.begin(),
                              Report->Summaries.end(),
                              [&](const auto &Summary) {
                                return Summary.ID == *Edge.CalleeSummaryID;
                              }));
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 42u);
}

TEST(SBFCallXContexts, LaterRootsCanReclaimUnreferencedSolverVariants) {
  std::array<uint8_t, 16> Table;
  llvm::support::endian::write64le(Table.data(), kBytecodeStart + 11 * 8);
  llvm::support::endian::write64le(Table.data() + 8, kBytecodeStart + 17 * 8);
  auto Image = test::makeImage(
      {encode(Opcode::MOV64_IMM, 6), encode(Opcode::MOV64_IMM, 1),
       encode(Opcode::MOV64_REG, 2, 6), encode(Opcode::CALL_IMM, 0, 1, 0, 3),
       encode(Opcode::ADD64_IMM, 6, 0, 0, 1),
       encode(Opcode::JNE64_IMM, 6, 0, -5, 6), encode(Opcode::EXIT),
       encode(Opcode::ST_DW_REG, 10, 2, -8), encode(Opcode::LD_DW_REG, 2, 1),
       encode(Opcode::CALL_REG, 2), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 1, 0, 0, 8),
       encode(Opcode::MOV64_IMM, 2, 0, 0, 999),
       encode(Opcode::CALL_IMM, 0, 1, 0, -9), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 43), encode(Opcode::EXIT)},
      Table);
  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  EXPECT_TRUE(std::any_of(
      Report->Contexts.begin(), Report->Contexts.end(), [](const auto &Proof) {
        return Proof.CallerSlot == 15 && !Proof.EntryPrefixOnly &&
               !Proof.GeneralizedInputs && Proof.TargetSlot == 17;
      }));
  for (size_t Entry : {0u, 13u}) {
    Image.Entry = kBytecodeStart + Entry * kInstructionSize;
    auto Selected = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Selected))
        << llvm::toString(Selected.takeError());
    auto Raw = executeRaw(*Selected);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, Entry == 0 ? 42u : 43u);
  }
}

TEST(SBFCallXContexts, ReclaimsOrphanParentsBeforeLaterChildSpecializations) {
  std::array<uint8_t, 16> Table;
  llvm::support::endian::write64le(Table.data(), kBytecodeStart + 14 * 8);
  llvm::support::endian::write64le(Table.data() + 8, kBytecodeStart + 20 * 8);
  auto Image = test::makeImage({encode(Opcode::MOV64_IMM, 6),
                                encode(Opcode::MOV64_IMM, 1),
                                encode(Opcode::MOV64_REG, 2, 6),
                                encode(Opcode::CALL_IMM, 0, 1, 0, 3),
                                encode(Opcode::ADD64_IMM, 6, 0, 0, 1),
                                encode(Opcode::JNE64_IMM, 6, 0, -5, 6),
                                encode(Opcode::EXIT),
                                encode(Opcode::ST_DW_REG, 10, 2, -8),
                                encode(Opcode::CALL_IMM, 0, 1, 0, 1),
                                encode(Opcode::EXIT),
                                encode(Opcode::ST_DW_REG, 10, 2, -8),
                                encode(Opcode::LD_DW_REG, 2, 1),
                                encode(Opcode::CALL_REG, 2),
                                encode(Opcode::EXIT),
                                encode(Opcode::MOV64_IMM, 0, 0, 0, 42),
                                encode(Opcode::EXIT),
                                encode(Opcode::MOV64_IMM, 1, 0, 0, 8),
                                encode(Opcode::MOV64_IMM, 2, 0, 0, 999),
                                encode(Opcode::CALL_IMM, 0, 1, 0, -9),
                                encode(Opcode::EXIT),
                                encode(Opcode::MOV64_IMM, 0, 0, 0, 43),
                                encode(Opcode::EXIT)},
                               Table);
  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  EXPECT_TRUE(std::any_of(
      Report->Contexts.begin(), Report->Contexts.end(), [](const auto &Proof) {
        return Proof.CallerSlot == 18 && !Proof.EntryPrefixOnly &&
               !Proof.GeneralizedInputs && Proof.TargetSlot == 20;
      }));
  for (size_t Entry : {0u, 16u}) {
    Image.Entry = kBytecodeStart + Entry * kInstructionSize;
    auto Selected = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Selected))
        << llvm::toString(Selected.takeError());
    auto Raw = executeRaw(*Selected);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, Entry == 0 ? 42u : 43u);
  }
}

TEST(SBFCallXContexts, CatalogNeverReturnDoesNotOverrideTheHostOutcome) {
  const uint32_t Hash = findSyscallByName("abort")->Hash;
  auto Text = test::loadImm64(2, kBytecodeStart + 6 * 8);
  for (const auto &I :
       {encode(Opcode::CALL_IMM, 0, 0, 0, Hash), encode(Opcode::CALL_REG, 2),
        encode(Opcode::EXIT), encode(Opcode::EXIT),
        encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)})
    Text.push_back(I);
  auto Program = analyze(test::makeImage(Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  EXPECT_EQ(Report->UnresolvedCallXSlots.size(), 1u);
  EXPECT_FALSE(Report->Contexts.empty());
  ExecutionEnvironment Environment;
  Environment.HostSyscall = [](uint32_t, const SyscallArguments &) {
    return SyscallOutcome::returned(0);
  };
  auto Raw = executeRaw(*Program, Environment);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 42u);
  EXPECT_TRUE(std::any_of(Raw->Trace.begin(), Raw->Trace.end(),
                          [](const auto &Step) { return Step.Slot == 3; }));
}

TEST(SBFCallXContexts, UnknownValuesStillMeetTheirDistinctParameterOrigins) {
  for (bool Spill : {false, true})
    for (int32_t Selector : {0, 1, 2}) {
      SCOPED_TRACE(::testing::Message()
                   << "spill=" << Spill << ", selector=" << Selector);
      std::vector<test::EncodedInstruction> Text{
          encode(Opcode::MOV64_IMM, 1),
          encode(Opcode::MOV64_IMM, 2, 0, 0, 8),
          Selector == 2 ? encode(Opcode::MOV64_REG, 5, 3)
                        : encode(Opcode::MOV64_IMM, 5, 0, 0, Selector),
          encode(Opcode::CALL_IMM, 0, 1, 0, 3),
          encode(Opcode::LD_DW_REG, 2, 0),
          encode(Opcode::CALL_REG, 2),
          encode(Opcode::EXIT)};
      if (Spill) {
        auto Address = test::loadImm64(6, kStackStart + 8);
        Text.insert(Text.end(), Address.begin(), Address.end());
      }
      Text.push_back(encode(Opcode::JEQ64_IMM, 5, 0, 2));
      Text.push_back(Spill ? encode(Opcode::ST_DW_REG, 6, 2)
                           : encode(Opcode::MOV64_REG, 0, 2));
      Text.push_back(encode(Opcode::JA, 0, 0, 1));
      Text.push_back(Spill ? encode(Opcode::ST_DW_REG, 6, 1)
                           : encode(Opcode::MOV64_REG, 0, 1));
      if (Spill)
        Text.push_back(encode(Opcode::LD_DW_REG, 0, 6));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Target = Text.size();
      for (const auto &I :
           {encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT),
            encode(Opcode::MOV64_IMM, 0, 0, 0, 43), encode(Opcode::EXIT)})
        Text.push_back(I);
      std::array<uint8_t, 16> Table;
      llvm::support::endian::write64le(Table.data(),
                                       kBytecodeStart + Target * 8);
      llvm::support::endian::write64le(Table.data() + 8,
                                       kBytecodeStart + (Target + 2) * 8);
      auto Program = analyze(test::makeImage(Text, Table));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      bool Generic = false;
      for (const auto &Summary : Report->Summaries)
        if (Summary.EntrySlot == 7 && Summary.GeneralizedInputs) {
          EXPECT_FALSE(Summary.ReturnInputExpression);
          Generic = true;
        }
      EXPECT_TRUE(Generic);
      bool Specialized = false;
      for (const auto &Proof : Report->Contexts) {
        if (Selector == 2 || Proof.EntryPrefixOnly || Proof.GeneralizedInputs)
          EXPECT_FALSE(Proof.TargetSlot);
        else {
          EXPECT_EQ(Proof.TargetSlot, Selector == 0 ? Target : Target + 2);
          Specialized = true;
        }
      }
      EXPECT_EQ(Specialized, Selector != 2);
      auto Raw = executeRaw(*Program);
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, Selector == 1 ? 43u : 42u);
    }
}

TEST(SBFCallXContexts, ReusesEquivalentLeafInputsWithoutLosingCallerFacts) {
  std::array<uint8_t, 8> Table;
  llvm::support::endian::write64le(Table.data(), kBytecodeStart + 6 * 8);
  auto Program = analyze(test::makeImage(
      {encode(Opcode::CALL_IMM, 0, 1, 0, 3), encode(Opcode::LD_DW_REG, 2, 0),
       encode(Opcode::CALL_REG, 2), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)},
      Table));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  EXPECT_GT(Report->EquivalentGenericReuses, 0u);
  EXPECT_GT(Report->SkippedNonCallXRootSpecializations, 0u);
  EXPECT_EQ(
      std::count_if(Report->Summaries.begin(), Report->Summaries.end(),
                    [](const auto &Summary) { return Summary.EntrySlot == 4; }),
      1);
  bool Reused = false, Proven = false;
  for (const auto &Edge : Report->SummaryEdges)
    Reused |= Edge.CalleeEntrySlot == 4 &&
              Edge.InputGeneralization ==
                  CallXInputGeneralization::EquivalentGenericInputs;
  for (const auto &Proof : Report->Contexts)
    Proven |= Proof.CallXSlot == 2 && Proof.TargetSlot == 6;
  EXPECT_TRUE(Reused);
  EXPECT_TRUE(Proven);
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 42u);
}

TEST(SBFCallXContexts, SameRegistersWithDifferentMemoryUseDifferentSummaries) {
  std::array<uint8_t, 16> Table;
  llvm::support::endian::write64le(Table.data(), kBytecodeStart + 24 * 8);
  llvm::support::endian::write64le(Table.data() + 8, kBytecodeStart + 26 * 8);
  auto Program =
      analyze(test::makeImage({encode(Opcode::MOV64_REG, 6, 10),
                               encode(Opcode::ADD64_IMM, 6, 0, 0, -8),
                               encode(Opcode::MOV64_IMM, 2),
                               encode(Opcode::LD_DW_REG, 2, 2),
                               encode(Opcode::ST_DW_REG, 6, 2),
                               encode(Opcode::MOV64_REG, 1, 6),
                               encode(Opcode::MOV64_IMM, 2),
                               encode(Opcode::MOV64_IMM, 0),
                               encode(Opcode::CALL_IMM, 0, 1, 0, 11),
                               encode(Opcode::MOV64_IMM, 2, 0, 0, 8),
                               encode(Opcode::LD_DW_REG, 2, 2),
                               encode(Opcode::ST_DW_REG, 6, 2),
                               encode(Opcode::MOV64_REG, 1, 6),
                               encode(Opcode::MOV64_IMM, 2),
                               encode(Opcode::MOV64_IMM, 0),
                               encode(Opcode::CALL_IMM, 0, 1, 0, 4),
                               encode(Opcode::EXIT),
                               encode(Opcode::EXIT),
                               encode(Opcode::EXIT),
                               encode(Opcode::EXIT),
                               encode(Opcode::LD_DW_REG, 2, 1),
                               encode(Opcode::CALL_REG, 2),
                               encode(Opcode::EXIT),
                               encode(Opcode::EXIT),
                               encode(Opcode::MOV64_IMM, 0, 0, 0, 42),
                               encode(Opcode::EXIT),
                               encode(Opcode::MOV64_IMM, 0, 0, 0, 43),
                               encode(Opcode::EXIT)},
                              Table));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  bool First = false, Second = false;
  for (const auto &Context : callerContexts(*Report)) {
    if (Context.CallerSlot == 8) {
      First = true;
      EXPECT_EQ(Context.TargetSlot, 24u);
    }
    if (Context.CallerSlot == 15) {
      Second = true;
      EXPECT_EQ(Context.TargetSlot, 26u);
    }
  }
  EXPECT_TRUE(First);
  EXPECT_TRUE(Second);
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 43u);
}

TEST(SBFCallXContexts, RecursiveCallsDoNotInventMemoryOrReturns) {
  auto Program = analyze(test::makeImage(
      {encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
       encode(Opcode::MOV64_REG, 6, 1), encode(Opcode::CALL_IMM, 0, 1, 0, -2),
       encode(Opcode::MOV64_REG, 1, 6), encode(Opcode::LD_DW_REG, 2, 1),
       encode(Opcode::CALL_REG, 2), encode(Opcode::EXIT)},
      {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  bool Recursive = false;
  for (const auto &Edge : Report->SummaryEdges)
    Recursive |= Edge.Barrier == CallXSummaryBarrier::RecursiveCall;
  EXPECT_TRUE(Recursive);
  for (const auto &Context : Report->Contexts)
    EXPECT_FALSE(Context.TargetSlot);
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Fault, FaultCode::CallDepth);
}

TEST(SBFCallXContexts, GeneralizedInputsNeverBorrowAnotherCallersTarget) {
  const size_t Count = kCallXContextSpecializationsPerEntry + 2;
  const size_t Callee = Count * 3 + 1;
  const size_t Targets = Callee + 3;
  std::vector<test::EncodedInstruction> Text;
  std::vector<uint8_t> Table(Count * 8);
  for (size_t I = 0; I < Count; ++I) {
    Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, I * 8));
    Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, Callee - Text.size() - 1));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    llvm::support::endian::write64le(Table.data() + I * 8,
                                     kBytecodeStart + (Targets + I * 2) * 8);
  }
  Text.push_back(encode(Opcode::EXIT));
  Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
  Text.push_back(encode(Opcode::CALL_REG, 2));
  Text.push_back(encode(Opcode::EXIT));
  for (size_t I = 0; I < Count; ++I) {
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42 + I));
    Text.push_back(encode(Opcode::EXIT));
  }
  auto Program = analyze(test::makeImage(Text, Table));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  // Distinct inputs are not successive loop widenings. Keep the four
  // retained variants instead of recomputing them during fixed-point replay.
  EXPECT_EQ(Report->SummaryCacheEvictions, 0u);
  bool SawGeneralized = false, SawSpecialized = false;
  for (const auto &Context : Report->Contexts) {
    if (Context.IsRootContext || Context.EntryPrefixOnly)
      continue;
    const auto Summary =
        std::find_if(Report->Summaries.begin(), Report->Summaries.end(),
                     [&](const auto &S) { return S.ID == Context.SummaryID; });
    ASSERT_NE(Summary, Report->Summaries.end());
    if (Summary->GeneralizedInputs) {
      SawGeneralized = true;
      EXPECT_FALSE(Context.TargetSlot);
    } else {
      SawSpecialized = true;
      EXPECT_EQ(Context.TargetSlot, Targets + (Context.CallerSlot / 3) * 2);
    }
  }
  EXPECT_TRUE(SawGeneralized);
  EXPECT_TRUE(SawSpecialized);
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
}

TEST(SBFCallXContexts, LegacyRuntimeInvalidatesMemoryEvenWithPureCallee) {
  auto Text = test::loadImm64(6, kHeapStart);
  for (const auto &I :
       {encode(Opcode::MOV64_IMM, 3), encode(Opcode::LD_DW_REG, 3, 3),
        encode(Opcode::ST_DW_REG, 6, 3), encode(Opcode::CALL_IMM, 0, 1, 0, 4),
        encode(Opcode::LD_DW_REG, 2, 6), encode(Opcode::CALL_REG, 2),
        encode(Opcode::EXIT), encode(Opcode::EXIT), encode(Opcode::EXIT),
        encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)})
    Text.push_back(I);
  std::array<uint8_t, 8> Table;
  llvm::support::endian::write64le(Table.data(), kBytecodeStart + 11 * 8);
  auto Program = analyze(test::makeImage(Text, Table));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Before = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Before)) << llvm::toString(Before.takeError());
  EXPECT_TRUE(std::any_of(Before->Contexts.begin(), Before->Contexts.end(),
                          [](const auto &C) { return C.TargetSlot == 11; }));
  for (auto &Instruction : Program->Med.Instructions)
    if (Instruction.Slot == 5) {
      Instruction.Dispatch = CallDispatchPolicy::LegacyRuntimeThenFunction;
      Instruction.Immediate = Program->RegisteredSyscallHashes.front();
    }
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  ASSERT_FALSE(Report->Contexts.empty());
  for (const auto &Context : Report->Contexts)
    EXPECT_FALSE(Context.TargetSlot);
}

TEST(SBFCallXContexts, DiscoversSummaryForNewRawBlockEntry) {
  std::array<uint8_t, 8> Table;
  llvm::support::endian::write64le(Table.data(), kBytecodeStart + 12 * 8);
  auto Program = analyze(test::makeImage(
      {encode(Opcode::MOV64_IMM, 1), encode(Opcode::CALL_IMM, 0, 1, 0, 2),
       encode(Opcode::EXIT), encode(Opcode::EXIT),
       encode(Opcode::LD_DW_REG, 2, 1), encode(Opcode::CALL_REG, 2),
       encode(Opcode::EXIT), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 9), encode(Opcode::JA, 0, 0, 2),
       encode(Opcode::EXIT), encode(Opcode::EXIT),
       encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)},
      Table));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  bool SawNewEntry = false;
  for (const auto &Summary : Report->Summaries)
    if (Summary.EntrySlot == 12) {
      SawNewEntry = true;
      EXPECT_FALSE(Summary.IsRoot);
    }
  EXPECT_TRUE(SawNewEntry);
  EXPECT_TRUE(std::any_of(Report->Contexts.begin(), Report->Contexts.end(),
                          [](const auto &C) { return C.TargetSlot == 12; }));
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 42u);
}

TEST(SBFStoredRegisterMemory, JointFlowMeetsEveryPathAndTheFirstLoopIteration) {
  for (const int32_t Alternate : {11, 22}) {
    auto Program = analyze(test::makeImage(
        {encode(Opcode::MOV64_REG, 1, 10),
         encode(Opcode::ADD64_IMM, 1, 0, 0, -8),
         encode(Opcode::JEQ64_IMM, 7, 0, 2),
         encode(Opcode::ST_DW_IMM, 1, 0, 0, 11), encode(Opcode::JA, 0, 0, 1),
         encode(Opcode::ST_DW_IMM, 1, 0, 0, Alternate),
         encode(Opcode::LD_DW_REG, 2, 1), encode(Opcode::JA),
         encode(Opcode::EXIT)},
        {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    const MedInstructionIndex Index(Program->Med);
    const ScratchFlow Flow(*Program, Index, ScratchLoadPolicy::ProvenValues);
    const auto *Registers = Flow.entryRegisters(Program->Med.Blocks.back().ID);
    ASSERT_NE(Registers, nullptr);
    EXPECT_EQ((*Registers)[2].ValueKind, Alternate == 11
                                             ? RegisterValue::Kind::Constant
                                             : RegisterValue::Kind::Unknown);
    if (Alternate == 11)
      EXPECT_EQ((*Registers)[2].Value, 11u);
  }
  auto Program = analyze(test::makeImage(
      {encode(Opcode::MOV64_REG, 1, 10), encode(Opcode::ADD64_IMM, 1, 0, 0, -8),
       encode(Opcode::LD_DW_REG, 2, 1), encode(Opcode::ST_DW_IMM, 1, 0, 0, 11),
       encode(Opcode::JNE64_IMM, 7, 0, -3), encode(Opcode::EXIT)},
      {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const MedInstructionIndex Index(Program->Med);
  const ScratchFlow Flow(*Program, Index, ScratchLoadPolicy::ProvenValues);
  const auto *Registers = Flow.entryRegisters(Program->Med.Blocks.back().ID);
  ASSERT_NE(Registers, nullptr);
  EXPECT_EQ((*Registers)[2].ValueKind, RegisterValue::Kind::Unknown);
}

TEST(SBFStoredRegisterMemory,
     TransferPreservesMemmoveProvenanceAndClobbersCalls) {
  MachineState State;
  const RegisterValue Pointer{RegisterValue::Kind::StackAddress,
                              kStackStart + 4096, -32};
  std::array<uint8_t, 8> Bytes;
  llvm::support::endian::write64le(Bytes.data(), *effectiveAddress(Pointer, 0));
  State.Scratch.Memory.write(kStackStart, Bytes);
  State.Scratch.Values.write(kStackStart, Pointer);
  State.Registers[1] = {RegisterValue::Kind::Constant, kStackStart + 1, 0};
  State.Registers[2] = {RegisterValue::Kind::Constant, kStackStart, 0};
  State.Registers[3] = {RegisterValue::Kind::Constant, 8, 0};
  MedInstruction Move;
  Move.Call = CallKind::Syscall;
  Move.Syscall = getSyscallInfo(Syscall::Memmove);
  ASSERT_NE(Move.Syscall, nullptr);
  applyTransfer(Move, State, ProgramImage{}, ScratchLoadPolicy::ProvenValues);
  EXPECT_EQ(State.Scratch.Values.read(kStackStart + 1), Pointer);
  EXPECT_EQ(State.Scratch.Values.read(kStackStart), RegisterValue{});
  MedInstruction Call;
  Call.Call = CallKind::Internal;
  applyTransfer(Call, State, ProgramImage{}, ScratchLoadPolicy::ProvenValues);
  EXPECT_TRUE(State.Scratch.empty());
}

TEST(SBFStoredRegisterMemory, CopiesKnownWordsWithoutInventingOtherBytes) {
  const va_t Source = kStackStart + 1024;
  const va_t Destination = kStackStart + 2048;
  const RegisterValue Origin{RegisterValue::Kind::StackAddress, 2, 16};
  for (auto Which : {Syscall::Memcpy, Syscall::Memmove})
    for (uint64_t Length : std::array<uint64_t, 3>{7, 127, kMemoryRegionSize}) {
      MachineState State;
      State.Registers[1] = {RegisterValue::Kind::StackAddress, Destination, 0};
      State.Registers[2] = {RegisterValue::Kind::StackAddress, Source, 0};
      State.Registers[3] = {RegisterValue::Kind::Constant, Length, 0};
      State.Scratch.Values.write(Source, Origin,
                                 MemoryCoordinate::FrameRelative);
      const std::array<uint8_t, 1> Partial{0xa5};
      State.Scratch.Memory.write(Source, Partial);
      MedInstruction Copy;
      Copy.Call = CallKind::Syscall;
      Copy.Syscall = getSyscallInfo(Which);
      applyTransfer(Copy, State, ProgramImage{},
                    ScratchLoadPolicy::ProvenValues);
      EXPECT_EQ(State.Scratch.Values.read(Destination,
                                          MemoryCoordinate::FrameRelative),
                Length == 127 ? Origin : RegisterValue{});
      EXPECT_EQ(State.Scratch.Values.read(Destination + 8,
                                          MemoryCoordinate::FrameRelative),
                RegisterValue{});
      EXPECT_TRUE(State.Scratch.Memory.read(Destination, 1).empty());
    }
}

TEST(SBFStoredRegisterMemory, WrappingFrameStoreCoordinatesCannotProveAWord) {
  MachineState State;
  const va_t Frame = kStackStart + 4096;
  State.Registers[1] = {RegisterValue::Kind::StackAddress, Frame,
                        -static_cast<int64_t>(Frame) - 4};
  State.Registers[2] = {RegisterValue::Kind::Constant, 42, 0};
  MedInstruction Store;
  Store.Op = Operation::Store;
  Store.Form = OperandForm::StoreReg;
  Store.Width = kDoubleWordBitWidth;
  Store.Dst = 1;
  Store.Src = 2;
  const va_t Address = *effectiveAddress(State.Registers[1], 0);
  EXPECT_EQ(Address, UINT64_MAX - 3);
  applyTransfer(Store, State, ProgramImage{}, ScratchLoadPolicy::ProvenValues);
  EXPECT_EQ(State.Scratch.Values.read(Address, MemoryCoordinate::FrameRelative),
            RegisterValue{});
  EXPECT_TRUE(State.Scratch.empty());
}

TEST(SBFStoredRegisterMemory, FrameCoordinatesAreNotAbsoluteStackAddresses) {
  StoredRegisterMemory Memory;
  const RegisterValue Number{RegisterValue::Kind::Constant, 42, 0};
  Memory.write(kStackStart + 8, Number, MemoryCoordinate::FrameRelative);
  EXPECT_EQ(Memory.read(kStackStart + 8), RegisterValue{});
  EXPECT_EQ(Memory.read(kStackStart + 8, MemoryCoordinate::FrameRelative),
            Number);
  // Its numeric value alone cannot prove where an unknown-depth frame is.
  Memory.write(kHeapStart, Number);
  EXPECT_EQ(Memory.read(kStackStart + 8, MemoryCoordinate::FrameRelative),
            RegisterValue{});
  EXPECT_EQ(Memory.read(kHeapStart), Number);
  Memory.write(kStackStart + 8, Number, MemoryCoordinate::FrameRelative);
  EXPECT_EQ(Memory.read(kHeapStart), RegisterValue{});
}

TEST(SBFStoredRegisterMemory,
     AbstractFrameCannotAuthorizeAnImmutableImageLoad) {
  auto Program = makeTableProgram();
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  MachineState State;
  State.Registers[6] = {RegisterValue::Kind::StackAddress, kStackStart + 4096,
                        -static_cast<int64_t>(kStackStart + 4096) - 8};
  // [r6 + 8] has analysis coordinates zero, where this v3 image stores a
  // target. A different runtime call depth changes that address.
  applyTransfer(Program->Med.Instructions[8], State, Program->ExecutableImage,
                ScratchLoadPolicy::ProvenValues);
  EXPECT_EQ(State.Registers[1].ValueKind, RegisterValue::Kind::Unknown);
}

TEST(SBFStoredRegisterMemory, PreservesPointerKindAndRejectsPartialOverwrite) {
  StoredRegisterMemory Memory;
  const RegisterValue Pointer{RegisterValue::Kind::StackAddress,
                              kStackStart + 4096, -32};
  Memory.write(kStackStart + 8, Pointer);
  EXPECT_EQ(Memory.read(kStackStart + 8), Pointer);
  Memory.invalidate(kStackStart + 15, 1);
  EXPECT_EQ(Memory.read(kStackStart + 8), RegisterValue{});
  Memory.write(kStackStart + 8, Pointer);
  Memory.invalidate(kStackStart, 8);
  EXPECT_EQ(Memory.read(kStackStart + 8), Pointer);
}

TEST(SBFStoredRegisterMemory, SnapshotsOverlappingCopiesAndMeetsProvenance) {
  StoredRegisterMemory Memory;
  const RegisterValue Pointer{RegisterValue::Kind::StackAddress,
                              kStackStart + 4096, -32};
  const RegisterValue Number{RegisterValue::Kind::Constant, 99, 0};
  Memory.write(kStackStart, Pointer);
  Memory.write(kStackStart + 8, Number);
  Memory.copyFrom(Memory, kStackStart, kStackStart + 8, 16);
  EXPECT_EQ(Memory.read(kStackStart + 8), Pointer);
  EXPECT_EQ(Memory.read(kStackStart + 16), Number);
  StoredRegisterMemory Other = Memory;
  Other.write(kStackStart + 8, {RegisterValue::Kind::Constant,
                                *effectiveAddress(Pointer, 0), 0});
  Memory.meet(Other);
  EXPECT_EQ(Memory.read(kStackStart + 8), RegisterValue{});
  EXPECT_EQ(Memory.read(kStackStart + 16), Number);
}

TEST(SBFStoredRegisterMemory,
     ScratchLoadsUseTypedValuesRatherThanDescriptorBytes) {
  auto Program = makeTableProgram();
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  MachineState State;
  const RegisterValue Pointer{RegisterValue::Kind::StackAddress,
                              kStackStart + 4096, -32};
  State.Registers[6] = {RegisterValue::Kind::Constant, kStackStart + 8, 0};
  State.Scratch.Values.write(kStackStart + 8, Pointer);
  std::array<uint8_t, 8> Bytes;
  llvm::support::endian::write64le(Bytes.data(), *effectiveAddress(Pointer, 0));
  State.Scratch.Memory.write(kStackStart + 8, Bytes);
  const auto Load = Program->Med.Instructions[8];
  State.Registers[6].Value -= 8; // the instruction reads [r6 + 8]
  MachineState Ordinary = State;
  applyTransfer(Load, Ordinary, Program->ExecutableImage);
  EXPECT_EQ(Ordinary.Registers[1].ValueKind, RegisterValue::Kind::Unknown);
  applyTransfer(Load, State, Program->ExecutableImage,
                ScratchLoadPolicy::ProvenValues);
  EXPECT_EQ(State.Registers[1], Pointer);
}

} // namespace
} // namespace neverd::sbf
