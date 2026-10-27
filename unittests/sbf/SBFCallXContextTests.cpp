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

#include "SBFCallXMemoryEffects.h"
#include "SBFSolanaTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/sbf/analysis/SBFCallXContexts.h"
#include "neverd/sbf/analysis/SBFDataflow.h"
#include "neverd/sbf/image/SBFProgramImage.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/runtime/SBFInterpreter.h"
#include "neverd/sbf/runtime/SBFSemantics.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/sbf/runtime/SBFVMConfig.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <tuple>
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

struct AlternativeCallFixture {
  std::vector<test::EncodedInstruction> Text;
  std::vector<size_t> Literals;
  std::vector<size_t> Callees;
  std::vector<size_t> Targets;
  size_t FirstCall;
  size_t LastCall;
};

AlternativeCallFixture makeAlternativeCallees(size_t Count,
                                              unsigned CallRegister,
                                              bool Stores, bool Distinct,
                                              bool OmitsLastStore = false) {
  AlternativeCallFixture Fixture;
  auto &Text = Fixture.Text;
  const auto AppendLiteral = [&](unsigned Register, uint64_t Value) {
    const size_t Slot = Text.size();
    const auto Literal = test::loadImm64(Register, Value);
    Text.insert(Text.end(), Literal.begin(), Literal.end());
    return Slot;
  };
  AppendLiteral(4, kInputStart);
  Text.push_back(encode(Opcode::LD_DW_REG, 5, 4));
  if (Stores) {
    Text.push_back(encode(Opcode::MOV64_REG, 6, 10));
    Text.push_back(encode(Opcode::ADD64_IMM, 6, 0, 0, -16));
    Text.push_back(encode(Opcode::MOV64_REG, 1, 6));
  }
  std::vector<size_t> Branches, Joins;
  for (size_t I = 0; I + 1 < Count; ++I) {
    Branches.push_back(Text.size());
    Text.push_back({});
  }
  Fixture.Literals.resize(Count);
  // The fallthrough selects the last alternative; preceding comparisons select
  // every other one. The runtime input remains unknown to static analysis.
  for (size_t N = 0; N < Count; ++N) {
    const size_t I = (N + Count - 1) % Count;
    Fixture.Literals[I] = AppendLiteral(CallRegister, 0);
    Joins.push_back(Text.size());
    Text.push_back({});
  }
  Fixture.FirstCall = Text.size();
  Text.push_back(encode(Opcode::CALL_REG, CallRegister));
  Text.push_back(Stores ? encode(Opcode::LD_DW_REG, 2, 6)
                        : encode(Opcode::MOV64_REG, 2, 0));
  Fixture.LastCall = Text.size();
  Text.push_back(encode(Opcode::CALL_REG, 2));
  Text.push_back(encode(Opcode::EXIT));
  std::vector<std::optional<size_t>> ReturnLiterals;
  for (size_t I = 0; I < Count; ++I) {
    Fixture.Callees.push_back(Text.size());
    if (Stores && OmitsLastStore && I + 1 == Count) {
      ReturnLiterals.push_back(std::nullopt);
    } else {
      ReturnLiterals.push_back(AppendLiteral(Stores ? 2 : 0, 0));
      if (Stores)
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 2));
    }
    if (Stores)
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
  }
  for (size_t I = 0; I < (Distinct ? Count : 1); ++I) {
    Fixture.Targets.push_back(Text.size());
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 77 + I));
    Text.push_back(encode(Opcode::EXIT));
  }
  for (size_t I = 0; I < Branches.size(); ++I)
    Text[Branches[I]] = encode(Opcode::JEQ64_IMM, 5, 0,
                               Fixture.Literals[I] - Branches[I] - 1, I);
  for (size_t Join : Joins)
    Text[Join] = encode(Opcode::JA, 0, 0, Fixture.FirstCall - Join - 1);
  for (size_t I = 0; I < Count; ++I) {
    const auto Literal = test::loadImm64(
        CallRegister, kBytecodeStart + Fixture.Callees[I] * kInstructionSize);
    std::copy(Literal.begin(), Literal.end(),
              Text.begin() + Fixture.Literals[I]);
    if (ReturnLiterals[I]) {
      const auto Return = test::loadImm64(
          Stores ? 2 : 0, kBytecodeStart + Fixture.Targets[Distinct ? I : 0] *
                                               kInstructionSize);
      std::copy(Return.begin(), Return.end(),
                Text.begin() + *ReturnLiterals[I]);
    }
  }
  return Fixture;
}

llvm::Expected<ExecutionResult>
executeSelector(const SBFProgram &Program, size_t Selector,
                InterpreterOptions Options = {}) {
  ExecutionEnvironment Environment;
  std::vector<uint8_t> Input(sizeof(uint64_t));
  llvm::support::endian::write64le(Input.data(), Selector);
  Environment.Memory.push_back({kInputStart, Input, false, "selector"});
  return executeRaw(Program, std::move(Environment), Options);
}

void checkSummaryReferences(const CallXContextReport &Report) {
  std::map<size_t, size_t> Variants;
  std::set<size_t> IDs;
  for (const auto &Summary : Report.Summaries) {
    EXPECT_TRUE(IDs.insert(Summary.ID).second);
    if (!Summary.GeneralizedInputs)
      EXPECT_LE(++Variants[Summary.EntrySlot],
                kCallXContextSpecializationsPerEntry);
  }
  for (const auto &Edge : Report.SummaryEdges) {
    EXPECT_TRUE(IDs.contains(Edge.CallerSummaryID));
    if (Edge.CalleeSummaryID)
      EXPECT_TRUE(IDs.contains(*Edge.CalleeSummaryID));
  }
  for (const auto &Expression : Report.InputExpressions)
    if (Expression.MemoryGuard)
      EXPECT_LT(*Expression.MemoryGuard, Report.InputMemoryGuards.size());
}

TEST(SBFCallXContexts, JoinsEveryFiniteCalleeReturnAndMemoryOutcome) {
  for (size_t Count : {2u, 4u})
    for (unsigned Register : {0u, 7u})
      for (bool Stores : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "count=" << Count << ", register="
                                          << Register << ", stores=" << Stores);
        const auto Fixture =
            makeAlternativeCallees(Count, Register, Stores, false);
        auto Program = analyze(test::makeImage(Fixture.Text, {}));
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        EXPECT_TRUE(std::any_of(Report->Contexts.begin(),
                                Report->Contexts.end(), [&](const auto &Site) {
                                  return Site.CallXSlot == Fixture.LastCall &&
                                         Site.TargetSlot == Fixture.Targets[0];
                                }));
        std::set<size_t> Callees;
        for (const auto &Edge : Report->SummaryEdges)
          if (Edge.CallerSlot == Fixture.FirstCall && Edge.CalleeEntrySlot) {
            Callees.insert(*Edge.CalleeEntrySlot);
            EXPECT_EQ(Edge.CallXTargetAddress,
                      kBytecodeStart +
                          *Edge.CalleeEntrySlot * kInstructionSize);
          }
        EXPECT_EQ(Callees, std::set<size_t>(Fixture.Callees.begin(),
                                            Fixture.Callees.end()));
        checkSummaryReferences(*Report);
        bool CompleteSet = false;
        for (const auto &Site : Report->Contexts)
          if (Site.CallXSlot == Fixture.FirstCall) {
            EXPECT_EQ(Site.Result, CallXContextResult::ProvenTargetSet);
            EXPECT_EQ(Site.TargetValueAlternatives.values().size(), Count);
            EXPECT_FALSE(Site.TargetSlot);
            EXPECT_EQ(Site.Missing.Kind, CallXMissingFactKind::None);
            CompleteSet = true;
          }
        EXPECT_TRUE(CompleteSet);
        const auto JSON = callXContextReportJSON(*Program, *Report);
        EXPECT_EQ(JSON.getInteger("schema"), 6);
        const auto *Frame = JSON.getObject("frame_coordinates");
        ASSERT_NE(Frame, nullptr);
        const auto Translation =
            frameTranslationLimit(Program->Low.TheVersion, Program->Config);
        EXPECT_EQ(Frame->getInteger("maximum_translation_bytes"), Translation);
        EXPECT_EQ(JSON.getInteger("callx_sites_with_proven_context"), 1);
        EXPECT_EQ(JSON.getInteger("callx_sites_with_proven_target_set_context"),
                  1);
        for (size_t Selector = 0; Selector < Count; ++Selector) {
          auto Raw = executeSelector(*Program, Selector);
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, 77u);
          if (Register >= kFirstCalleeSavedRegister)
            EXPECT_EQ(Raw->Registers[Register],
                      kBytecodeStart +
                          Fixture.Callees[Selector] * kInstructionSize);
          bool SawFirst = false, SawLast = false;
          for (size_t I = 0; I + 1 < Raw->Trace.size(); ++I) {
            if (Raw->Trace[I].Slot == Fixture.FirstCall) {
              EXPECT_EQ(Raw->Trace[I + 1].Slot, Fixture.Callees[Selector]);
              SawFirst = true;
            }
            if (Raw->Trace[I].Slot == Fixture.LastCall) {
              EXPECT_EQ(Raw->Trace[I + 1].Slot, Fixture.Targets[0]);
              SawLast = true;
            }
          }
          EXPECT_TRUE(SawFirst);
          EXPECT_TRUE(SawLast);
        }
      }
}

TEST(SBFCallXContexts, DistinctFiniteReturnsAndStoresRemainCompleteSets) {
  for (size_t Count : {2u, 4u})
    for (bool Stores : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "count=" << Count << ", stores=" << Stores);
      const auto Fixture = makeAlternativeCallees(Count, 7, Stores, true);
      auto Program = analyze(test::makeImage(Fixture.Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      bool SawLast = false;
      for (const auto &Site : Report->Contexts) {
        EXPECT_FALSE(Site.TargetSlot);
        if (Site.CallXSlot == Fixture.LastCall) {
          EXPECT_EQ(Site.Result, CallXContextResult::ProvenTargetSet);
          EXPECT_EQ(Site.TargetValueAlternatives.values().size(), Count);
          SawLast = true;
        }
      }
      EXPECT_TRUE(SawLast);
      const auto JSON = callXContextReportJSON(*Program, *Report);
      EXPECT_EQ(JSON.getInteger("callx_sites_with_proven_context"), 0);
      EXPECT_EQ(JSON.getInteger("callx_sites_with_proven_target_set_context"),
                2);
      EXPECT_TRUE(*JSON.getBoolean("dynamic_fallback_required"));
      checkSummaryReferences(*Report);
      for (size_t Selector = 0; Selector < Count; ++Selector) {
        auto Raw = executeSelector(*Program, Selector);
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, 77u + Selector);
      }
    }
}

TEST(SBFCallXContexts, MissingStoreInOneAlternativeDoesNotInventAMustValue) {
  const auto Fixture = makeAlternativeCallees(2, 7, true, false, true);
  auto Program = analyze(test::makeImage(Fixture.Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  bool SawLast = false;
  for (const auto &Site : Report->Contexts)
    if (Site.CallXSlot == Fixture.LastCall) {
      EXPECT_EQ(Site.Result, CallXContextResult::UnknownTarget);
      EXPECT_FALSE(Site.TargetSlot);
      SawLast = true;
    }
  EXPECT_TRUE(SawLast);
  checkSummaryReferences(*Report);
  for (size_t Selector : {0u, 1u}) {
    auto Raw = executeSelector(*Program, Selector);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    if (Selector == 0) {
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 77u);
    } else {
      EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted);
      EXPECT_EQ(Raw->Fault, FaultCode::UnknownIndirectCall);
    }
  }
}

TEST(SBFCallXContexts, UnknownAndOverflowTargetsNeverSelectAKnownSubset) {
  for (bool Unknown : {false, true}) {
    const size_t Count = Unknown ? 2 : kCallXContextValueAlternatives + 1;
    auto Fixture = makeAlternativeCallees(Count, 7, false, false);
    if (Unknown) {
      Fixture.Text[Fixture.Literals.back()] = encode(Opcode::MOV64_REG, 7, 3);
      Fixture.Text[Fixture.Literals.back() + 1] =
          encode(Opcode::MOV64_REG, 7, 7);
    }
    auto Program = analyze(test::makeImage(Fixture.Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    for (const auto &Site : Report->Contexts) {
      EXPECT_EQ(Site.Result, CallXContextResult::UnknownTarget);
      EXPECT_TRUE(Site.TargetValueAlternatives.unknown());
    }
    for (const auto &Edge : Report->SummaryEdges)
      if (Edge.CallerSlot == Fixture.FirstCall) {
        EXPECT_EQ(Edge.Barrier, CallXSummaryBarrier::UnknownTarget);
        EXPECT_FALSE(Edge.CalleeSummaryID);
        EXPECT_FALSE(Edge.CallXTargetAddress);
      }
    checkSummaryReferences(*Report);
    for (size_t Selector = 0; Selector < Count; ++Selector) {
      auto Raw = executeSelector(*Program, Selector);
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      if (Unknown && Selector + 1 == Count) {
        EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted);
        EXPECT_EQ(Raw->Fault, FaultCode::UnknownIndirectCall);
      } else {
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, 77u);
      }
    }
  }
}

TEST(SBFCallXContexts, UnusableRawAlternativesRetainConservativeEffects) {
  for (unsigned Kind : {0u, 1u, 2u}) {
    auto Fixture = makeAlternativeCallees(2, 0, false, false);
    const va_t Address =
        Kind == 0 ? UINT64_MAX
                  : kBytecodeStart +
                        (Fixture.Callees.back() + Kind) * kInstructionSize;
    const auto Literal = test::loadImm64(0, Address);
    std::copy(Literal.begin(), Literal.end(),
              Fixture.Text.begin() + Fixture.Literals.back());
    auto Program = analyze(test::makeImage(Fixture.Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    bool SawBarrier = false;
    for (const auto &Edge : Report->SummaryEdges)
      if (Edge.CallerSlot == Fixture.FirstCall &&
          Edge.CallXTargetAddress == Address) {
        EXPECT_FALSE(Edge.CalleeSummaryID);
        EXPECT_EQ(Edge.Barrier, Kind == 0
                                    ? CallXSummaryBarrier::UnknownTarget
                                    : CallXSummaryBarrier::UnsupportedRawEntry);
        SawBarrier = true;
      }
    EXPECT_TRUE(SawBarrier);
    for (const auto &Site : Report->Contexts) {
      EXPECT_FALSE(Site.TargetSlot);
      if (Site.CallXSlot == Fixture.FirstCall)
        EXPECT_EQ(Site.Result, Kind == 0 ? CallXContextResult::UnknownTarget
                                         : CallXContextResult::ProvenTargetSet);
      else
        EXPECT_EQ(Site.Result, CallXContextResult::UnknownTarget);
    }
    checkSummaryReferences(*Report);
    for (size_t Selector : {0u, 1u}) {
      auto Raw = executeSelector(*Program, Selector);
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      if (Selector == 0 || Kind == 2) {
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, Selector == 0 ? 77u : Address);
      } else {
        EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted);
        EXPECT_EQ(Raw->Fault, Kind == 0 ? FaultCode::UnknownIndirectCall
                                        : FaultCode::InvalidInstruction);
      }
    }
  }
}

TEST(SBFCallXContexts, UnalignedAlternativesKeepTheirActualRegisterInputs) {
  auto Fixture = makeAlternativeCallees(2, 7, false, false);
  const size_t Callee = Fixture.Callees[0];
  const va_t Address = kBytecodeStart + Callee * kInstructionSize;
  const auto Unaligned = test::loadImm64(7, Address + 1);
  std::copy(Unaligned.begin(), Unaligned.end(),
            Fixture.Text.begin() + Fixture.Literals[1]);
  // Returning r7 distinguishes the original addresses even though both raw
  // calls floor to the same entry. Refining only the PC would lose this fact.
  Fixture.Text[Callee] = encode(Opcode::MOV64_REG, 0, 7);
  Fixture.Text[Callee + 1] = encode(Opcode::MOV64_REG, 0, 0);
  auto Program = analyze(test::makeImage(Fixture.Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  std::set<va_t> Inputs;
  for (const auto &Edge : Report->SummaryEdges)
    if (Edge.CallerSlot == Fixture.FirstCall && Edge.CalleeSummaryID) {
      EXPECT_EQ(Edge.CalleeEntrySlot, Callee);
      ASSERT_TRUE(Edge.CallXTargetAddress);
      Inputs.insert(*Edge.CallXTargetAddress);
      const auto It =
          std::find_if(Report->Summaries.begin(), Report->Summaries.end(),
                       [&](const auto &Summary) {
                         return Summary.ID == *Edge.CalleeSummaryID;
                       });
      ASSERT_NE(It, Report->Summaries.end());
      if (!It->GeneralizedInputs) {
        EXPECT_EQ(It->Inputs[7].ValueKind, RegisterValue::Kind::Constant);
        EXPECT_EQ(It->Inputs[7].Value, *Edge.CallXTargetAddress);
      }
    }
  EXPECT_EQ(Inputs, (std::set<va_t>{Address, Address + 1}));
  bool SawLast = false;
  for (const auto &Site : Report->Contexts) {
    EXPECT_FALSE(Site.TargetSlot);
    if (Site.CallXSlot == Fixture.LastCall) {
      EXPECT_EQ(Site.Result, CallXContextResult::ProvenTargetSet);
      EXPECT_EQ(Site.TargetValueAlternatives.values().size(), 2u);
      SawLast = true;
    }
  }
  EXPECT_TRUE(SawLast);
  const auto JSON = callXContextReportJSON(*Program, *Report);
  EXPECT_EQ(JSON.getInteger("callx_sites_with_proven_context"), 0);
  EXPECT_EQ(JSON.getInteger("callx_sites_with_proven_target_set_context"), 2);
  for (const auto &Site : *JSON.getArray("callx_sites")) {
    const auto &Row = *Site.getAsObject();
    EXPECT_TRUE(Row.getArray("conditional_target_slots")->empty());
    for (const auto &Set : *Row.getArray("conditional_target_sets")) {
      ASSERT_EQ(Set.getAsArray()->size(), 2u);
      EXPECT_EQ((*Set.getAsArray())[0].getAsObject()->getInteger("target_slot"),
                Callee);
      EXPECT_EQ((*Set.getAsArray())[1].getAsObject()->getInteger("target_slot"),
                Callee);
      EXPECT_NE(
          (*Set.getAsArray())[0].getAsObject()->getString("target_address"),
          (*Set.getAsArray())[1].getAsObject()->getString("target_address"));
    }
  }
  checkSummaryReferences(*Report);
  for (size_t Selector : {0u, 1u}) {
    auto Raw = executeSelector(*Program, Selector);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, Address + Selector);
  }
}

TEST(SBFCallXContexts, JoinsOnlyReturningAlternativesWithoutInventingAReturn) {
  for (bool AllLoop : {false, true}) {
    auto Fixture = makeAlternativeCallees(2, 7, false, false);
    for (size_t I = AllLoop ? 0 : 1; I < Fixture.Callees.size(); ++I) {
      Fixture.Text[Fixture.Callees[I]] = encode(Opcode::JA, 0, 0, -1);
      Fixture.Text[Fixture.Callees[I] + 1] = encode(Opcode::MOV64_REG, 0, 0);
    }
    auto Program = analyze(test::makeImage(Fixture.Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    bool SawLast = false;
    for (const auto &Site : Report->Contexts)
      if (Site.CallXSlot == Fixture.LastCall) {
        EXPECT_EQ(Site.TargetSlot, Fixture.Targets[0]);
        SawLast = true;
      }
    EXPECT_EQ(SawLast, !AllLoop);
    checkSummaryReferences(*Report);
    for (size_t Selector : {0u, 1u}) {
      InterpreterOptions Options;
      Options.MaxSteps = 100;
      auto Raw = executeSelector(*Program, Selector, Options);
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      if (AllLoop || Selector == 1) {
        EXPECT_EQ(Raw->Status, ExecutionStatus::StepLimit);
        EXPECT_FALSE(std::any_of(
            Raw->Trace.begin(), Raw->Trace.end(),
            [&](const auto &Step) { return Step.Slot == Fixture.LastCall; }));
      } else {
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, 77u);
      }
    }
  }
}

TEST(SBFCallXContexts, FiniteAlternativesRetainRecursiveAndDepthBarriers) {
  for (bool Recursive : {false, true}) {
    auto Fixture = makeAlternativeCallees(2, 7, false, false);
    if (Recursive) {
      const auto Literal = test::loadImm64(7, kBytecodeStart);
      std::copy(Literal.begin(), Literal.end(),
                Fixture.Text.begin() + Fixture.Literals.back());
    }
    auto Program = analyze(test::makeImage(Fixture.Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    if (!Recursive)
      Program->Config.MaxCallDepth = 2;
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    bool SawBarrier = false;
    for (const auto &Edge : Report->SummaryEdges)
      if (Edge.CallerSlot == Fixture.FirstCall &&
          (!Recursive || Edge.CallXTargetAddress == kBytecodeStart)) {
        EXPECT_EQ(Edge.Barrier, Recursive ? CallXSummaryBarrier::RecursiveCall
                                          : CallXSummaryBarrier::CallDepth);
        EXPECT_FALSE(Edge.CalleeSummaryID);
        SawBarrier = true;
      }
    EXPECT_TRUE(SawBarrier);
    for (const auto &Site : Report->Contexts)
      if (Site.CallXSlot == Fixture.LastCall)
        EXPECT_EQ(Site.Result, CallXContextResult::UnknownTarget);
    checkSummaryReferences(*Report);
    for (size_t Selector : {0u, 1u}) {
      auto Raw = executeSelector(*Program, Selector);
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      if (Recursive && Selector == 1) {
        EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted);
        EXPECT_EQ(Raw->Fault, FaultCode::CallDepth);
      } else {
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, 77u);
      }
    }
  }
}

TEST(SBFCallXContexts, ProvenCalleeDependenciesAvoidUnusedCallerVariants) {
  for (unsigned Layers : {0u, 2u}) {
    SCOPED_TRACE(Layers);
    constexpr uint32_t TableAddress = 0x10000;
    const size_t Repeats = kCallXContextSpecializationsPerEntry + 2;
    std::vector<test::EncodedInstruction> Text;
    std::vector<size_t> Calls;
    for (size_t I = 0; I <= Repeats; ++I) {
      Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0,
                            TableAddress + (I == Repeats ? 8 : 0)));
      Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I + 1));
      Calls.push_back(Text.size());
      Text.push_back({});
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Leaf = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    Text.push_back({});
    Text.push_back(encode(Opcode::LD_DW_REG, 2, 6));
    Text.push_back(encode(Opcode::CALL_REG, 2));
    Text.push_back(encode(Opcode::EXIT));
    const size_t FirstTarget = Text.size();
    for (int32_t Result : {41, 42}) {
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, Result));
      Text.push_back(encode(Opcode::EXIT));
    }
    const size_t Primer = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    Text[Leaf + 1] = encode(Opcode::CALL_IMM, 0, 1, 0, Primer - Leaf - 2);
    std::vector<size_t> Entries{Leaf};
    for (unsigned Layer = 0; Layer < Layers; ++Layer) {
      const size_t Entry = Text.size();
      Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0,
                            static_cast<int32_t>(Entries.back()) - Entry - 1));
      Text.push_back(encode(Opcode::EXIT));
      Entries.push_back(Entry);
    }
    for (size_t Call : Calls)
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Entries.back() - Call - 1);
    std::array<uint8_t, 16> Table;
    llvm::support::endian::write64le(Table.data(),
                                     kBytecodeStart + FirstTarget * 8);
    llvm::support::endian::write64le(Table.data() + 8,
                                     kBytecodeStart + (FirstTarget + 2) * 8);
    auto Image = test::makeImage(Text, Table);
    Image.Sections[1].VA = TableAddress;
    Image.SBF->RodataVM = {TableAddress, Table.size()};
    auto Program = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    std::set<size_t> Targets;
    for (const auto &Site : Report->Contexts)
      if (!Site.EntryPrefixOnly && Site.CallXSlot == Leaf + 3 &&
          Site.TargetSlot)
        Targets.insert(*Site.TargetSlot);
    EXPECT_EQ(Targets, (std::set<size_t>{FirstTarget, FirstTarget + 2}));
    EXPECT_GT(Report->EquivalentSpecializedReuses, 0u);
    for (size_t Entry : Entries) {
      std::set<size_t> Variants;
      for (const auto &Summary : Report->Summaries)
        if (Summary.EntrySlot == Entry && !Summary.GeneralizedInputs &&
            Summary.Inputs[1].ValueKind == RegisterValue::Kind::Constant &&
            (Summary.Inputs[1].Value == TableAddress ||
             Summary.Inputs[1].Value == TableAddress + 8)) {
          Variants.insert(Summary.ID);
          EXPECT_EQ(Summary.ReuseInputRegisterMask,
                    (1u << 1) | (1u << kFramePointerRegister));
          EXPECT_TRUE(Summary.ReuseReadsInputMemory);
        }
      EXPECT_EQ(Variants.size(), 2u) << Entry;
    }
    checkSummaryReferences(*Report);
    auto Raw = executeRaw(*Program);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, 42u);
  }
}

TEST(SBFCallXContexts,
     DefinedInputJoinsRemainSoundWhenOnlyDefinitionBitsChange) {
  for (bool Loop : {false, true})
    for (bool ReverseEdges : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "loop=" << Loop << ", reverse=" << ReverseEdges);
      std::vector<test::EncodedInstruction> Text;
      std::vector<size_t> Arguments, StableArguments, Calls;
      for (unsigned I = 0; I < 2; ++I) {
        const auto Input = test::loadImm64(4, kInputStart);
        Text.insert(Text.end(), Input.begin(), Input.end());
        Text.push_back(encode(Opcode::LD_DW_REG, 5, 4));
        if (Loop)
          Text.push_back(encode(Opcode::MOV64_IMM, 8, 0, 0, 1));
        StableArguments.push_back(Text.size());
        const auto Stable = test::loadImm64(3, 0);
        Text.insert(Text.end(), Stable.begin(), Stable.end());
        Arguments.push_back(Text.size());
        const auto Argument = test::loadImm64(9, 0);
        Text.insert(Text.end(), Argument.begin(), Argument.end());
        Calls.push_back(Text.size());
        Text.push_back({});
      }
      Text.push_back(encode(Opcode::EXIT));
      const size_t Leaf = Text.size();
      Text.push_back({});
      const size_t OtherBranch = Text.size();
      Text.push_back({});
      const size_t SkipHead = Text.size();
      Text.push_back({});
      const size_t SetLiteral = Text.size();
      const auto Set = test::loadImm64(9, 0);
      Text.insert(Text.end(), Set.begin(), Set.end());
      const size_t SetJump = Text.size();
      Text.push_back({});
      const size_t SetFromOther = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 9, 3));
      Text.push_back(encode(Opcode::JA));
      const size_t Join = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 2));
      Text.push_back(encode(Opcode::JA));
      const size_t InnerCall = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 9));
      std::optional<size_t> LoopGuard;
      if (Loop) {
        LoopGuard = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::MOV64_IMM, 8));
        const size_t Backedge = Text.size();
        Text.push_back(encode(Opcode::JA, 0, 0,
                              static_cast<int16_t>(Join - Backedge - 1)));
      }
      const size_t Exit = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t Skip = Text.size();
      // The first two predecessors both write the same value, with different
      // origins, meeting the origin to unknown while keeping r9 defined. The
      // delayed third predecessor changes only that definition guarantee.
      for (unsigned I = 0; I < 6; ++I)
        Text.push_back(encode(Opcode::JA));
      const size_t SkipEnd = Text.size();
      Text.push_back(
          encode(Opcode::JA, 0, 0, static_cast<int16_t>(Join - SkipEnd - 1)));
      Text[Leaf] = encode(Opcode::JEQ64_IMM, 5, 0, SetLiteral - Leaf - 1);
      Text[OtherBranch] =
          encode(Opcode::JEQ64_IMM, 5, 0, SetFromOther - OtherBranch - 1, 1);
      Text[SetJump] = encode(Opcode::JA, 0, 0, Join - SetJump - 1);
      Text[SkipHead] = encode(Opcode::JA, 0, 0, Skip - SkipHead - 1);
      if (LoopGuard)
        Text[*LoopGuard] =
            encode(Opcode::JEQ64_IMM, 8, 0, Exit - *LoopGuard - 1);
      const size_t Targets = Text.size();
      for (int32_t Result : {41, 42}) {
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, Result));
        Text.push_back(encode(Opcode::EXIT));
      }
      const auto First = test::loadImm64(9, kBytecodeStart + Targets * 8);
      std::copy(First.begin(), First.end(), Text.begin() + SetLiteral);
      for (size_t I = 0; I < Calls.size(); ++I) {
        const auto Stable = test::loadImm64(3, kBytecodeStart + Targets * 8);
        std::copy(Stable.begin(), Stable.end(),
                  Text.begin() + StableArguments[I]);
        const auto Argument =
            test::loadImm64(9, kBytecodeStart + (Targets + I * 2) * 8);
        std::copy(Argument.begin(), Argument.end(),
                  Text.begin() + Arguments[I]);
        Text[Calls[I]] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Calls[I] - 1);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (ReverseEdges)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      bool SawSecond = false;
      for (const auto &Site : Report->Contexts)
        if (!Site.EntryPrefixOnly && !Site.GeneralizedInputs &&
            Site.CallXSlot == InnerCall && Site.CallerSlot == Calls[1]) {
          EXPECT_FALSE(Site.TargetSlot);
          EXPECT_EQ(Site.Result, CallXContextResult::ProvenTargetSet);
          EXPECT_EQ(Site.TargetValueAlternatives.values().size(), 2u);
          SawSecond = true;
        }
      EXPECT_TRUE(SawSecond);
      for (const auto &Summary : Report->Summaries)
        if (Summary.EntrySlot == Leaf)
          EXPECT_NE(Summary.ReuseInputRegisterMask & (1u << 9), 0u);
      checkSummaryReferences(*Report);
      for (size_t Selector : {0u, 1u, 2u}) {
        auto Raw = executeSelector(*Program, Selector);
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, Selector == 2 ? 42u : 41u);
      }
    }
}

TEST(SBFCallXContexts, EveryFiniteAlternativeContributesInputDependencies) {
  auto Fixture = makeAlternativeCallees(2, 7, false, false);
  Fixture.Text[Fixture.LastCall] = encode(Opcode::EXIT);
  const size_t ReadsNine = Fixture.Callees[1];
  Fixture.Text[ReadsNine] = encode(Opcode::MOV64_REG, 0, 9);
  Fixture.Text[ReadsNine + 1] = encode(Opcode::MOV64_REG, 0, 0);
  const size_t SecondTarget = Fixture.Text.size();
  Fixture.Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 78));
  Fixture.Text.push_back(encode(Opcode::EXIT));
  const size_t Entry = Fixture.Text.size();
  std::vector<size_t> Calls, CallXs;
  for (size_t Target : {Fixture.Targets[0], SecondTarget}) {
    const auto Argument = test::loadImm64(9, kBytecodeStart + Target * 8);
    Fixture.Text.insert(Fixture.Text.end(), Argument.begin(), Argument.end());
    Calls.push_back(Fixture.Text.size());
    Fixture.Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0,
                                  -static_cast<int32_t>(Calls.back()) - 1));
    Fixture.Text.push_back(encode(Opcode::MOV64_REG, 2, 0));
    CallXs.push_back(Fixture.Text.size());
    Fixture.Text.push_back(encode(Opcode::CALL_REG, 2));
  }
  Fixture.Text.push_back(encode(Opcode::EXIT));
  auto Image = test::makeImage(Fixture.Text, {});
  Image.Entry = kBytecodeStart + Entry * 8;
  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  bool SawSecond = false;
  for (const auto &Site : Report->Contexts)
    if (Site.CallXSlot == CallXs[1] && !Site.EntryPrefixOnly &&
        !Site.GeneralizedInputs) {
      EXPECT_EQ(Site.Result, CallXContextResult::ProvenTargetSet);
      EXPECT_FALSE(Site.TargetSlot);
      SawSecond = true;
    }
  EXPECT_TRUE(SawSecond);
  for (const auto &Summary : Report->Summaries)
    if (Summary.EntrySlot == 0)
      EXPECT_NE(Summary.ReuseInputRegisterMask & (1u << 9), 0u);
  checkSummaryReferences(*Report);
  for (size_t Selector : {0u, 1u}) {
    auto Raw = executeSelector(*Program, Selector);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, Selector == 0 ? 77u : 78u);
  }
}

TEST(SBFCallXContexts, ConditionalReuseKeepsTheActualCallersScratch) {
  for (bool ReadsMemory : {false, true}) {
    SCOPED_TRACE(ReadsMemory);
    std::vector<test::EncodedInstruction> Text;
    std::vector<size_t> Literals, Callees, Calls, CallXs;
    for (unsigned I = 0; I < 2; ++I) {
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
      Literals.push_back(Text.size());
      const auto Literal = test::loadImm64(3, 0);
      Text.insert(Text.end(), Literal.begin(), Literal.end());
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 3));
      Callees.push_back(Text.size());
      const auto Callee = test::loadImm64(2, 0);
      Text.insert(Text.end(), Callee.begin(), Callee.end());
      Calls.push_back(Text.size());
      Text.push_back({});
      Text.push_back(ReadsMemory ? encode(Opcode::MOV64_REG, 2, 0)
                                 : encode(Opcode::LD_DW_REG, 2, 0));
      CallXs.push_back(Text.size());
      Text.push_back(encode(Opcode::CALL_REG, 2));
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Leaf = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    Text.push_back(encode(Opcode::MOV64_REG, 7, 2));
    Text.push_back(encode(Opcode::CALL_REG, 7));
    Text.push_back(ReadsMemory ? encode(Opcode::LD_DW_REG, 0, 6)
                               : encode(Opcode::MOV64_REG, 0, 6));
    Text.push_back(encode(Opcode::EXIT));
    const size_t NoMemory = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Targets = Text.size();
    for (int32_t Value : {41, 42}) {
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, Value));
      Text.push_back(encode(Opcode::EXIT));
    }
    for (size_t I = 0; I < Calls.size(); ++I) {
      const auto Target =
          test::loadImm64(3, kBytecodeStart + (Targets + I * 2) * 8);
      std::copy(Target.begin(), Target.end(), Text.begin() + Literals[I]);
      const auto Callee = test::loadImm64(2, kBytecodeStart + NoMemory * 8);
      std::copy(Callee.begin(), Callee.end(), Text.begin() + Callees[I]);
      Text[Calls[I]] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Calls[I] - 1);
    }
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    std::set<size_t> Variants;
    for (const auto &Edge : Report->SummaryEdges)
      if (Edge.CalleeEntrySlot == Leaf && Edge.CalleeSummaryID &&
          !Edge.GeneralizedInputs)
        Variants.insert(*Edge.CalleeSummaryID);
    EXPECT_EQ(Variants.size(), ReadsMemory ? 2u : 1u);
    for (size_t I = 0; I < CallXs.size(); ++I)
      EXPECT_TRUE(std::any_of(Report->Contexts.begin(), Report->Contexts.end(),
                              [&](const auto &Site) {
                                return Site.CallXSlot == CallXs[I] &&
                                       !Site.EntryPrefixOnly &&
                                       Site.TargetSlot == Targets + I * 2;
                              }));
    checkSummaryReferences(*Report);
    auto Raw = executeRaw(*Program);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, 42u);
  }
}

TEST(SBFCallXContexts, ConditionalReusePreservesTheSymbolicFrameAnchor) {
  const auto Literal = test::loadImm64(7, 0);
  std::vector<test::EncodedInstruction> Text(Literal.begin(), Literal.end());
  for (const auto &I :
       {encode(Opcode::MOV64_REG, 6, 10),
        encode(Opcode::ADD64_IMM, 6, 0, 0, -8), encode(Opcode::ST_DW_IMM, 6),
        encode(Opcode::MOV64_REG, 1, 6)})
    Text.push_back(I);
  const size_t ShallowCall = Text.size();
  Text.push_back({});
  // Give the deeper call the same argument and scratch snapshot. Only the
  // callee frame anchor differs; the body never reads the machine r10.
  Text.push_back(encode(Opcode::ST_DW_IMM, 6));
  Text.push_back(encode(Opcode::MOV64_REG, 1, 6));
  const size_t WrapperCall = Text.size();
  Text.push_back({});
  Text.push_back(encode(Opcode::EXIT));
  const size_t Wrapper = Text.size();
  const size_t DeepCall = Text.size();
  Text.push_back({});
  const size_t StoredLiteral = Text.size();
  Text.insert(Text.end(), Literal.begin(), Literal.end());
  Text.push_back(encode(Opcode::ST_DW_REG, 0, 2));
  Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -8));
  const size_t UnwrittenCall = Text.size();
  Text.push_back(encode(Opcode::CALL_REG, 0, 0, 0, 2));
  Text.push_back(encode(Opcode::EXIT));
  const size_t Leaf = Text.size();
  Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
  Text.push_back(encode(Opcode::CALL_REG, 0, 0, 0, 7));
  Text.push_back(encode(Opcode::MOV64_REG, 0, 6));
  Text.push_back(encode(Opcode::EXIT));
  const size_t Pure = Text.size();
  Text.push_back(encode(Opcode::MOV64_IMM, 0));
  Text.push_back(encode(Opcode::EXIT));
  const size_t Target = Text.size();
  Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
  Text.push_back(encode(Opcode::EXIT));
  for (size_t Call : {ShallowCall, DeepCall})
    Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Call - 1);
  Text[WrapperCall] =
      encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - WrapperCall - 1);
  const auto PureAddress = test::loadImm64(7, kBytecodeStart + Pure * 8);
  std::copy(PureAddress.begin(), PureAddress.end(), Text.begin());
  const auto TargetAddress = test::loadImm64(2, kBytecodeStart + Target * 8);
  std::copy(TargetAddress.begin(), TargetAddress.end(),
            Text.begin() + StoredLiteral);
  auto Image = test::makeImage(Text, {});
  Image.SBF->Version = Version::V0;
  Image.SBF->ELFFlags = 0;
  Image.SBF->StrictLayout = false;
  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  std::set<size_t> Variants;
  for (const auto &Summary : Report->Summaries)
    if (Summary.EntrySlot == Leaf && !Summary.GeneralizedInputs &&
        Summary.Inputs[1].ValueKind == RegisterValue::Kind::StackAddress &&
        Summary.Inputs[7].ValueKind == RegisterValue::Kind::Constant)
      Variants.insert(Summary.ID);
  EXPECT_EQ(Variants.size(), 2u);
  bool SawUnwritten = false;
  for (const auto &Site : Report->Contexts)
    if (Site.CallXSlot == UnwrittenCall) {
      EXPECT_FALSE(Site.TargetSlot);
      EXPECT_EQ(Site.Result, CallXContextResult::UnknownTarget);
      SawUnwritten = true;
    }
  EXPECT_TRUE(SawUnwritten);
  checkSummaryReferences(*Report);
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted);
  EXPECT_EQ(Raw->FinalSlot, UnwrittenCall);
}

TEST(SBFCallXContexts, GenericTargetInputsSurviveSpecializationCapacity) {
  for (unsigned Mode : {0u, 1u, 2u}) {
    SCOPED_TRACE(Mode);
    const bool Fields = Mode == 1;
    const bool Identity = Mode == 2;
    constexpr uint32_t TableAddress = 0x10000;
    const size_t Count = kCallXContextSpecializationsPerEntry + 2;
    std::vector<test::EncodedInstruction> Text;
    std::vector<size_t> Literals, Calls;
    for (size_t I = 0; I < Count; ++I) {
      if (Fields) {
        Text.push_back(encode(Opcode::MOV64_REG, 2, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0,
                              -64 * static_cast<int32_t>(I + 1)));
        Text.push_back(
            encode(Opcode::MOV64_IMM, 3, 0, 0, TableAddress + I * 32));
        Text.push_back(encode(Opcode::ST_DW_REG, 2, 3, 40));
      } else {
        Literals.push_back(Text.size());
        const auto Literal = test::loadImm64(1, 0);
        Text.insert(Text.end(), Literal.begin(), Literal.end());
      }
      Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I + 1));
      Calls.push_back(Text.size());
      Text.push_back({});
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Leaf = Text.size();
    if (Fields)
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 2, 40));
    const size_t IdentityCall = Text.size();
    if (Identity)
      Text.push_back({});
    Text.push_back(encode(Opcode::JA));
    if (Fields)
      Text.push_back(encode(Opcode::LD_DW_REG, 4, 2, 24));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, Fields ? 4 : Identity ? 0 : 1));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Targets = Text.size();
    std::vector<uint8_t> Table(Count * 32);
    for (size_t I = 0; I < Count; ++I) {
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 41 + I));
      Text.push_back(encode(Opcode::EXIT));
      Text[Calls[I]] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Calls[I] - 1);
      const uint64_t Address = kBytecodeStart + (Targets + I * 2) * 8;
      llvm::support::endian::write64le(Table.data() + I * 32 + 24, Address);
      if (!Fields) {
        const auto Literal = test::loadImm64(1, Address);
        std::copy(Literal.begin(), Literal.end(), Text.begin() + Literals[I]);
      }
    }
    if (Identity) {
      const size_t Entry = Text.size();
      Text[IdentityCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Entry - IdentityCall - 1);
      Text.push_back(encode(Opcode::MOV64_REG, 0, 1));
      Text.push_back(encode(Opcode::EXIT));
    }
    auto Image = test::makeImage(Text, Table);
    Image.Sections[1].VA = TableAddress;
    Image.SBF->RodataVM = {TableAddress, Table.size()};
    auto Program = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    for (size_t I = 0; I < Count; ++I)
      EXPECT_TRUE(std::any_of(Report->Contexts.begin(), Report->Contexts.end(),
                              [&](const auto &Site) {
                                return !Site.EntryPrefixOnly &&
                                       Site.CallerSlot == Calls[I] &&
                                       Site.CallXSlot == CallX &&
                                       Site.TargetSlot == Targets + I * 2;
                              }))
          << I;
    EXPECT_TRUE(
        std::any_of(Report->SummaryEdges.begin(), Report->SummaryEdges.end(),
                    [&](const auto &Edge) {
                      return Edge.CallerSlot == Calls.back() &&
                             Edge.CalleeEntrySlot == Leaf &&
                             Edge.InputGeneralization ==
                                 CallXInputGeneralization::SpecializationLimit;
                    }));
    EXPECT_EQ(std::count_if(Report->Summaries.begin(), Report->Summaries.end(),
                            [&](const auto &Summary) {
                              return Summary.EntrySlot == Leaf &&
                                     !Summary.GeneralizedInputs;
                            }),
              kCallXContextSpecializationsPerEntry);
    size_t Instantiated = 0;
    for (const auto &Site : Report->Contexts)
      if (!Site.EntryPrefixOnly && Site.CallXSlot == CallX &&
          Site.InputGeneralization ==
              CallXInputGeneralization::SpecializationLimit) {
        ASSERT_TRUE(Site.InputExpression);
        ASSERT_TRUE(Site.TargetSlot);
        ASSERT_EQ(Site.InputExpressionValues.size(), Fields ? 3u : 1u);
        const auto &First = Site.InputExpressionValues.front();
        ASSERT_LT(First.Expression, Report->InputExpressions.size());
        EXPECT_EQ(Report->InputExpressions[First.Expression].Kind,
                  CallXInputExpressionKind::InputRegister);
        EXPECT_EQ(Report->InputExpressions[First.Expression].Register,
                  Fields ? 2u : 1u);
        EXPECT_EQ(Site.InputExpressionValues.back().Values.singleton().Value,
                  kBytecodeStart + *Site.TargetSlot * 8);
        ++Instantiated;
      }
    EXPECT_EQ(Instantiated, Count - kCallXContextSpecializationsPerEntry);
    checkSummaryReferences(*Report);
    auto Raw = executeRaw(*Program);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, 41u + Count - 1);
  }
}

TEST(SBFCallXContexts, GenericCallersRequireAProvenPathToTheFirstCall) {
  for (unsigned Selector : {0u, 1u, 2u})
    for (bool Conditional : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "selector=" << Selector
                                        << ", conditional=" << Conditional);
      std::vector<test::EncodedInstruction> Text{
          encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT)};
      const size_t Wrapper = Text.size();
      const size_t Literal = Text.size();
      const auto Placeholder = test::loadImm64(3, 0);
      Text.insert(Text.end(), Placeholder.begin(), Placeholder.end());
      if (Selector == 2)
        Text.push_back(encode(Opcode::LD_DW_REG, 4, 1));
      else
        Text.push_back(encode(Opcode::MOV64_IMM, 4, 0, 0, Selector));
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -8));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
      const size_t Call = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
      const size_t Leaf = Text.size();
      if (Conditional)
        Text.push_back(encode(Opcode::JEQ64_IMM, 4, 0, 2));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 2));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Target = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Call - 1);
      const auto Address = test::loadImm64(3, kBytecodeStart + Target * 8);
      std::copy(Address.begin(), Address.end(), Text.begin() + Literal);
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      bool GenericProof = false;
      for (const auto &Site : Report->Contexts) {
        if (Site.EntryPrefixOnly || Site.CallerSlot != Call ||
            Site.CallXSlot != CallX || !Site.TargetSlot ||
            !Site.CallerSummaryID)
          continue;
        const auto Caller =
            std::find_if(Report->Summaries.begin(), Report->Summaries.end(),
                         [&](const auto &Summary) {
                           return Summary.ID == *Site.CallerSummaryID;
                         });
        ASSERT_NE(Caller, Report->Summaries.end());
        if (Caller->GeneralizedInputs && Caller->EntrySlot == Wrapper) {
          EXPECT_EQ(Site.TargetSlot, Target);
          EXPECT_FALSE(Site.InputExpressionValues.empty());
          GenericProof = true;
        }
      }
      EXPECT_EQ(GenericProof, !Conditional || Selector == 1);
      checkSummaryReferences(*Report);
      auto Raw = executeSelector(*Program, 0);
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      const bool Executed = !Conditional || Selector == 1;
      EXPECT_EQ(Raw->ReturnValue, Executed ? 42u : 0u);
      EXPECT_EQ(
          std::any_of(Raw->Trace.begin(), Raw->Trace.end(),
                      [&](const auto &Step) { return Step.Slot == CallX; }),
          Executed);
    }
}

TEST(SBFCallXContexts, GenericCallerPathStopsBeforeAnEarlierCallee) {
  for (unsigned Selector : {0u, 1u}) {
    SCOPED_TRACE(Selector);
    std::vector<test::EncodedInstruction> Text{
        encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT)};
    const size_t Wrapper = Text.size();
    const size_t Literal = Text.size();
    const auto Placeholder = test::loadImm64(3, 0);
    Text.insert(Text.end(), Placeholder.begin(), Placeholder.end());
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -8));
    Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
    Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
    Text.push_back(encode(Opcode::MOV64_IMM, 4, 0, 0, Selector));
    const size_t Call = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::EXIT));
    const size_t Leaf = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    const size_t Earlier = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::LD_DW_REG, 2, 6));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 2));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Helper = Text.size();
    Text.push_back(encode(Opcode::JNE64_IMM, 4, 0, 1));
    Text.push_back(encode(Opcode::JA, 0, 0, -1));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Target = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Call - 1);
    Text[Earlier] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Earlier - 1);
    const auto Address = test::loadImm64(3, kBytecodeStart + Target * 8);
    std::copy(Address.begin(), Address.end(), Text.begin() + Literal);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    for (const auto &Site : Report->Contexts) {
      if (Site.EntryPrefixOnly || Site.CallerSlot != Call ||
          Site.CallXSlot != CallX || !Site.CallerSummaryID)
        continue;
      const auto Caller =
          std::find_if(Report->Summaries.begin(), Report->Summaries.end(),
                       [&](const auto &Summary) {
                         return Summary.ID == *Site.CallerSummaryID;
                       });
      ASSERT_NE(Caller, Report->Summaries.end());
      if (Caller->GeneralizedInputs && Caller->EntrySlot == Wrapper)
        EXPECT_FALSE(Site.TargetSlot);
    }
    checkSummaryReferences(*Report);
    InterpreterOptions Options;
    Options.MaxSteps = 128;
    auto Raw = executeRaw(*Program, {}, Options);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, Selector ? ExecutionStatus::Returned
                                    : ExecutionStatus::StepLimit);
    EXPECT_EQ(std::any_of(Raw->Trace.begin(), Raw->Trace.end(),
                          [&](const auto &Step) { return Step.Slot == CallX; }),
              Selector != 0);
    if (Selector)
      EXPECT_EQ(Raw->ReturnValue, 42u);
  }
}

TEST(SBFCallXContexts, GenericTargetLoadsUseTheCorrectMemorySnapshot) {
  for (bool CaptureBeforeWrite : {false, true}) {
    SCOPED_TRACE(CaptureBeforeWrite);
    const size_t Count = kCallXContextSpecializationsPerEntry + 2;
    std::vector<test::EncodedInstruction> Text;
    std::vector<size_t> Literals, Calls;
    for (size_t I = 0; I < Count; ++I) {
      const auto Input = test::loadImm64(4, kInputStart);
      Text.insert(Text.end(), Input.begin(), Input.end());
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 4, 8));
      Text.push_back(encode(Opcode::LD_DW_REG, 4, 4));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
      Literals.push_back(Text.size());
      const auto Literal = test::loadImm64(3, 0);
      Text.insert(Text.end(), Literal.begin(), Literal.end());
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 3));
      Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
      Calls.push_back(Text.size());
      Text.push_back({});
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Leaf = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 8, 9));
    Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    if (CaptureBeforeWrite)
      Text.push_back(encode(Opcode::LD_DW_REG, 7, 6));
    const size_t OtherLiteral = Text.size();
    const auto Literal = test::loadImm64(3, 0);
    Text.insert(Text.end(), Literal.begin(), Literal.end());
    Text.push_back(encode(Opcode::JEQ64_IMM, 5, 0, 1));
    Text.push_back(encode(Opcode::ST_DW_REG, 4, 3));
    // One predecessor changes only the entry-memory preservation guarantee:
    // generic values, expressions and joined postconditions stay identical.
    if (!CaptureBeforeWrite)
      Text.push_back(encode(Opcode::LD_DW_REG, 7, 6));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 7));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Targets = Text.size();
    for (unsigned I = 0; I < 2; ++I) {
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 41 + I));
      Text.push_back(encode(Opcode::EXIT));
    }
    for (size_t I = 0; I < Count; ++I) {
      const auto Target = test::loadImm64(3, kBytecodeStart + Targets * 8);
      std::copy(Target.begin(), Target.end(), Text.begin() + Literals[I]);
      Text[Calls[I]] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Calls[I] - 1);
    }
    const auto Other = test::loadImm64(3, kBytecodeStart + (Targets + 2) * 8);
    std::copy(Other.begin(), Other.end(), Text.begin() + OtherLiteral);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    bool SawGeneric = false;
    for (const auto &Site : Report->Contexts)
      if (!Site.EntryPrefixOnly && Site.GeneralizedInputs &&
          Site.InputGeneralization ==
              CallXInputGeneralization::SpecializationLimit &&
          Site.CallerSlot == Calls.back() && Site.CallXSlot == CallX) {
        EXPECT_EQ(Site.TargetSlot.has_value(), CaptureBeforeWrite);
        if (CaptureBeforeWrite) {
          EXPECT_EQ(Site.TargetSlot, Targets);
          EXPECT_FALSE(Site.InputExpressionValues.empty());
        }
        SawGeneric = true;
      }
    EXPECT_TRUE(SawGeneric);
    checkSummaryReferences(*Report);
    for (unsigned Selector : {0u, 1u}) {
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input(16);
      llvm::support::endian::write64le(
          Input.data(),
          initialFramePointer(Program->Low.TheVersion, Program->Config) - 8);
      llvm::support::endian::write64le(Input.data() + 8, Selector);
      Environment.Memory.push_back({kInputStart, Input, false, "input"});
      auto Raw = executeRaw(*Program, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue,
                CaptureBeforeWrite || Selector == 0 ? 41u : 42u);
    }
  }
}

TEST(SBFCallXContexts, GenericTargetInputsJoinEquivalentTransfers) {
  enum class Difference { None, Operand, Operation, EntryMemory };
  for (const auto Transfer :
       {Opcode::LD_DW_REG, Opcode::ADD64_IMM, Opcode::SUB64_IMM})
    for (const auto Different :
         {Difference::None, Difference::Operand, Difference::Operation,
          Difference::EntryMemory}) {
      SCOPED_TRACE(::testing::Message()
                   << "transfer=" << static_cast<unsigned>(Transfer)
                   << ", difference=" << static_cast<unsigned>(Different));
      const bool Load = Transfer == Opcode::LD_DW_REG;
      const size_t Count = kCallXContextSpecializationsPerEntry + 2;
      std::vector<test::EncodedInstruction> Text;
      std::vector<size_t> Literals, Calls;
      const auto Input = test::loadImm64(8, kInputStart);
      Text.insert(Text.end(), Input.begin(), Input.end());
      for (size_t I = 0; I < Count; ++I) {
        Text.push_back(encode(Opcode::LD_DW_REG, 5, 8));
        Text.push_back(encode(Opcode::LD_DW_REG, 4, 8, 8));
        Literals.push_back(Text.size());
        const auto Target = test::loadImm64(3, 0);
        Text.insert(Text.end(), Target.begin(), Target.end());
        if (Load) {
          Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -32));
          Text.push_back(encode(Opcode::ADD64_IMM, 3, 0, 0, 16));
          Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -24));
          Text.push_back(encode(Opcode::MOV64_REG, 3, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 3, 0, 0, -40));
        }
        Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -8));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
        Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
        Calls.push_back(Text.size());
        Text.push_back({});
      }
      Text.push_back(encode(Opcode::EXIT));
      const size_t Leaf = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 8, 9));
      const size_t Branch = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 6));
      if (Load) {
        Text.push_back(encode(Transfer, 7, 2, 8));
      } else {
        Text.push_back(encode(Opcode::MOV64_REG, 7, 2));
        Text.push_back(encode(Transfer, 7, 0, 0, 16));
      }
      const size_t Jump = Text.size();
      Text.push_back({});
      const size_t Other = Text.size();
      if (Different == Difference::EntryMemory)
        Text.push_back(encode(Opcode::ST_DW_IMM, 4));
      Text.push_back(encode(Opcode::MOV64_REG, 3, 1));
      Text.push_back(encode(Opcode::LD_DW_REG, 4, 3));
      if (Load) {
        const auto OtherTransfer =
            Different == Difference::Operation ? Opcode::LD_W_REG : Transfer;
        Text.push_back(encode(OtherTransfer, 7, 4,
                              Different == Difference::Operand ? 16 : 8));
      } else {
        const auto OtherTransfer = Different != Difference::Operation ? Transfer
                                   : Transfer == Opcode::ADD64_IMM
                                       ? Opcode::SUB64_IMM
                                       : Opcode::ADD64_IMM;
        Text.push_back(encode(Opcode::MOV64_REG, 7, 4));
        Text.push_back(encode(OtherTransfer, 7, 0, 0,
                              Different == Difference::Operand ? 32 : 16));
      }
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 7));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Targets = Text.size();
      for (size_t I = 0; I < Count + 4; ++I) {
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 200 + I));
        Text.push_back(encode(Opcode::EXIT));
      }
      for (size_t I = 0; I < Count; ++I) {
        const uint64_t Address = kBytecodeStart + (Targets + (I + 2) * 2) * 8;
        const auto Target =
            test::loadImm64(3, Load                            ? Address
                               : Transfer == Opcode::ADD64_IMM ? Address - 16
                                                               : Address + 16);
        std::copy(Target.begin(), Target.end(), Text.begin() + Literals[I]);
        Text[Calls[I]] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Calls[I] - 1);
      }
      Text[Branch] = encode(Opcode::JEQ64_IMM, 5, 0, Other - Branch - 1, 1);
      Text[Jump] = encode(Opcode::JA, 0, 0, CallX - Jump - 1);
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      for (size_t I = kCallXContextSpecializationsPerEntry; I < Count; ++I) {
        const auto Site = std::find_if(
            Report->Contexts.begin(), Report->Contexts.end(),
            [&](const auto &Site) {
              return !Site.EntryPrefixOnly && Site.CallerSlot == Calls[I] &&
                     Site.CallXSlot == CallX &&
                     Site.InputGeneralization ==
                         CallXInputGeneralization::SpecializationLimit;
            });
        ASSERT_NE(Site, Report->Contexts.end());
        EXPECT_EQ(Site->TargetSlot.has_value(), Different == Difference::None);
        if (Different == Difference::None) {
          EXPECT_EQ(Site->TargetSlot, Targets + (I + 2) * 2);
          EXPECT_EQ(Site->InputExpressionValues.size(), 3u);
        }
      }
      checkSummaryReferences(*Report);
      for (uint64_t Selector : {0u, 1u}) {
        ExecutionEnvironment Environment;
        std::vector<uint8_t> Bytes(24);
        llvm::support::endian::write64le(Bytes.data(), Selector);
        llvm::support::endian::write64le(Bytes.data() + 8, kInputStart + 16);
        Environment.Memory.push_back({kInputStart, Bytes, true, "selector"});
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        if (Load && Different == Difference::Operation && Selector) {
          EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted);
          continue;
        }
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        int64_t Selected = Count + 1;
        if (Selector && Different == Difference::Operand)
          Selected += Transfer == Opcode::SUB64_IMM ? -1 : 1;
        if (Selector && Different == Difference::Operation)
          Selected += Transfer == Opcode::ADD64_IMM ? -2 : 2;
        EXPECT_EQ(Raw->ReturnValue, 200u + Selected);
      }
    }
}

TEST(SBFCallXContexts, GenericTargetInputsComposeLoadedReturns) {
  enum class Write { None, BeforeCall, BeforeLoad, AfterLoad };
  for (size_t Depth : {1u, 2u, 4u})
    for (const auto Writes : {Write::None, Write::BeforeCall, Write::BeforeLoad,
                              Write::AfterLoad}) {
      SCOPED_TRACE(::testing::Message() << "depth=" << Depth << ", write="
                                        << static_cast<unsigned>(Writes));
      const size_t Count = kCallXContextSpecializationsPerEntry + 2;
      std::vector<test::EncodedInstruction> Text;
      std::vector<size_t> Literals, Calls, HelperCalls;
      const auto Input = test::loadImm64(8, kInputStart);
      Text.insert(Text.end(), Input.begin(), Input.end());
      for (size_t I = 0; I < Count; ++I) {
        Text.push_back(encode(Opcode::LD_DW_REG, 4, 8));
        for (size_t J = 0; J < Depth; ++J) {
          Text.push_back(encode(Opcode::MOV64_REG, 2, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, -64 - J * 16));
          Text.push_back(
              encode(Opcode::ST_DW_REG, 10, 2, J ? -56 - (J - 1) * 16 : -8));
        }
        Literals.push_back(Text.size());
        const auto Target = test::loadImm64(3, 0);
        Text.insert(Text.end(), Target.begin(), Target.end());
        Text.push_back(encode(Opcode::ST_DW_REG, 2, 3, 8));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
        Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
        Calls.push_back(Text.size());
        Text.push_back({});
      }
      Text.push_back(encode(Opcode::EXIT));
      const size_t Wrapper = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 8, 9));
      if (Writes == Write::BeforeCall)
        Text.push_back(encode(Opcode::ST_DW_IMM, 4));
      const size_t WrapperCall = Text.size();
      Text.push_back({});
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 0));
      Text.push_back(encode(Opcode::EXIT));
      std::vector<size_t> Helpers;
      for (size_t I = 0; I < Depth; ++I) {
        Helpers.push_back(Text.size());
        HelperCalls.push_back(Text.size());
        Text.push_back({});
        if (I == 0 && Writes == Write::BeforeLoad)
          Text.push_back(encode(Opcode::ST_DW_IMM, 4));
        Text.push_back(encode(Opcode::LD_DW_REG, 0, 0, 8));
        if (I == 0 && Writes == Write::AfterLoad)
          Text.push_back(encode(Opcode::ST_DW_IMM, 4));
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Leaf = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Targets = Text.size();
      for (size_t I = 0; I < Count; ++I) {
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 200 + I));
        Text.push_back(encode(Opcode::EXIT));
        const auto Target =
            test::loadImm64(3, kBytecodeStart + (Targets + I * 2) * 8);
        std::copy(Target.begin(), Target.end(), Text.begin() + Literals[I]);
        Text[Calls[I]] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
      }
      Text[WrapperCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Helpers.front() - WrapperCall - 1);
      for (size_t I = 0; I < Depth; ++I) {
        const size_t Callee = I + 1 < Depth ? Helpers[I + 1] : Leaf;
        Text[HelperCalls[I]] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Callee - HelperCalls[I] - 1);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      size_t GenericLeafReturns = 0, GenericHelperReturns = 0;
      for (const auto &Summary : Report->Summaries) {
        if (!Summary.GeneralizedInputs)
          continue;
        if (Summary.EntrySlot == Leaf) {
          ASSERT_TRUE(Summary.ReturnValueExpression);
          const auto &Root =
              Report->InputExpressions[*Summary.ReturnValueExpression];
          EXPECT_EQ(Root.Kind, CallXInputExpressionKind::Instruction);
          ++GenericLeafReturns;
        }
        if (Summary.EntrySlot == Helpers.front()) {
          EXPECT_EQ(Summary.ReturnValueExpression.has_value(),
                    Writes != Write::BeforeLoad);
          ++GenericHelperReturns;
        }
      }
      EXPECT_EQ(GenericLeafReturns, 1u);
      EXPECT_EQ(GenericHelperReturns, 1u);
      for (size_t I = kCallXContextSpecializationsPerEntry; I < Count; ++I) {
        const auto Site = std::find_if(
            Report->Contexts.begin(), Report->Contexts.end(),
            [&](const auto &Site) {
              return !Site.EntryPrefixOnly && Site.CallerSlot == Calls[I] &&
                     Site.CallXSlot == CallX &&
                     Site.InputGeneralization ==
                         CallXInputGeneralization::SpecializationLimit;
            });
        ASSERT_NE(Site, Report->Contexts.end());
        const bool Available =
            Writes == Write::None || Writes == Write::AfterLoad;
        EXPECT_EQ(Site->TargetSlot.has_value(), Available);
        if (Available) {
          EXPECT_EQ(Site->TargetSlot, Targets + I * 2);
          EXPECT_EQ(Site->InputExpressionValues.size(), Depth + 2);
        }
      }
      checkSummaryReferences(*Report);
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Bytes(16);
      llvm::support::endian::write64le(Bytes.data(), kInputStart + 8);
      Environment.Memory.push_back({kInputStart, Bytes, true, "write address"});
      auto Raw = executeRaw(*Program, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 200u + Count - 1);
    }
}

TEST(SBFCallXContexts, LoadedExpressionsSurviveCompleteSpillsOnly) {
  enum class Write {
    None,
    Disjoint,
    Partial,
    Unknown,
    BeforeSpill,
    ReadOnlyCall
  };
  for (Write Writes :
       {Write::None, Write::Disjoint, Write::Partial, Write::Unknown,
        Write::BeforeSpill, Write::ReadOnlyCall}) {
    SCOPED_TRACE(static_cast<unsigned>(Writes));
    const size_t Count = kCallXContextSpecializationsPerEntry + 2;
    std::vector<test::EncodedInstruction> Text;
    std::vector<size_t> Literals, Calls;
    const auto Input = test::loadImm64(8, kInputStart);
    Text.insert(Text.end(), Input.begin(), Input.end());
    Text.push_back(encode(Opcode::LD_DW_REG, 7, 8));
    for (size_t I = 0; I < Count; ++I) {
      Literals.push_back(Text.size());
      const auto Target = test::loadImm64(3, 0);
      Text.insert(Text.end(), Target.begin(), Target.end());
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -8));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
      Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
      Calls.push_back(Text.size());
      Text.push_back({});
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Wrapper = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 8, 9));
    const size_t HelperCall = Text.size();
    Text.push_back({});
    if (Writes == Write::BeforeSpill)
      Text.push_back(encode(Opcode::ST_DW_IMM, 7));
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 0, -8));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    if (Writes == Write::ReadOnlyCall)
      Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                            getSyscallInfo(Syscall::GetStackHeight)->Hash));
    if (Writes == Write::Disjoint)
      Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -16));
    if (Writes == Write::Partial)
      Text.push_back(encode(Opcode::ST_B_IMM, 10, 0, -1));
    if (Writes == Write::Unknown)
      Text.push_back(encode(Opcode::ST_DW_IMM, 7));
    Text.push_back(encode(Opcode::JA, 0, 0, 0));
    Text.push_back(encode(Opcode::LD_DW_REG, 0, 10, -8));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Helper = Text.size();
    Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Targets = Text.size();
    for (size_t I = 0; I < Count; ++I) {
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 400 + I));
      Text.push_back(encode(Opcode::EXIT));
      const auto Target =
          test::loadImm64(3, kBytecodeStart + (Targets + I * 2) * 8);
      std::copy(Target.begin(), Target.end(), Text.begin() + Literals[I]);
      Text[Calls[I]] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
    }
    Text[HelperCall] =
        encode(Opcode::CALL_IMM, 0, 1, 0, Helper - HelperCall - 1);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    for (size_t I = kCallXContextSpecializationsPerEntry; I < Count; ++I) {
      const auto Site = std::find_if(
          Report->Contexts.begin(), Report->Contexts.end(),
          [&](const auto &Site) {
            return !Site.EntryPrefixOnly && Site.CallerSlot == Calls[I] &&
                   Site.CallXSlot == CallX &&
                   Site.InputGeneralization ==
                       CallXInputGeneralization::SpecializationLimit;
          });
      ASSERT_NE(Site, Report->Contexts.end());
      const bool Available =
          Writes != Write::Partial && Writes != Write::Unknown;
      EXPECT_EQ(Site->TargetSlot.has_value(), Available);
      if (Available)
        EXPECT_EQ(Site->TargetSlot, Targets + I * 2);
    }
    checkSummaryReferences(*Report);
    ExecutionEnvironment Environment;
    std::vector<uint8_t> Bytes(8);
    llvm::support::endian::write64le(Bytes.data(), kInputStart + 8);
    Bytes.resize(16);
    Environment.Memory.push_back({kInputStart, Bytes, true, "opaque pointer"});
    Environment.HostSyscall = [](uint32_t, const SyscallArguments &) {
      return SyscallOutcome::returned(0);
    };
    auto Raw = executeRaw(*Program, std::move(Environment));
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, 400u + Count - 1);
  }
}

TEST(SBFCallXContexts, BoundedFramesPreserveOnlyDisjointAbsoluteWrites) {
  for (Version V : {Version::V0, Version::V3})
    for (bool Heap : {false, true}) {
      SCOPED_TRACE(::testing::Message() << static_cast<unsigned>(V) << Heap);
      const size_t Count = kCallXContextSpecializationsPerEntry + 2;
      std::vector<test::EncodedInstruction> Text;
      std::vector<size_t> Literals, Calls;
      for (size_t I = 0; I < Count; ++I) {
        Literals.push_back(Text.size());
        const auto Address = test::loadImm64(3, 0);
        Text.insert(Text.end(), Address.begin(), Address.end());
        Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -8));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
        Calls.push_back(Text.size());
        Text.push_back({});
      }
      Text.push_back(encode(Opcode::EXIT));
      const size_t Helper = Text.size();
      const auto Destination = test::loadImm64(
          2, Heap ? kHeapStart : initialFramePointer(V, SBFVMConfig{}) - 8);
      Text.insert(Text.end(), Destination.begin(), Destination.end());
      Text.push_back(encode(Opcode::ST_DW_IMM, 2));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
      const size_t CallX = Text.size();
      Text.push_back(V == Version::V0 ? encode(Opcode::CALL_REG, 0, 0, 0, 2)
                                      : encode(Opcode::CALL_REG, 2));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Targets = Text.size();
      for (size_t I = 0; I < Count; ++I) {
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 500 + I));
        Text.push_back(encode(Opcode::EXIT));
        const auto Target =
            test::loadImm64(3, kBytecodeStart + (Targets + I * 2) * 8);
        std::copy(Target.begin(), Target.end(), Text.begin() + Literals[I]);
        Text[Calls[I]] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Calls[I] - 1);
      }
      auto Image = test::makeImage(Text, {});
      Image.SBF->Version = V;
      Image.SBF->ELFFlags = static_cast<uint32_t>(V);
      Image.SBF->StrictLayout = V == Version::V3;
      auto Program = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      for (size_t I = kCallXContextSpecializationsPerEntry; I < Count; ++I) {
        const auto Site =
            std::find_if(Report->Contexts.begin(), Report->Contexts.end(),
                         [&](const auto &Site) {
                           return !Site.EntryPrefixOnly &&
                                  Site.CallerSlot == Calls[I] &&
                                  Site.CallXSlot == CallX;
                         });
        ASSERT_NE(Site, Report->Contexts.end());
        EXPECT_EQ(Site->TargetSlot.has_value(), Heap);
        if (Heap)
          EXPECT_EQ(Site->TargetSlot, Targets + I * 2);
      }
      ExecutionEnvironment Environment;
      Environment.Memory.push_back(
          {kHeapStart, std::vector<uint8_t>(8), true, "explicit heap cell"});
      auto Raw = executeRaw(*Program, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status,
                Heap ? ExecutionStatus::Returned : ExecutionStatus::Faulted)
          << Raw->Error;
      if (Heap)
        EXPECT_EQ(Raw->ReturnValue, 500u + Count - 1);
      checkSummaryReferences(*Report);
    }
}

void addLegacySyscallRelocation(BinaryImage &Image, size_t Slot,
                                const SyscallInfo &Syscall) {
  RelocationEntry Reloc;
  Reloc.Address = kBytecodeStart + Slot * kInstructionSize;
  Reloc.Type = static_cast<uint32_t>(Relocation::Call32);
  Reloc.SymbolName = Syscall.Name.str();
  Reloc.ELF.emplace();
  Reloc.ELF->Source = ELFRelocationSource::ProgramDynamicTable;
  Reloc.ELF->RawOffset = Slot * kInstructionSize;
  Reloc.ELF->Symbol.emplace();
  Reloc.ELF->Symbol->Name = Syscall.Name.str();
  Reloc.ELF->Symbol->SectionIndex = llvm::ELF::SHN_UNDEF;
  Reloc.ELF->Symbol->Type = llvm::ELF::STT_NOTYPE;
  Reloc.ELF->Symbol->Binding = llvm::ELF::STB_GLOBAL;
  Image.Relocations.push_back(std::move(Reloc));
}

TEST(SBFCallXContexts, SyscallsPreserveArgumentValuesLoadedBeforeTheCall) {
  const size_t Count = kCallXContextSpecializationsPerEntry + 2;
  for (Version V : {Version::V0, Version::V3})
    for (unsigned Register = kFirstArgumentRegister;
         Register < kFirstCalleeSavedRegister; ++Register)
      for (bool WritesMemory : {false, true}) {
        if (WritesMemory &&
            Register < kFirstArgumentRegister +
                           getSyscallInfo(Syscall::Memset)->ArgumentCount)
          continue;
        SCOPED_TRACE(::testing::Message() << unsigned(V) << ":r" << Register
                                          << ":writes=" << WritesMemory);
        std::vector<test::EncodedInstruction> Text;
        std::vector<size_t> Literals, Calls;
        for (size_t I = 0; I < Count; ++I) {
          Literals.push_back(Text.size());
          const auto Literal = test::loadImm64(3, 0);
          Text.insert(Text.end(), Literal.begin(), Literal.end());
          Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -64));
          Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -64));
          Calls.push_back(Text.size());
          Text.push_back({});
        }
        Text.push_back(encode(Opcode::EXIT));
        const size_t Wrapper = Text.size();
        Text.push_back(encode(Opcode::LD_DW_REG, Register, 1));
        if (WritesMemory) {
          // The guest performs this same zero fill before the host call, so
          // the callback's no-op is a faithful memset for these actual inputs.
          // The loaded register must retain its earlier value nonetheless.
          Text.push_back(encode(Opcode::ST_DW_IMM, 1));
          Text.push_back(encode(Opcode::MOV64_IMM, 2));
          Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0, 8));
        }
        const auto *Syscall = getSyscallInfo(
            WritesMemory ? Syscall::Memset : Syscall::GetStackHeight);
        const size_t SyscallSlot = Text.size();
        Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                              V == Version::V0 ? kLegacyUnresolvedCallImmediate
                                               : Syscall->Hash));
        const size_t CallX = Text.size();
        Text.push_back(V == Version::V0
                           ? encode(Opcode::CALL_REG, 0, 0, 0, Register)
                           : encode(Opcode::CALL_REG, Register));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Targets = Text.size();
        for (size_t I = 0; I < Count; ++I) {
          Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42 + I));
          Text.push_back(encode(Opcode::EXIT));
          const auto Target =
              test::loadImm64(3, kBytecodeStart + (Targets + I * 2) * 8);
          std::copy(Target.begin(), Target.end(), Text.begin() + Literals[I]);
          Text[Calls[I]] =
              encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
        }
        auto Image = test::makeImage(Text, {});
        Image.SBF->Version = V;
        Image.SBF->ELFFlags = static_cast<uint32_t>(V);
        Image.SBF->StrictLayout = V == Version::V3;
        if (V == Version::V0)
          addLegacySyscallRelocation(Image, SyscallSlot, *Syscall);
        auto Program = analyze(Image);
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        for (size_t I = 0; I < Count; ++I)
          EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
            return !Site.EntryPrefixOnly && Site.CallerSlot == Calls[I] &&
                   Site.CallXSlot == CallX &&
                   Site.TargetSlot == Targets + I * 2;
          })) << I;
        checkSummaryReferences(*Report);
        ExecutionEnvironment Environment;
        Environment.HostSyscall = [&](uint32_t Hash,
                                      const SyscallArguments &Args) {
          if (Hash != Syscall->Hash)
            return SyscallOutcome::unregistered();
          if (WritesMemory) {
            EXPECT_EQ(Args[1], 0u);
            EXPECT_EQ(Args[2], 8u);
          }
          return SyscallOutcome::returned(0);
        };
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, 42u + Count - 1);
        EXPECT_EQ(Raw->Syscalls.size(), Count);
      }
}

TEST(SBFCallXContexts, SyscallConstantsReachTheGlobalCallResolver) {
  for (Version V : {Version::V0, Version::V3})
    for (unsigned Register = kFirstArgumentRegister;
         Register < kFirstCalleeSavedRegister; ++Register) {
      SCOPED_TRACE(::testing::Message() << unsigned(V) << ":r" << Register);
      auto Text = test::loadImm64(Register, kBytecodeStart + 5 * 8);
      const auto *Syscall = getSyscallInfo(Syscall::GetStackHeight);
      Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                            V == Version::V0 ? kLegacyUnresolvedCallImmediate
                                             : Syscall->Hash));
      Text.push_back(V == Version::V0
                         ? encode(Opcode::CALL_REG, 0, 0, 0, Register)
                         : encode(Opcode::CALL_REG, Register));
      Text.push_back(encode(Opcode::EXIT));
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      auto Image = test::makeImage(Text, {});
      Image.SBF->Version = V;
      Image.SBF->ELFFlags = static_cast<uint32_t>(V);
      Image.SBF->StrictLayout = V == Version::V3;
      if (V == Version::V0)
        addLegacySyscallRelocation(Image, 2, *Syscall);
      auto Program = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      const auto *Call = MedInstructionIndex(Program->Med).find(3);
      ASSERT_NE(Call, nullptr);
      EXPECT_EQ(Call->Call, CallKind::Internal);
      EXPECT_EQ(Call->CallTarget, 5u);
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      EXPECT_TRUE(Report->UnresolvedCallXSlots.empty());
      ExecutionEnvironment Environment;
      Environment.HostSyscall = [](uint32_t, const SyscallArguments &) {
        return SyscallOutcome::returned(0);
      };
      auto Raw = executeRaw(*Program, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 42u);
    }
}

TEST(SBFCallXContexts, SyscallsClobberLoadedInputExpressions) {
  const size_t Count = kCallXContextSpecializationsPerEntry + 2;
  std::vector<test::EncodedInstruction> Text;
  std::vector<size_t> Literals, Calls;
  for (size_t I = 0; I < Count; ++I) {
    Literals.push_back(Text.size());
    const auto Target = test::loadImm64(3, 0);
    Text.insert(Text.end(), Target.begin(), Target.end());
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -8));
    Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
    Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
    Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
    Calls.push_back(Text.size());
    Text.push_back({});
  }
  Text.push_back(encode(Opcode::EXIT));
  const size_t Wrapper = Text.size();
  Text.push_back(encode(Opcode::MOV64_REG, 8, 9));
  Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
  Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                        getSyscallInfo(Syscall::GetStackHeight)->Hash));
  const size_t CallX = Text.size();
  Text.push_back(encode(Opcode::CALL_REG, 0));
  Text.push_back(encode(Opcode::EXIT));
  const size_t Target = Text.size();
  Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
  Text.push_back(encode(Opcode::EXIT));
  for (size_t I = 0; I < Count; ++I) {
    const auto Address = test::loadImm64(3, kBytecodeStart + Target * 8);
    std::copy(Address.begin(), Address.end(), Text.begin() + Literals[I]);
    Text[Calls[I]] = encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
  }
  auto Program = analyze(test::makeImage(Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  size_t Checked = 0;
  for (const auto &Site : Report->Contexts)
    if (!Site.EntryPrefixOnly && Site.CallXSlot == CallX) {
      EXPECT_FALSE(Site.TargetSlot);
      EXPECT_FALSE(Site.InputExpression);
      EXPECT_TRUE(Site.TargetValueAlternatives.unknown());
      ++Checked;
    }
  EXPECT_GE(Checked, Count);
  checkSummaryReferences(*Report);
  ExecutionEnvironment Environment;
  Environment.HostSyscall = [](uint32_t, const SyscallArguments &) {
    return SyscallOutcome::returned(0);
  };
  auto Raw = executeRaw(*Program, std::move(Environment));
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted);
}

TEST(SBFCallXContexts, LoadedReturnLivenessIncludesTheNextCalleesInput) {
  for (bool Forward : {false, true}) {
    SCOPED_TRACE(Forward);
    const size_t Count = kCallXContextSpecializationsPerEntry + 2;
    std::vector<test::EncodedInstruction> Text;
    std::vector<size_t> Literals, Calls;
    for (size_t I = 0; I < Count; ++I) {
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16 * (I + 1)));
      Literals.push_back(Text.size());
      const auto Value = test::loadImm64(3, 0);
      Text.insert(Text.end(), Value.begin(), Value.end());
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 3));
      Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
      Calls.push_back(Text.size());
      Text.push_back({});
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Load = Text.size();
    Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Relay = Text.size();
    const size_t RelayLiteral = Text.size();
    if (!Forward) {
      const auto Value = test::loadImm64(0, 0);
      Text.insert(Text.end(), Value.begin(), Value.end());
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Wrapper = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 8, 9));
    Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, Load - Text.size() - 1));
    Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, Relay - Text.size() - 1));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Targets = Text.size();
    for (size_t I = 0; I < Count; ++I) {
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 300 + I));
      Text.push_back(encode(Opcode::EXIT));
      const auto Value =
          test::loadImm64(3, kBytecodeStart + (Targets + 2 * I) * 8);
      std::copy(Value.begin(), Value.end(), Text.begin() + Literals[I]);
      Text[Calls[I]] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
    }
    if (!Forward) {
      const auto Value = test::loadImm64(0, kBytecodeStart + Targets * 8);
      std::copy(Value.begin(), Value.end(), Text.begin() + RelayLiteral);
    }
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    for (size_t I = kCallXContextSpecializationsPerEntry; I < Count; ++I)
      EXPECT_TRUE(std::any_of(
          Report->Contexts.begin(), Report->Contexts.end(), [&](const auto &S) {
            return !S.EntryPrefixOnly && S.CallerSlot == Calls[I] &&
                   S.CallXSlot == CallX &&
                   S.TargetSlot == Targets + (Forward ? 2 * I : 0);
          }));
    checkSummaryReferences(*Report);
    auto Raw = executeRaw(*Program);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, 300u + (Forward ? Count - 1 : 0));
  }
}

TEST(SBFCallXContexts, UnknownWriteEffectsHaveOrderIndependentEquality) {
  using namespace callx_detail;
  auto Program = analyze(test::makeImage({encode(Opcode::EXIT)}, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const RegisterValue FrameField{RegisterValue::Kind::StackAddress,
                                 kRegisterCount, -8};
  MemoryEffects Before, After;
  Before.addWrite(FrameField, 8, *Program);
  Before.addWrite({}, 8, *Program);
  After.addWrite({}, 8, *Program);
  After.addWrite(FrameField, 8, *Program);
  EXPECT_EQ(Before, After);
  Before.include(After, symbolicInputs(), *Program);
  EXPECT_EQ(Before, After);
  for (const auto &Effects : {Before, After}) {
    ScratchState Scratch;
    Scratch.Values.write(kInputStart, {RegisterValue::Kind::Constant, 7, 0});
    EXPECT_FALSE(Effects.invalidate(Scratch, {}, *Program));
    EXPECT_TRUE(Scratch.empty());
  }
}

TEST(SBFCallXContexts, GenericLoadedReturnsCheckRebasedWriteFootprints) {
  enum class Write { LeafFrame, WrapperFrame, BoundPointer, UnknownPointer };
  for (unsigned Depth : {1u, 2u, 4u})
    for (Write Writes : {Write::LeafFrame, Write::WrapperFrame,
                         Write::BoundPointer, Write::UnknownPointer})
      for (bool Aliases : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "depth=" << Depth << ", write="
                                          << static_cast<unsigned>(Writes)
                                          << ", aliases=" << Aliases);
        const size_t Count = kCallXContextSpecializationsPerEntry + 2;
        const auto Stride = automaticFrameStride(Version::V3, SBFVMConfig{});
        const auto Frame = initialFramePointer(Version::V3, SBFVMConfig{});
        const int32_t Offset =
            Aliases && (Writes == Write::LeafFrame ||
                        Writes == Write::WrapperFrame)
                ? static_cast<int32_t>(
                      Stride * (Writes == Write::LeafFrame ? Depth + 1 : 1)) -
                      8
                : -8;
        std::vector<test::EncodedInstruction> Text;
        std::vector<size_t> Calls, Literals;
        const auto Input = test::loadImm64(8, kInputStart);
        Text.insert(Text.end(), Input.begin(), Input.end());
        for (size_t I = 0; I < Count; ++I) {
          Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, Offset));
          Literals.push_back(Text.size());
          const auto Target = test::loadImm64(3, 0);
          Text.insert(Text.end(), Target.begin(), Target.end());
          Text.push_back(encode(Opcode::ST_DW_REG, 1, 3));
          if (Writes == Write::BoundPointer) {
            Text.push_back(encode(Opcode::MOV64_REG, 4, 1));
            if (!Aliases)
              Text.push_back(encode(Opcode::ADD64_IMM, 4, 0, 0, -8));
          } else {
            Text.push_back(encode(Opcode::LD_DW_REG, 4, 8));
          }
          Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
          Calls.push_back(Text.size());
          Text.push_back({});
        }
        Text.push_back(encode(Opcode::EXIT));
        const size_t Leaf = Text.size();
        if (Writes == Write::LeafFrame)
          Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -8));
        if (Writes == Write::UnknownPointer || Writes == Write::BoundPointer)
          Text.push_back(encode(Opcode::ST_DW_IMM, 4));
        Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
        Text.push_back(encode(Opcode::EXIT));
        size_t Callee = Leaf;
        for (unsigned I = 1; I < Depth; ++I) {
          const size_t Entry = Text.size();
          Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0,
                                static_cast<int32_t>(Callee) - Entry - 1));
          Text.push_back(encode(Opcode::EXIT));
          Callee = Entry;
        }
        const size_t Wrapper = Text.size();
        Text.push_back(encode(Opcode::MOV64_REG, 8, 9));
        if (Writes == Write::WrapperFrame)
          Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -8));
        const size_t HelperCall = Text.size();
        Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0,
                              static_cast<int32_t>(Callee) - HelperCall - 1));
        const size_t CallX = Text.size();
        Text.push_back(encode(Opcode::CALL_REG, 0));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Targets = Text.size();
        for (size_t I = 0; I < Count; ++I) {
          Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 300 + I));
          Text.push_back(encode(Opcode::EXIT));
          const auto Target =
              test::loadImm64(3, kBytecodeStart + (Targets + I * 2) * 8);
          std::copy(Target.begin(), Target.end(), Text.begin() + Literals[I]);
          Text[Calls[I]] =
              encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
        }
        auto Program = analyze(test::makeImage(Text, {}));
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        for (size_t I = kCallXContextSpecializationsPerEntry; I < Count; ++I) {
          const auto Site = std::find_if(
              Report->Contexts.begin(), Report->Contexts.end(),
              [&](const auto &Site) {
                return !Site.EntryPrefixOnly && Site.CallerSlot == Calls[I] &&
                       Site.CallXSlot == CallX &&
                       Site.InputGeneralization ==
                           CallXInputGeneralization::SpecializationLimit;
              });
          ASSERT_NE(Site, Report->Contexts.end());
          const bool Available = !Aliases && Writes != Write::UnknownPointer;
          EXPECT_EQ(Site->TargetSlot.has_value(), Available);
          if (Available)
            EXPECT_EQ(Site->TargetSlot, Targets + I * 2);
        }
        checkSummaryReferences(*Report);
        ExecutionEnvironment Environment;
        std::vector<uint8_t> InputBytes(16);
        llvm::support::endian::write64le(
            InputBytes.data(), Writes == Write::UnknownPointer && Aliases
                                   ? Frame - 8
                                   : kInputStart + 8);
        Environment.Memory.push_back(
            {kInputStart, InputBytes, true, "explicit write address"});
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        if (Aliases) {
          EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted);
          EXPECT_EQ(Raw->Fault, FaultCode::UnknownIndirectCall);
        } else {
          EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, 300u + Count - 1);
        }
      }
}

TEST(SBFCallXContexts, GenericTargetInputsKeepCompleteAlternatives) {
  for (size_t Alternatives : {2u, 4u, 5u})
    for (bool UnknownLast : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "alternatives=" << Alternatives
                                        << ", unknown=" << UnknownLast);
      std::vector<test::EncodedInstruction> Text;
      std::vector<std::pair<size_t, size_t>> Literals;
      std::vector<size_t> Calls;
      for (size_t I = 0; I < kCallXContextSpecializationsPerEntry; ++I) {
        Literals.emplace_back(Text.size(), I);
        const auto Literal = test::loadImm64(1, 0);
        Text.insert(Text.end(), Literal.begin(), Literal.end());
        Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
        Calls.push_back(Text.size());
        Text.push_back({});
      }
      const auto Input = test::loadImm64(4, kInputStart);
      Text.insert(Text.end(), Input.begin(), Input.end());
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 4));
      std::vector<size_t> Branches, Choices(Alternatives), Joins;
      for (size_t I = 0; I + 1 < Alternatives; ++I) {
        Branches.push_back(Text.size());
        Text.push_back({});
      }
      for (size_t N = 0; N < Alternatives; ++N) {
        const size_t I = (N + Alternatives - 1) % Alternatives;
        Choices[I] = Text.size();
        if (UnknownLast && I + 1 == Alternatives) {
          Text.push_back(encode(Opcode::LD_DW_REG, 1, 4, 8));
        } else {
          Literals.emplace_back(Text.size(), I);
          const auto Literal = test::loadImm64(1, 0);
          Text.insert(Text.end(), Literal.begin(), Literal.end());
        }
        Joins.push_back(Text.size());
        Text.push_back({});
      }
      const size_t Join = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, 99));
      Calls.push_back(Text.size());
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
      const size_t Leaf = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 8, 9));
      Text.push_back(encode(Opcode::JA));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 1));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Targets = Text.size();
      for (size_t I = 0; I < 5; ++I) {
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 41 + I));
        Text.push_back(encode(Opcode::EXIT));
      }
      for (const auto &[Slot, Target] : Literals) {
        const auto Literal =
            test::loadImm64(1, kBytecodeStart + (Targets + Target * 2) * 8);
        std::copy(Literal.begin(), Literal.end(), Text.begin() + Slot);
      }
      for (size_t I = 0; I < Branches.size(); ++I)
        Text[Branches[I]] =
            encode(Opcode::JEQ64_IMM, 5, 0, Choices[I] - Branches[I] - 1, I);
      for (size_t Jump : Joins)
        Text[Jump] = encode(Opcode::JA, 0, 0, Join - Jump - 1);
      for (size_t Call : Calls)
        Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Call - 1);
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      bool SawGeneric = false;
      for (const auto &Site : Report->Contexts)
        if (!Site.EntryPrefixOnly && Site.CallerSlot == Calls.back() &&
            Site.CallXSlot == CallX &&
            Site.InputGeneralization ==
                CallXInputGeneralization::SpecializationLimit) {
          const bool Complete =
              !UnknownLast && Alternatives <= kCallXContextValueAlternatives;
          EXPECT_EQ(Site.Result == CallXContextResult::ProvenTargetSet,
                    Complete);
          EXPECT_EQ(Site.InputExpressionValues.empty(), !Complete);
          if (Complete)
            EXPECT_EQ(Site.TargetValueAlternatives.values().size(),
                      Alternatives);
          EXPECT_FALSE(Site.TargetSlot);
          SawGeneric = true;
        }
      EXPECT_TRUE(SawGeneric);
      checkSummaryReferences(*Report);
      for (size_t Selector = 0; Selector < Alternatives; ++Selector) {
        ExecutionEnvironment Environment;
        std::vector<uint8_t> InputBytes(16);
        llvm::support::endian::write64le(InputBytes.data(), Selector);
        llvm::support::endian::write64le(
            InputBytes.data() + 8,
            kBytecodeStart + (Targets + (Alternatives - 1) * 2) * 8);
        Environment.Memory.push_back(
            {kInputStart, InputBytes, false, "selector"});
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, 41u + Selector);
      }
    }
}

TEST(SBFCallXContexts, GenericTargetCarriersRespectTheCallerABI) {
  for (unsigned Carrier : {4u, 7u}) {
    SCOPED_TRACE(Carrier);
    const size_t Count = kCallXContextSpecializationsPerEntry + 2;
    std::vector<test::EncodedInstruction> Text;
    std::vector<size_t> Literals, Calls;
    for (size_t I = 0; I < Count; ++I) {
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
      Literals.push_back(Text.size());
      const auto Literal = test::loadImm64(3, 0);
      Text.insert(Text.end(), Literal.begin(), Literal.end());
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 3));
      Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
      Calls.push_back(Text.size());
      Text.push_back({});
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Leaf = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 8, 9));
    Text.push_back(encode(Opcode::LD_DW_REG, Carrier, 1));
    const size_t HelperCall = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::JA));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, Carrier));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Helper = Text.size();
    const auto Literal = test::loadImm64(4, 0);
    Text.insert(Text.end(), Literal.begin(), Literal.end());
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Targets = Text.size();
    for (unsigned I = 0; I < 2; ++I) {
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 41 + I));
      Text.push_back(encode(Opcode::EXIT));
    }
    for (size_t I = 0; I < Count; ++I) {
      const auto Target = test::loadImm64(3, kBytecodeStart + Targets * 8);
      std::copy(Target.begin(), Target.end(), Text.begin() + Literals[I]);
      Text[Calls[I]] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Calls[I] - 1);
    }
    Text[HelperCall] =
        encode(Opcode::CALL_IMM, 0, 1, 0, Helper - HelperCall - 1);
    const auto Other = test::loadImm64(4, kBytecodeStart + (Targets + 2) * 8);
    std::copy(Other.begin(), Other.end(), Text.begin() + Helper);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    bool SawGeneric = false;
    for (const auto &Site : Report->Contexts)
      if (!Site.EntryPrefixOnly && Site.CallXSlot == CallX &&
          Site.CallerSlot == Calls.back() &&
          Site.InputGeneralization ==
              CallXInputGeneralization::SpecializationLimit) {
        EXPECT_EQ(Site.TargetSlot.has_value(), Carrier == 7);
        EXPECT_EQ(Site.InputExpressionValues.empty(), Carrier == 4);
        if (Carrier == 7)
          EXPECT_EQ(Site.TargetSlot, Targets);
        SawGeneric = true;
      }
    EXPECT_TRUE(SawGeneric);
    checkSummaryReferences(*Report);
    auto Raw = executeRaw(*Program);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, Carrier == 7 ? 41u : 42u);
  }
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
    EXPECT_EQ(GenericStore, MustStore || Case == StoreCase::Aliased ||
                                Case == StoreCase::Conflicting);
    if (Case == StoreCase::Conflicting)
      for (const auto &Proof : Report->Contexts)
        if (!Proof.EntryPrefixOnly &&
            std::find(CallXs.begin(), CallXs.end(), Proof.CallXSlot) !=
                CallXs.end()) {
          // The zero target is a real alternative, never silently discarded
          // to turn this incomplete dispatcher into a singleton proof.
          EXPECT_FALSE(Proof.TargetSlot);
          EXPECT_EQ(Proof.TargetValueAlternatives.values().size(), 2u);
          EXPECT_TRUE(std::any_of(
              Proof.TargetValueAlternatives.values().begin(),
              Proof.TargetValueAlternatives.values().end(),
              [](const auto &Value) {
                return Value.ValueKind == RegisterValue::Kind::Constant &&
                       Value.Value == 0;
              }));
        }
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

TEST(SBFCallXContexts, GenericStoresKeepCompleteFiniteValues) {
  enum class StoreCase {
    Branches,
    Joined,
    Reloaded,
    Parameters,
    JoinedParameters,
    Missing,
    Unknown,
    Partial,
    Aliased
  };
  for (const auto Case :
       {StoreCase::Branches, StoreCase::Joined, StoreCase::Reloaded,
        StoreCase::Parameters, StoreCase::JoinedParameters, StoreCase::Missing,
        StoreCase::Unknown, StoreCase::Partial, StoreCase::Aliased})
    for (size_t Alternatives : {2u, 4u, 5u}) {
      SCOPED_TRACE(::testing::Message()
                   << "case=" << static_cast<unsigned>(Case)
                   << ", alternatives=" << Alternatives);
      // Both wrapper and writer exhaust their four specializations. The last
      // two calls must instantiate the generic stores using their own output.
      const size_t Count = kCallXContextSpecializationsPerEntry + 2;
      std::vector<test::EncodedInstruction> Text;
      std::vector<size_t> Calls, CallXs, Branches, Choices(Alternatives), Joins;
      std::vector<std::tuple<size_t, unsigned, size_t>> Literals;
      const auto Literal = [&](unsigned Register, size_t Target) {
        Literals.emplace_back(Text.size(), Register, Target);
        const auto Value = test::loadImm64(Register, 0);
        Text.insert(Text.end(), Value.begin(), Value.end());
      };
      const auto Input = test::loadImm64(8, kInputStart);
      Text.insert(Text.end(), Input.begin(), Input.end());
      for (size_t I = 0; I < Count; ++I) {
        Text.push_back(encode(Opcode::MOV64_REG, 6, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 6, 0, 0,
                              -static_cast<int32_t>((I + 1) * 32)));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 6));
        Literal(2, I);
        Literal(3, Count);
        // Initialize the omitted path to a valid target for raw execution.
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 3));
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 3, 8));
        Text.push_back(encode(Opcode::MOV64_REG, 4, 1));
        Text.push_back(encode(Opcode::LD_DW_REG, 5, 8));
        Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
        Calls.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 6));
        CallXs.push_back(Text.size());
        Text.push_back(encode(Opcode::CALL_REG, 2));
        Text.push_back(encode(Opcode::MOV64_REG, 7, 0));
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 6, 8));
        CallXs.push_back(Text.size());
        Text.push_back(encode(Opcode::CALL_REG, 2));
        Text.push_back(encode(Opcode::ADD64_REG, 0, 7));
      }
      Text.push_back(encode(Opcode::EXIT));
      const size_t Middle = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 7, 9));
      Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, 1));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Leaf = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 7, 9));
      for (size_t I = 0; I + 1 < Alternatives; ++I) {
        Branches.push_back(Text.size());
        Text.push_back({});
      }
      for (size_t N = 0; N < Alternatives; ++N) {
        const size_t I = (N + Alternatives - 1) % Alternatives;
        Choices[I] = Text.size();
        if (Case == StoreCase::Parameters ||
            Case == StoreCase::JoinedParameters) {
          Text.push_back(encode(Opcode::MOV64_REG, 0, I % 2 ? 3 : 2));
        } else if (Case == StoreCase::Unknown && I + 1 == Alternatives) {
          Text.push_back(encode(Opcode::LD_DW_REG, 0, 8, 8));
        } else {
          Literal(0, I);
        }
        if (Case != StoreCase::Joined && Case != StoreCase::JoinedParameters &&
            !(Case == StoreCase::Missing && I + 1 == Alternatives)) {
          Text.push_back(encode(Opcode::ST_DW_REG, 1, 0));
          if (Case != StoreCase::Reloaded)
            Text.push_back(encode(Opcode::ST_DW_REG, 1, 0, 8));
        }
        Joins.push_back(Text.size());
        Text.push_back({});
      }
      const size_t Join = Text.size();
      if (Case == StoreCase::Joined || Case == StoreCase::JoinedParameters) {
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 0));
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 0, 8));
      }
      if (Case == StoreCase::Reloaded) {
        Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 0, 8));
      }
      if (Case == StoreCase::Partial)
        Text.push_back(encode(Opcode::ST_B_IMM, 1, 0, 7, 1));
      if (Case == StoreCase::Aliased)
        Text.push_back(encode(Opcode::ST_DW_REG, 4, 3));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Targets = Text.size();
      for (size_t I = 0; I <= Count; ++I) {
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 100 + I));
        Text.push_back(encode(Opcode::EXIT));
      }
      for (const auto &[Slot, Register, Target] : Literals) {
        const auto Value = test::loadImm64(
            Register, kBytecodeStart + (Targets + Target * 2) * 8);
        std::copy(Value.begin(), Value.end(), Text.begin() + Slot);
      }
      for (size_t I = 0; I < Branches.size(); ++I)
        Text[Branches[I]] =
            encode(Opcode::JEQ64_IMM, 5, 0, Choices[I] - Branches[I] - 1, I);
      for (size_t Jump : Joins)
        Text[Jump] = encode(Opcode::JA, 0, 0, Join - Jump - 1);
      for (size_t Call : Calls)
        Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Middle - Call - 1);
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      const bool Complete =
          Case == StoreCase::Parameters ||
          Case == StoreCase::JoinedParameters ||
          (Alternatives <= kCallXContextValueAlternatives &&
           (Case == StoreCase::Branches || Case == StoreCase::Joined ||
            Case == StoreCase::Reloaded));
      for (size_t I = kCallXContextSpecializationsPerEntry; I < Count; ++I) {
        EXPECT_TRUE(std::any_of(
            Report->SummaryEdges.begin(), Report->SummaryEdges.end(),
            [&](const auto &E) {
              return E.CallerSlot == Calls[I] &&
                     E.InputGeneralization ==
                         CallXInputGeneralization::SpecializationLimit;
            }));
        for (size_t Field = 0; Field < 2; ++Field) {
          bool Saw = false;
          for (const auto &Site : Report->Contexts)
            if (!Site.EntryPrefixOnly &&
                Site.CallXSlot == CallXs[I * 2 + Field]) {
              if (Complete) {
                EXPECT_EQ(Site.Result, CallXContextResult::ProvenTargetSet);
                std::set<uint64_t> Expected, Actual;
                if (Case == StoreCase::Parameters ||
                    Case == StoreCase::JoinedParameters) {
                  Expected.insert(kBytecodeStart + (Targets + I * 2) * 8);
                  Expected.insert(kBytecodeStart + (Targets + Count * 2) * 8);
                } else {
                  for (size_t N = 0; N < Alternatives; ++N)
                    Expected.insert(kBytecodeStart + (Targets + N * 2) * 8);
                }
                for (const auto &Value :
                     Site.TargetValueAlternatives.values()) {
                  EXPECT_EQ(Value.ValueKind, RegisterValue::Kind::Constant);
                  Actual.insert(Value.Value);
                }
                EXPECT_EQ(Actual, Expected);
              } else if (Field == 0) {
                // An aliased final store is known to write the caller's r3.
                EXPECT_EQ(Site.Result, Case == StoreCase::Aliased
                                           ? CallXContextResult::ProvenTarget
                                           : CallXContextResult::UnknownTarget);
                if (Case == StoreCase::Aliased)
                  EXPECT_EQ(Site.TargetSlot, Targets + Count * 2);
                else
                  EXPECT_TRUE(Site.TargetValueAlternatives.unknown());
              }
              Saw = true;
            }
          EXPECT_TRUE(Saw);
        }
      }
      checkSummaryReferences(*Report);
      for (size_t Selector = 0; Selector < Alternatives; ++Selector) {
        ExecutionEnvironment Environment;
        std::vector<uint8_t> Bytes(16);
        llvm::support::endian::write64le(Bytes.data(), Selector);
        llvm::support::endian::write64le(
            Bytes.data() + 8, kBytecodeStart + (Targets + Selector * 2) * 8);
        Environment.Memory.push_back({kInputStart, Bytes, false, "selector"});
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        if (Case == StoreCase::Partial) {
          EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted);
          EXPECT_EQ(Raw->FinalSlot, CallXs[0]);
        } else {
          EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          const size_t Selected =
              (Case == StoreCase::Parameters ||
               Case == StoreCase::JoinedParameters)
                  ? (Selector % 2 ? Count : Count - 1)
              : Case == StoreCase::Missing && Selector + 1 == Alternatives
                  ? Count
                  : Selector;
          const size_t First = Case == StoreCase::Aliased ? Count : Selected;
          EXPECT_EQ(Raw->ReturnValue, 200u + First + Selected);
        }
      }
    }
}

TEST(SBFCallXContexts, GenericReturnsKeepCompleteParameterOrigins) {
  enum class Binding { Known, Unknown, Finite };
  for (size_t Alternatives : {2u, 4u, 5u})
    for (bool Frames : {false, true})
      for (bool UnknownLast : {false, true})
        for (bool InsideCall : {false, true})
          for (const auto InputBinding :
               {Binding::Known, Binding::Unknown, Binding::Finite}) {
            if (InputBinding != Binding::Known &&
                (!InsideCall || Frames || UnknownLast))
              continue;
            SCOPED_TRACE(::testing::Message()
                         << "alternatives=" << Alternatives
                         << ", frames=" << Frames << ", unknown=" << UnknownLast
                         << ", inside=" << InsideCall << ", binding="
                         << static_cast<unsigned>(InputBinding));
            const size_t Count = kCallXContextSpecializationsPerEntry + 2;
            std::vector<test::EncodedInstruction> Text;
            std::vector<std::tuple<size_t, unsigned, size_t>> Literals;
            std::vector<size_t> Calls, CallXs, Branches, Choices(Alternatives),
                Joins;
            const auto Input = test::loadImm64(8, kInputStart);
            Text.insert(Text.end(), Input.begin(), Input.end());
            for (size_t I = 0; I < Count; ++I) {
              Text.push_back(encode(Opcode::LD_DW_REG, 7, 8));
              for (unsigned R = 1; R <= Alternatives; ++R) {
                const unsigned Register = Frames ? 6 : R;
                Literals.emplace_back(Text.size(), Register, I + R - 1);
                const auto Value = test::loadImm64(Register, 0);
                Text.insert(Text.end(), Value.begin(), Value.end());
                if (Frames) {
                  Text.push_back(encode(Opcode::MOV64_REG, R, 10));
                  Text.push_back(
                      encode(Opcode::ADD64_IMM, R, 0, 0,
                             -static_cast<int32_t>((I * 5 + R) * 8)));
                  Text.push_back(encode(Opcode::ST_DW_REG, R, 6));
                }
              }
              if (InputBinding == Binding::Unknown)
                Text.push_back(encode(Opcode::LD_DW_REG, 1, 8, 8));
              if (InputBinding == Binding::Finite) {
                // Each argument stays complete, but four symbolic alternatives
                // can bind to five different values. Never select an arbitrary
                // four-value subset.
                Text.push_back(encode(Opcode::JEQ64_IMM, 7, 0, 2));
                Literals.emplace_back(Text.size(), 1, Count + Alternatives - 1);
                const auto Extra = test::loadImm64(1, 0);
                Text.insert(Text.end(), Extra.begin(), Extra.end());
              }
              Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
              Calls.push_back(Text.size());
              Text.push_back({});
              if (!InsideCall) {
                Text.push_back(Frames ? encode(Opcode::LD_DW_REG, 2, 0)
                                      : encode(Opcode::MOV64_REG, 2, 0));
                CallXs.push_back(Text.size());
                Text.push_back(encode(Opcode::CALL_REG, 2));
              }
            }
            Text.push_back(encode(Opcode::EXIT));
            const size_t Middle = Text.size();
            Text.push_back(encode(Opcode::MOV64_REG, 6, 9));
            Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, 1));
            Text.push_back(encode(Opcode::EXIT));
            const size_t Leaf = Text.size();
            Text.push_back(encode(Opcode::MOV64_REG, 6, 9));
            for (size_t I = 0; I + 1 < Alternatives; ++I) {
              Branches.push_back(Text.size());
              Text.push_back({});
            }
            for (size_t N = 0; N < Alternatives; ++N) {
              const size_t I = (N + Alternatives - 1) % Alternatives;
              Choices[I] = Text.size();
              Text.push_back(UnknownLast && I + 1 == Alternatives
                                 ? encode(Opcode::LD_DW_REG, 0, 8, 8)
                                 : encode(Opcode::MOV64_REG, 0, I + 1));
              Joins.push_back(Text.size());
              Text.push_back({});
            }
            const size_t Join = Text.size();
            if (InsideCall) {
              if (Frames)
                Text.push_back(encode(Opcode::LD_DW_REG, 0, 0));
              CallXs.push_back(Text.size());
              Text.push_back(encode(Opcode::CALL_REG, 0));
            }
            Text.push_back(encode(Opcode::EXIT));
            const size_t Targets = Text.size();
            for (size_t I = 0; I < Count + Alternatives; ++I) {
              Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 77 + I));
              Text.push_back(encode(Opcode::EXIT));
            }
            for (const auto &[Slot, Register, Target] : Literals) {
              const auto Value = test::loadImm64(
                  Register, kBytecodeStart + (Targets + Target * 2) * 8);
              std::copy(Value.begin(), Value.end(), Text.begin() + Slot);
            }
            for (size_t I = 0; I < Branches.size(); ++I)
              Text[Branches[I]] = encode(Opcode::JEQ64_IMM, 7, 0,
                                         Choices[I] - Branches[I] - 1, I);
            for (size_t Jump : Joins)
              Text[Jump] = encode(Opcode::JA, 0, 0, Join - Jump - 1);
            for (size_t Call : Calls)
              Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0,
                                  (InsideCall ? Leaf : Middle) - Call - 1);
            auto Program = analyze(test::makeImage(Text, {}));
            ASSERT_TRUE(static_cast<bool>(Program))
                << llvm::toString(Program.takeError());
            auto Report = recoverCallXContexts(*Program);
            ASSERT_TRUE(static_cast<bool>(Report))
                << llvm::toString(Report.takeError());
            const bool OriginsComplete =
                !UnknownLast && Alternatives <= kCallXContextValueAlternatives;
            const bool Complete =
                OriginsComplete && InputBinding != Binding::Unknown &&
                (InputBinding != Binding::Finite ||
                 Alternatives < kCallXContextValueAlternatives);
            for (size_t I = kCallXContextSpecializationsPerEntry; I < Count;
                 ++I) {
              EXPECT_TRUE(std::any_of(
                  Report->SummaryEdges.begin(), Report->SummaryEdges.end(),
                  [&](const auto &E) {
                    return E.CallerSlot == Calls[I] && E.GeneralizedInputs &&
                           E.InputGeneralization ==
                               CallXInputGeneralization::SpecializationLimit;
                  }));
              bool Saw = false;
              for (const auto &Site : Report->Contexts)
                if (!Site.EntryPrefixOnly &&
                    Site.CallXSlot == CallXs[InsideCall ? 0 : I] &&
                    (!InsideCall ||
                     (Site.CallerSlot == Calls[I] &&
                      Site.InputGeneralization ==
                          CallXInputGeneralization::SpecializationLimit))) {
                  EXPECT_EQ(Site.Result,
                            Complete ? CallXContextResult::ProvenTargetSet
                                     : CallXContextResult::UnknownTarget);
                  std::set<uint64_t> Actual, Expected;
                  for (const auto &Value :
                       Site.TargetValueAlternatives.values()) {
                    EXPECT_EQ(Value.ValueKind, RegisterValue::Kind::Constant);
                    Actual.insert(Value.Value);
                  }
                  if (Complete)
                    for (size_t N = 0; N < Alternatives; ++N)
                      Expected.insert(kBytecodeStart +
                                      (Targets + (I + N) * 2) * 8);
                  if (Complete && InputBinding == Binding::Finite)
                    Expected.insert(kBytecodeStart +
                                    (Targets + (Count + Alternatives - 1) * 2) *
                                        8);
                  EXPECT_EQ(Actual, Expected);
                  if (InsideCall && OriginsComplete) {
                    EXPECT_TRUE(Site.InputExpression.has_value());
                    if (Site.InputExpression) {
                      const auto &Root =
                          Report->InputExpressions[*Site.InputExpression];
                      const auto &Union =
                          Frames ? Report->InputExpressions[*Root.Input] : Root;
                      EXPECT_EQ(Union.Kind,
                                CallXInputExpressionKind::InputAlternatives);
                      EXPECT_EQ(Union.Alternatives.size(), Alternatives);
                      EXPECT_EQ(Site.InputExpressionValues.size(),
                                Complete ? Alternatives + 1 + (Frames ? 1 : 0)
                                         : 0u);
                    }
                  }
                  Saw = true;
                }
              EXPECT_TRUE(Saw);
            }
            size_t GenericReturns = 0;
            for (const auto &Summary : Report->Summaries)
              if (Summary.GeneralizedInputs &&
                  (Summary.EntrySlot == Leaf || Summary.EntrySlot == Middle)) {
                EXPECT_EQ(Summary.ReturnInputAlternatives.size(),
                          Complete && !InsideCall ? Alternatives : 0u);
                std::set<unsigned> Registers;
                for (const auto &Origin : Summary.ReturnInputAlternatives) {
                  EXPECT_EQ(Origin.Kind,
                            CallXInputExpressionKind::InputRegister);
                  EXPECT_EQ(Origin.Offset, 0);
                  Registers.insert(Origin.Register);
                }
                if (Complete && !InsideCall)
                  for (unsigned R = 1; R <= Alternatives; ++R)
                    EXPECT_TRUE(Registers.contains(R));
                ++GenericReturns;
              }
            EXPECT_EQ(GenericReturns, 2u);
            const auto JSON = callXContextReportJSON(*Program, *Report);
            const auto *Summaries = JSON.getArray("function_summaries");
            ASSERT_NE(Summaries, nullptr);
            const auto *Nodes = JSON.getArray("target_input_expressions");
            ASSERT_NE(Nodes, nullptr);
            size_t Unions = 0;
            for (const auto &Node : *Nodes) {
              const auto *Expression = Node.getAsObject();
              ASSERT_NE(Expression, nullptr);
              if (Expression->getString("kind") != "entry_input_alternatives")
                continue;
              ++Unions;
              const auto *Inputs = Expression->getArray("inputs");
              ASSERT_NE(Inputs, nullptr);
              EXPECT_GE(Inputs->size(), 2u);
              EXPECT_LE(Inputs->size(), kCallXContextValueAlternatives);
              for (const auto &Input : *Inputs) {
                const auto ID = Input.getAsInteger();
                ASSERT_TRUE(ID);
                const auto Parent = Expression->getInteger("expression");
                ASSERT_TRUE(Parent);
                ASSERT_GE(*ID, 0);
                ASSERT_LT(*ID, *Parent);
                const auto Kind =
                    (*Nodes)[*ID].getAsObject()->getString("kind");
                EXPECT_TRUE(Kind == "entry_register" || Kind == "constant");
              }
            }
            if (InsideCall && OriginsComplete)
              EXPECT_GT(Unions, 0u);
            for (const auto &Value : *Summaries) {
              const auto *Summary = Value.getAsObject();
              ASSERT_NE(Summary, nullptr);
              const auto *Origins =
                  Summary->getArray("return_input_alternatives");
              ASSERT_NE(Origins, nullptr);
              for (const auto &Origin : *Origins) {
                const auto *Expression = Origin.getAsObject();
                ASSERT_NE(Expression, nullptr);
                const auto Kind = Expression->getString("kind");
                EXPECT_TRUE(Kind == "constant" || Kind == "entry_register");
              }
            }
            checkSummaryReferences(*Report);
            for (size_t Selector = 0; Selector < Alternatives; ++Selector) {
              ExecutionEnvironment Environment;
              std::vector<uint8_t> Bytes(16);
              llvm::support::endian::write64le(Bytes.data(), Selector);
              llvm::support::endian::write64le(
                  Bytes.data() + 8,
                  Frames ? initialFramePointer(Program->Low.TheVersion,
                                               Program->Config) -
                               8
                         : kBytecodeStart + Targets * 8);
              Environment.Memory.push_back(
                  {kInputStart, Bytes, false, "selector"});
              auto Raw = executeRaw(*Program, std::move(Environment));
              ASSERT_TRUE(static_cast<bool>(Raw))
                  << llvm::toString(Raw.takeError());
              EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
              EXPECT_EQ(Raw->ReturnValue,
                        ((UnknownLast && Selector + 1 == Alternatives) ||
                         (InputBinding == Binding::Unknown && Selector == 0))
                            ? 77u
                            : 77u + Count - 1 + Selector);
            }
          }
}

TEST(SBFCallXMemoryPostconditions,
     InstantiatesEveryCompleteArgumentAlternative) {
  using namespace callx_detail;
  const RegisterValue Output{RegisterValue::Kind::StackAddress, 2, 0};
  MemoryPostconditions Left, Right;
  Left.store(Output, 8, RegisterValue{RegisterValue::Kind::StackAddress, 3, 0});
  Right.store(Output, 8,
              RegisterValue{RegisterValue::Kind::StackAddress, 4, 0});
  Left.meet(Right);
  auto Program = analyze(test::makeImage(
      {encode(Opcode::MOV64_IMM, 0), encode(Opcode::EXIT)}, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  std::array<ProvenValueSet, kRegisterCount> Entry{};
  Entry[1] = RegisterValue{RegisterValue::Kind::StackAddress, kStackStart, 16};
  Entry[2] = RegisterValue{RegisterValue::Kind::Constant, kBytecodeStart, 0};
  for (unsigned I = 1; I < kCallXContextValueAlternatives; ++I)
    Entry[2].join(RegisterValue{RegisterValue::Kind::Constant,
                                kBytecodeStart + I * 8, 0});
  // This formal is a duplicate of an actual alternative, so there are four
  // results after substitution, not five independently sampled expressions.
  Entry[3] = Entry[2].values().back();
  ScratchState Scratch;
  EXPECT_EQ(Left.apply(Scratch, Entry, *Program), 1u);
  EXPECT_EQ(Scratch.Values.readValues(kStackStart + 16,
                                      MemoryCoordinate::FrameRelative),
            Entry[2]);
  // Distinct fifth and unknown values both absorb the complete fact.
  for (const auto &Value :
       {RegisterValue{RegisterValue::Kind::Constant, kBytecodeStart + 32, 0},
        RegisterValue{}}) {
    Entry[3] = Value;
    Scratch.clear();
    EXPECT_EQ(Left.apply(Scratch, Entry, *Program), 0u);
    EXPECT_TRUE(Scratch.empty());
  }
  // Private formal origins are substituted into tagged caller values. Neither
  // a symbolic input number nor a frame coordinate may become a VM integer.
  Entry[2] = RegisterValue{RegisterValue::Kind::StackAddress, kStackStart, 32};
  Entry[3] = RegisterValue{RegisterValue::Kind::StackAddress, kStackStart, 40};
  EXPECT_EQ(Left.apply(Scratch, Entry, *Program), 1u);
  const auto Stored = Scratch.Values.readValues(
      kStackStart + 16, MemoryCoordinate::FrameRelative);
  ASSERT_EQ(Stored.values().size(), 2u);
  for (const auto &Value : Stored.values())
    EXPECT_EQ(Value.ValueKind, RegisterValue::Kind::StackAddress);
  EXPECT_TRUE(
      Scratch.Values.readValues(kStackStart + 16, MemoryCoordinate::Absolute)
          .unknown());
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
          bool SpecializedLaterTarget = false;
          for (const auto &Site : Report->Contexts) {
            FirstTarget |=
                Site.CallXSlot == Leaf + 8 && Site.TargetSlot == Targets;
            LaterTarget |=
                Site.CallXSlot == Leaf + 5 &&
                Site.TargetSlot == (NewImageArgument ? SecondTarget : Targets);
            SpecializedLaterTarget |=
                Site.CallXSlot == Leaf + 5 &&
                Site.TargetSlot ==
                    (NewImageArgument ? SecondTarget : Targets) &&
                Site.InputExpressionValues.empty();
          }
          EXPECT_EQ(FirstTarget, KnownFirstTarget);
          // Replacing a published summary must preserve the scalar result used
          // by its callers. Equal CALLX targets alone do not authorize
          // replacement.
          const bool Replaced = NewImageArgument && !KnownFirstTarget;
          EXPECT_TRUE(LaterTarget);
          EXPECT_EQ(SpecializedLaterTarget, Replaced);
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
  auto Text = test::loadImm64(2, kBytecodeStart + 8 * 8);
  Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -8));
  Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -8));
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
                          [](const auto &Step) { return Step.Slot == 5; }));
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
      if (Context.InputExpressionValues.empty()) {
        EXPECT_FALSE(Context.TargetSlot);
      } else {
        ASSERT_TRUE(Context.InputExpression);
        ASSERT_EQ(Context.InputExpressionValues.size(), 2u);
        EXPECT_EQ(
            Context.InputExpressionValues.front().Inputs.singleton().Value,
            (Context.CallerSlot / 3) * 8);
        EXPECT_EQ(Context.TargetSlot, Targets + (Context.CallerSlot / 3) * 2);
      }
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

TEST(SBFStoredRegisterMemory, BoundedFrameTranslationStillChecksEveryOverlap) {
  const RegisterValue Number{RegisterValue::Kind::Constant, 42, 0};
  const auto Limit = *frameTranslationLimit(Version::V0, SBFVMConfig{});
  const va_t Cell = kStackStart + 8;
  for (bool AbsoluteWrite : {false, true}) {
    SCOPED_TRACE(AbsoluteWrite);
    StoredRegisterMemory Memory;
    Memory.setFrameTranslationLimit(Limit);
    if (AbsoluteWrite) {
      Memory.write(Cell, Number, MemoryCoordinate::FrameRelative);
      Memory.write(kHeapStart, Number);
    } else {
      Memory.write(kHeapStart, Number);
      Memory.write(Cell, Number, MemoryCoordinate::FrameRelative);
    }
    EXPECT_EQ(Memory.read(Cell, MemoryCoordinate::FrameRelative), Number);
    EXPECT_EQ(Memory.read(kHeapStart), Number);
  }
  for (uint64_t Delta : {uint64_t{0}, Limit / 2, Limit}) {
    StoredRegisterMemory Memory;
    Memory.setFrameTranslationLimit(Limit);
    Memory.write(Cell, Number, MemoryCoordinate::FrameRelative);
    Memory.invalidate(Cell + Delta + 7, 1);
    EXPECT_TRUE(Memory.empty());
    Memory.write(Cell + Delta, Number);
    Memory.invalidate(Cell + 7, 1, MemoryCoordinate::FrameRelative);
    EXPECT_TRUE(Memory.empty());
  }
  StoredRegisterMemory Memory;
  Memory.setFrameTranslationLimit(Limit);
  // Frame-relative arithmetic can reach the heap. Numeric VM regions alone
  // never prove that this cell is on a stack, even with a bounded translation.
  Memory.write(kHeapStart - Limit, Number, MemoryCoordinate::FrameRelative);
  Memory.invalidate(kHeapStart, 8);
  EXPECT_TRUE(Memory.empty());
  Memory.write(Cell, Number, MemoryCoordinate::FrameRelative);
  Memory.invalidate(Cell + Limit + 8, 8);
  EXPECT_EQ(Memory.read(Cell, MemoryCoordinate::FrameRelative), Number);
}

TEST(SBFStoredRegisterMemory, JoinsWidenFrameTranslationBounds) {
  const RegisterValue Number{RegisterValue::Kind::Constant, 42, 0};
  const auto Limit = *frameTranslationLimit(Version::V0, SBFVMConfig{});
  const va_t Cell = kStackStart + 8;
  for (bool Unknown : {false, true})
    for (bool Alternatives : {false, true}) {
      StoredRegisterMemory Left, Right;
      Left.setFrameTranslationLimit(0);
      if (!Unknown)
        Right.setFrameTranslationLimit(Limit);
      Left.write(Cell, Number, MemoryCoordinate::FrameRelative);
      Right.write(Cell, Number, MemoryCoordinate::FrameRelative);
      if (Alternatives)
        Left.joinValues(Right);
      else
        Left.meet(Right);
      Left.invalidate(Unknown ? kHeapStart : Cell + Limit, 8);
      EXPECT_TRUE(Left.empty());
    }
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

TEST(SBFCallXContexts, ReclamationPreservesDistinctCallerEntryEvidence) {
  constexpr uint32_t TableAddress = 0x10000;
  std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
  std::vector<size_t> RootCalls;
  for (size_t I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
    Text.push_back(encode(
        Opcode::MOV64_IMM, 1, 0, 0,
        TableAddress + (I == kCallXContextSpecializationsPerEntry ? 8 : 0)));
    Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, I));
    Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0, I != 0));
    const auto Input = test::loadImm64(4, kInputStart);
    Text.insert(Text.end(), Input.begin(), Input.end());
    RootCalls.push_back(Text.size());
    Text.push_back({});
    Text.push_back(encode(Opcode::EXIT));
  }
  const size_t Dispatcher = Text.size();
  Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
  Text.push_back(encode(Opcode::MOV64_REG, 7, 4));
  Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -8));
  Text.push_back(encode(Opcode::JEQ64_IMM, 3, 0, 3));
  Text.push_back(encode(Opcode::MOV64_REG, 1, 6));
  const size_t SelectB = Text.size();
  Text.push_back({});
  Text.push_back(encode(Opcode::JA, 0, 0, 2));
  Text.push_back(encode(Opcode::MOV64_REG, 1, 6));
  const size_t SelectA = Text.size();
  Text.push_back({});
  Text.push_back(encode(Opcode::LD_DW_REG, 3, 7, 8));
  Text.push_back(encode(Opcode::MOV64_IMM, 0));
  Text.push_back(encode(Opcode::ST_DW_IMM, 3));
  Text.push_back(encode(Opcode::EXIT));
  const size_t A = Text.size();
  const size_t CallA = Text.size();
  Text.push_back({});
  Text.push_back(encode(Opcode::EXIT));
  const size_t B = Text.size();
  const size_t CallB = Text.size();
  Text.push_back({});
  Text.push_back(encode(Opcode::EXIT));
  const size_t Shared = Text.size();
  Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
  const size_t Site = Text.size();
  Text.push_back(encode(Opcode::CALL_REG, 2));
  Text.push_back(encode(Opcode::EXIT));
  const size_t FirstTarget = Text.size();
  Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
  Text.push_back(encode(Opcode::EXIT));
  const size_t SecondTarget = Text.size();
  Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
  Text.push_back(encode(Opcode::EXIT));
  const auto BindCall = [&](size_t Call, size_t Target) {
    Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Target - Call - 1);
  };
  for (size_t Call : RootCalls)
    BindCall(Call, Dispatcher);
  BindCall(SelectA, A);
  BindCall(SelectB, B);
  BindCall(CallA, Shared);
  BindCall(CallB, Shared);
  std::vector<uint8_t> Table(16);
  llvm::support::endian::write64le(Table.data(),
                                   kBytecodeStart + FirstTarget * 8);
  llvm::support::endian::write64le(Table.data() + 8,
                                   kBytecodeStart + SecondTarget * 8);
  auto Image = test::makeImage(Text, Table);
  Image.Sections[1].VA = TableAddress;
  Image.SBF->RodataVM.Address = TableAddress;
  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  const auto ProvenFrom = [&](size_t Caller, size_t Call, size_t Target) {
    return llvm::any_of(Report->Contexts, [&](const auto &Evidence) {
      if (!Evidence.CallerSummaryID || Evidence.CallerSlot != Call ||
          Evidence.CallXSlot != Site || Evidence.TargetSlot != Target)
        return false;
      return llvm::any_of(Report->Summaries, [&](const auto &Summary) {
        return Summary.ID == *Evidence.CallerSummaryID &&
               Summary.EntrySlot == Caller;
      });
    });
  };
  EXPECT_TRUE(ProvenFrom(A, CallA, FirstTarget));
  EXPECT_TRUE(ProvenFrom(B, CallB, SecondTarget));
  EXPECT_GT(Report->GeneralizedPublishedSummaries, 0u);
  for (size_t Entry : {Dispatcher, A, B, Shared})
    EXPECT_LE(llvm::count_if(Report->Summaries,
                             [&](const auto &Summary) {
                               return Summary.EntrySlot == Entry &&
                                      !Summary.GeneralizedInputs;
                             }),
              kCallXContextSpecializationsPerEntry);
}

TEST(SBFCallXContexts, ReclamationRetainsIncomingProofsOfOwnSites) {
  for (bool IndependentTarget : {false, true}) {
    SCOPED_TRACE(IndependentTarget);
    constexpr uint32_t TableAddress = 0x10000;
    std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
    std::vector<size_t> Entries, Calls;
    for (size_t I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
      Entries.push_back(Text.size());
      Text.push_back(encode(
          Opcode::MOV64_IMM, 1, 0, 0,
          TableAddress + (I == kCallXContextSpecializationsPerEntry ? 8 : 0)));
      Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, I));
      const auto Input = test::loadImm64(4, kInputStart);
      Text.insert(Text.end(), Input.begin(), Input.end());
      Calls.push_back(Text.size());
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
    }
    const size_t Leaf = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    Text.push_back(encode(Opcode::MOV64_REG, 7, 4));
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -8));
    const size_t PureCall = Text.size();
    Text.push_back({});
    if (IndependentTarget)
      Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
    Text.push_back(encode(Opcode::LD_DW_REG, 2, 6));
    const size_t Site = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 2));
    Text.push_back(encode(Opcode::LD_DW_REG, 3, 7, 8));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::ST_DW_IMM, 3));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Pure = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0,
                          IndependentTarget ? TableAddress : 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t FirstTarget = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    const size_t SecondTarget = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
    Text.push_back(encode(Opcode::EXIT));
    for (size_t Call : Calls)
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Call - 1);
    Text[PureCall] = encode(Opcode::CALL_IMM, 0, 1, 0, Pure - PureCall - 1);
    std::vector<uint8_t> Table(16);
    llvm::support::endian::write64le(Table.data(),
                                     kBytecodeStart + FirstTarget * 8);
    llvm::support::endian::write64le(Table.data() + 8,
                                     kBytecodeStart + SecondTarget * 8);
    auto Image = test::makeImage(Text, Table);
    Image.Sections[1].VA = TableAddress;
    Image.SBF->RodataVM.Address = TableAddress;
    auto Program = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    for (size_t I = 0; I < Calls.size(); ++I) {
      const size_t Target =
          !IndependentTarget && I == kCallXContextSpecializationsPerEntry
              ? SecondTarget
              : FirstTarget;
      EXPECT_TRUE(llvm::any_of(Report->Contexts,
                               [&](const auto &Proof) {
                                 return Proof.CallerSlot == Calls[I] &&
                                        Proof.CallXSlot == Site &&
                                        Proof.TargetSlot == Target;
                               }))
          << "caller " << I;
      Image.Entry = kBytecodeStart + Entries[I] * kInstructionSize;
      auto Selected = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Selected))
          << llvm::toString(Selected.takeError());
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input(16);
      llvm::support::endian::write64le(Input.data() + 8, kHeapStart + 16);
      Environment.Memory.push_back({kInputStart, Input, false, "input"});
      Environment.Memory.push_back(
          {kHeapStart, std::vector<uint8_t>(64), true, "heap"});
      auto Raw = executeRaw(*Selected, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 0u);
      EXPECT_TRUE(llvm::any_of(
          Raw->Trace, [&](const auto &Step) { return Step.Slot == Target; }));
    }
    EXPECT_EQ(Report->GeneralizedPublishedSummaries > 0, IndependentTarget);
  }
}

TEST(SBFCallXContexts, UnreadDescriptorBytesDoNotDisplaceTypedCallerFacts) {
  std::vector<test::EncodedInstruction> Text;
  const auto TargetAddress = test::loadImm64(6, 0);
  Text.insert(Text.end(), TargetAddress.begin(), TargetAddress.end());
  Text.push_back(encode(Opcode::ST_DW_REG, 10, 6, -16));
  std::vector<size_t> Calls;
  for (unsigned I = 0; I < kCallXContextSpecializationsPerEntry + 2; ++I) {
    Text.push_back(encode(Opcode::ST_B_IMM, 10, 0, -64, I));
    Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
    Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Calls.push_back(Text.size());
    Text.push_back({});
  }
  Text.push_back(encode(Opcode::EXIT));
  const size_t Leaf = Text.size();
  Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
  Text.push_back(encode(Opcode::CALL_REG, 2));
  Text.push_back(encode(Opcode::EXIT));
  const size_t Target = Text.size();
  Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
  Text.push_back(encode(Opcode::EXIT));
  const auto Literal = test::loadImm64(6, kBytecodeStart + Target * 8);
  std::copy(Literal.begin(), Literal.end(), Text.begin());
  for (size_t Slot : Calls)
    Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Slot - 1);
  auto Program = analyze(test::makeImage(Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  EXPECT_EQ(llvm::count_if(Report->Summaries,
                           [&](const auto &Summary) {
                             return Summary.EntrySlot == Leaf &&
                                    !Summary.GeneralizedInputs &&
                                    Summary.Inputs[1].ValueKind !=
                                        RegisterValue::Kind::Unknown;
                           }),
            1);
  for (size_t Slot : Calls)
    EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
      return Site.CallerSlot == Slot && Site.TargetSlot == Target;
    }));
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 42u);
}

} // namespace
} // namespace neverd::sbf
