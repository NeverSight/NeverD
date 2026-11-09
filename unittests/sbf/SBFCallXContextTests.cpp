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
#include <cstring>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <string>
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
        EXPECT_EQ(JSON.getInteger("schema"), 16);
        const auto *Frame = JSON.getObject("frame_coordinates");
        ASSERT_NE(Frame, nullptr);
        EXPECT_EQ(Frame->getString("namespace").value_or(""),
                  "owning_function_summary");
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
    bool ConcreteInputWitness = false;
    for (const auto &Site : Report->Contexts)
      if (!Site.EntryPrefixOnly && Site.CallXSlot == Leaf + 3 &&
          !Site.GeneralizedInputs && Site.CallerSummaryID && Site.TargetSlot &&
          (Layers != 0 ||
           Site.InputGeneralization ==
               CallXInputGeneralization::EquivalentSpecializedInputs)) {
        EXPECT_FALSE(Site.InputExpressionValues.empty());
        ConcreteInputWitness |= !Site.InputExpressionValues.empty();
      }
    EXPECT_TRUE(ConcreteInputWitness);
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

TEST(SBFStoredRegisterMemory, AlignmentMasksProjectKnownCaptureBounds) {
  auto Program = analyze(test::makeImage({encode(Opcode::EXIT)}, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Addresses = std::make_shared<SymbolicAddressSpace>();
  Addresses->push_back({kHeapStart + 8, UINT64_MAX});
  const RegisterValue Capture{RegisterValue::Kind::SymbolicAddress, 0, 0};
  for (bool Local : {false, true})
    for (bool WithMemory : {false, true})
      for (bool Immediate : {false, true})
        for (int64_t Offset : {-24, 0, 24})
          for (uint64_t Mask :
               {uint64_t{0}, UINT64_MAX - 7, UINT64_MAX - 15, UINT64_MAX - 127,
                UINT64_MAX, uint64_t{0xf0f0}}) {
            SCOPED_TRACE(::testing::Message()
                         << Local << ", memory=" << WithMemory
                         << ", immediate=" << Immediate << ", offset=" << Offset
                         << ", mask=" << Mask);
            StoredRegisterMemory Memory;
            Memory.setSymbolicAddresses(Addresses);
            if (Local)
              Memory.refineCapturedRange(Capture,
                                         {kHeapStart + 8, kHeapStart + 4096});
            RegisterState State{};
            State[0] = Capture;
            State[0].Offset = Offset;
            const auto Input = State[0];
            State[1] = {RegisterValue::Kind::Constant, Mask, 0};
            const auto &Info = *getOpcodeInfo(Immediate ? Opcode::AND64_IMM
                                                        : Opcode::AND64_REG);
            MedInstruction I;
            I.Op = Info.Op;
            I.Form = Info.Form;
            I.Width = Info.Width;
            I.Src = 1;
            I.Immediate = Mask;
            I.Semantics = semanticTraits(Info, Version::V0);
            I.Semantics.Immediate = ImmediateExtension::Full64;
            const auto Bounds = Memory.valueRange(Input);
            applyRegisterTransfer(I, State, Program->ExecutableImage,
                                  WithMemory ? &Memory : nullptr);
            if (Mask == UINT64_MAX) {
              EXPECT_EQ(State[0], Input);
            } else if (WithMemory && Bounds && Mask != 0xf0f0) {
              EXPECT_EQ(State[0], unsignedRangeValue({Bounds->Lower & Mask,
                                                      Bounds->Upper & Mask}));
              // Check concrete bit operations independently of the abstract
              // transfer, including both endpoints and an interior value.
              for (uint64_t Value :
                   {Bounds->Lower, Bounds->Upper,
                    Bounds->Lower + (Bounds->Upper - Bounds->Lower) / 2}) {
                const auto Result = unsignedValueRange(State[0]);
                ASSERT_TRUE(Result);
                EXPECT_LE(Result->Lower, Value & Mask);
                EXPECT_GE(Result->Upper, Value & Mask);
              }
            } else {
              EXPECT_EQ(State[0].ValueKind, RegisterValue::Kind::Unknown);
            }
          }
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
    EXPECT_EQ(GenericStore, MustStore || Case == StoreCase::Partial ||
                                Case == StoreCase::Aliased ||
                                Case == StoreCase::Conflicting);
    if (Case == StoreCase::Partial)
      for (const auto &Summary : Report->Summaries)
        if (Summary.GeneralizedInputs &&
            (Summary.EntrySlot == Middle || Summary.EntrySlot == Leaf)) {
          // The byte is a real scalar postcondition, never a restored pointer.
          EXPECT_EQ(Summary.OutputParameterizedWords, 0u);
          EXPECT_EQ(Summary.OutputNarrowParameterizedFields, 1u);
        }
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
                EXPECT_TRUE(Kind == "entry_register" || Kind == "constant" ||
                            Kind == "normalized_instruction");
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

TEST(SBFStoredRegisterMemory, GuardedCaptureBoundsStayOnTheirOwnPaths) {
  const RegisterValue Value{RegisterValue::Kind::Constant, 42, 0};
  const RegisterValue Pointer{RegisterValue::Kind::SymbolicAddress, 0, 0};
  const RegisterValue Allocated{RegisterValue::Kind::SymbolicAddress, 0, -24};
  const RegisterValue Independent{RegisterValue::Kind::SymbolicAddress, 1, -24};
  auto Addresses = std::make_shared<SymbolicAddressSpace>(SymbolicAddressSpace{
      {kHeapStart + 8, kHeapStart + 128}, {kHeapStart + 8, kHeapStart + 128}});
  StoredRegisterMemory Before;
  Before.setSymbolicAddresses(Addresses);
  Before.write(kHeapStart, Value);
  Before.writeValues(Pointer, Value);
  StoredRegisterMemory Success = Before;
  Success.refineCapturedRange(Allocated, {kHeapStart + 8, kHeapStart + 104});
  EXPECT_EQ(Success.valueRange(Pointer),
            (UnsignedValueRange{kHeapStart + 32, kHeapStart + 128}));
  EXPECT_EQ(Before.valueRange(Pointer),
            (UnsignedValueRange{kHeapStart + 8, kHeapStart + 128}));
  EXPECT_EQ((*Addresses)[0],
            (UnsignedValueRange{kHeapStart + 8, kHeapStart + 128}));
  EXPECT_GT(Success.retainedByteEstimate(), Before.retainedByteEstimate());
  llvm::FoldingSetNodeID BeforeKey, SuccessKey;
  Before.profile(BeforeKey);
  Success.profile(SuccessKey);
  EXPECT_NE(BeforeKey, SuccessKey);
  auto Written = Success;
  Written.invalidate(Allocated, 8);
  EXPECT_EQ(Written.read(kHeapStart), Value);
  EXPECT_EQ(Written.readValues(Pointer).singleton(), Value);
  Written.invalidate(Independent, 8);
  EXPECT_EQ(Written.read(kHeapStart), RegisterValue{});
  EXPECT_TRUE(Written.readValues(Pointer).unknown());

  // A successful edge must not refine the sibling or a join with that sibling.
  // Test both merge orders and both equality-meet and finite-value union.
  for (bool Reverse : {false, true})
    for (bool Union : {false, true}) {
      auto Joined = Reverse ? Before : Success;
      const auto &Other = Reverse ? Success : Before;
      if (Union)
        Joined.joinValues(Other);
      else
        Joined.meet(Other);
      EXPECT_EQ(Joined.valueRange(Pointer), Before.valueRange(Pointer));
      Joined.invalidate(Allocated, 8);
      EXPECT_EQ(Joined.read(kHeapStart), RegisterValue{});
    }
  auto Weaker = Before;
  Weaker.refineCapturedRange(Allocated, {kHeapStart - 8, kHeapStart + 104});
  auto Joined = Success;
  Joined.joinValues(Weaker);
  EXPECT_EQ(Joined.valueRange(Allocated),
            (UnsignedValueRange{kHeapStart - 8, kHeapStart + 104}));
  Joined.invalidate(Allocated, 8);
  EXPECT_EQ(Joined.read(kHeapStart), RegisterValue{});
  auto Overlay = Before;
  Overlay.includeCells(Success, [](const auto &) { return true; });
  EXPECT_EQ(Overlay.valueRange(Pointer), Before.valueRange(Pointer));
  auto Rebound = Success;
  Rebound.transformSymbols([](RegisterValue V) { return V; });
  EXPECT_EQ(Rebound.valueRange(Pointer), Before.valueRange(Pointer));
  auto Reconfigured = Success;
  auto OtherAddresses = std::make_shared<SymbolicAddressSpace>(*Addresses);
  Reconfigured.setCoordinates(std::make_shared<StoredMemoryCoordinates>(
      StoredMemoryCoordinates{std::nullopt, OtherAddresses}));
  EXPECT_EQ(Reconfigured.valueRange(Pointer), Before.valueRange(Pointer));
  Success.discardCapturedRanges();
  EXPECT_EQ(Success, Before);

  // An interval crossing zero is not an unsigned scalar bound. Refinement
  // must not silently keep only its low or high fragment.
  const RegisterValue Wrapped{Pointer.ValueKind, Pointer.Value,
                              -static_cast<int64_t>(kHeapStart + 24)};
  EXPECT_FALSE(Before.valueRange(Wrapped));
  Before.refineCapturedRange(Wrapped, {0, 104});
  EXPECT_FALSE(Before.valueRange(Wrapped));
  EXPECT_EQ(Before.valueRange(Wrapped, true),
            (UnsignedValueRange{UINT64_MAX - 15, 104}));
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
                !Site.GeneralizedInputs;
          }
          EXPECT_EQ(FirstTarget, KnownFirstTarget);
          // Replacing a published summary must preserve the scalar result used
          // by its callers. Equal CALLX targets alone do not authorize
          // replacement.
          const bool Replaced =
              NewImageArgument && (!KnownFirstTarget || !DistinctDescendants);
          if (KnownFirstTarget)
            for (size_t I = 0; I < kCallXContextSpecializationsPerEntry; ++I)
              EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
                return Site.CallerSlot == Calls[I] &&
                       Site.CallXSlot == Leaf + 8 && Site.TargetSlot == Targets;
              }));
          checkSummaryReferences(*Report);
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

TEST(SBFCallXContexts, MandatoryGuestRecursionHasNoReturningContinuation) {
  for (bool BaseCase : {false, true}) {
    SCOPED_TRACE(BaseCase);
    const uint32_t Abort = findSyscallByName("abort")->Hash;
    auto Text = test::loadImm64(6, kBytecodeStart + 14 * 8);
    for (const auto &I :
         {encode(Opcode::ST_DW_REG, 10, 6, -8),
          encode(Opcode::MOV64_IMM, 1, 0, 0, 0),
          encode(Opcode::CALL_IMM, 0, 1, 0, 3),
          encode(Opcode::LD_DW_REG, 6, 10, -8), encode(Opcode::CALL_REG, 6),
          encode(Opcode::EXIT), encode(Opcode::CALL_IMM, 0, 0, 0, Abort),
          encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
          BaseCase ? encode(Opcode::JEQ64_IMM, 1, 0, 1, 0)
                   : encode(Opcode::MOV64_REG, 1, 1),
          encode(Opcode::CALL_IMM, 0, 1, 0, -5), encode(Opcode::EXIT),
          encode(Opcode::MOV64_IMM, 0, 0, 0, 42), encode(Opcode::EXIT)})
      Text.push_back(I);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    ASSERT_EQ(Report->UnresolvedCallXSlots.size(), 1u);
    bool Continuation = false;
    for (const auto &Site : Report->Contexts)
      if (!Site.EntryPrefixOnly && Site.CallXSlot == 6)
        Continuation = true;
    EXPECT_EQ(Continuation, BaseCase);
    for (const auto &Summary : Report->Summaries)
      if (Summary.EntrySlot == 8 || Summary.EntrySlot == 11)
        EXPECT_EQ(Summary.MayReturn, BaseCase);
    ExecutionEnvironment Environment;
    Environment.HostSyscall = [](uint32_t, const SyscallArguments &) {
      return SyscallOutcome::returned(0);
    };
    auto Raw = executeRaw(*Program, Environment);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    if (BaseCase) {
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 42u);
    } else {
      EXPECT_EQ(Raw->Fault, FaultCode::CallDepth);
      EXPECT_FALSE(llvm::any_of(
          Raw->Trace, [](const auto &Step) { return Step.Slot == 6; }));
    }
  }
}

TEST(SBFCallXContexts, NonReturningCalleeKeepsItsCallerTargetEvidence) {
  auto Text = test::loadImm64(1, kBytecodeStart + 9 * 8);
  for (const auto &I :
       {encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
        encode(Opcode::MOV64_REG, 6, 1), encode(Opcode::CALL_REG, 6),
        encode(Opcode::MOV64_REG, 1, 6), encode(Opcode::CALL_IMM, 0, 1, 0, -4),
        encode(Opcode::EXIT), encode(Opcode::MOV64_IMM, 0, 0, 0, 42),
        encode(Opcode::EXIT)})
    Text.push_back(I);
  auto Program = analyze(test::makeImage(Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  checkSummaryReferences(*Report);
  EXPECT_TRUE(llvm::any_of(Report->Contexts, [](const auto &Site) {
    return !Site.EntryPrefixOnly && Site.CallerSlot == 2 &&
           Site.CallXSlot == 5 && Site.TargetSlot == 9;
  }));
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Fault, FaultCode::CallDepth);
  EXPECT_TRUE(llvm::any_of(Raw->Trace,
                           [](const auto &Step) { return Step.Slot == 5; }));
}

TEST(SBFCallXContexts, NonReturningCalleeDoesNotHavocAReturningSibling) {
  auto Text = test::loadImm64(2, kBytecodeStart + 18 * 8);
  for (const auto &I :
       {encode(Opcode::ST_DW_REG, 10, 2, -8),
        encode(Opcode::CALL_IMM, 0, 1, 0, 3),
        encode(Opcode::LD_DW_REG, 2, 10, -8), encode(Opcode::CALL_REG, 2),
        encode(Opcode::EXIT), encode(Opcode::JEQ64_IMM, 3, 0, 2, 0),
        encode(Opcode::CALL_IMM, 0, 1, 0, 2), encode(Opcode::EXIT),
        encode(Opcode::EXIT), encode(Opcode::ST_DW_REG, 4, 5),
        encode(Opcode::CALL_IMM, 0, 1, 0, 1), encode(Opcode::EXIT),
        encode(Opcode::CALL_IMM, 0, 1, 0, -4), encode(Opcode::EXIT),
        encode(Opcode::EXIT), encode(Opcode::EXIT),
        encode(Opcode::MOV64_IMM, 0, 0, 0, 77), encode(Opcode::EXIT)})
    Text.push_back(I);
  auto Program = analyze(test::makeImage(Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  checkSummaryReferences(*Report);
  EXPECT_TRUE(llvm::any_of(Report->Contexts, [](const auto &Site) {
    return !Site.EntryPrefixOnly && Site.CallXSlot == 5 &&
           Site.TargetSlot == 18;
  }));
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 77u);
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
    Recursive |= Edge.Barrier == CallXSummaryBarrier::NonReturningCall;
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

TEST(SBFCallXContexts, UnblockedCallerCanReplaceAnOpaqueRecursiveCut) {
  std::vector<test::EncodedInstruction> Text;
  Text.push_back(encode(Opcode::MOV64_IMM, 1));
  const size_t FirstCall = Text.size();
  Text.push_back({});
  const size_t FirstCallX = Text.size();
  Text.push_back(encode(Opcode::CALL_REG, 0));
  Text.push_back(encode(Opcode::MOV64_IMM, 1));
  const size_t SecondCall = Text.size();
  Text.push_back({});
  const size_t SecondCallX = Text.size();
  Text.push_back(encode(Opcode::CALL_REG, 0));
  Text.push_back(encode(Opcode::EXIT));
  const size_t Ancestor = Text.size();
  const size_t Branch = Text.size();
  Text.push_back({});
  const size_t NestedCall = Text.size();
  Text.push_back({});
  Text.push_back(encode(Opcode::EXIT));
  const size_t Direct = Text.size();
  const size_t TargetLiteral = Text.size();
  const auto Placeholder = test::loadImm64(0, 0);
  Text.insert(Text.end(), Placeholder.begin(), Placeholder.end());
  Text.push_back(encode(Opcode::EXIT));
  const size_t Helper = Text.size();
  Text.push_back(encode(Opcode::ST_DW_REG, 10, 1, -8));
  Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, 1));
  const size_t CutCall = Text.size();
  Text.push_back({});
  Text.push_back(encode(Opcode::EXIT));
  const size_t OtherCaller = Text.size();
  const size_t Unblocked = Text.size();
  Text.push_back({});
  Text.push_back(encode(Opcode::EXIT));
  const size_t Target = Text.size();
  Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
  Text.push_back(encode(Opcode::EXIT));
  for (auto [Call, Entry] :
       {std::pair{FirstCall, Ancestor}, std::pair{SecondCall, OtherCaller},
        std::pair{NestedCall, Helper}, std::pair{CutCall, Ancestor},
        std::pair{Unblocked, Helper}})
    Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Entry - Call - 1);
  Text[Branch] = encode(Opcode::JNE64_IMM, 1, 0, Direct - Branch - 1);
  const auto Literal = test::loadImm64(0, kBytecodeStart + Target * 8);
  std::copy(Literal.begin(), Literal.end(), Text.begin() + TargetLiteral);
  auto Program = analyze(test::makeImage(Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  checkSummaryReferences(*Report);
  EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
    return !Site.EntryPrefixOnly && Site.CallXSlot == SecondCallX &&
           Site.TargetSlot == Target;
  }));
  EXPECT_GT(Report->UnblockedRecursionRetries, 0u);
  EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
    return Edge.CallerSlot == NestedCall &&
           Edge.InputGeneralization ==
               CallXInputGeneralization::ReplacedRecursiveContext;
  }));
  auto Raw = executeRaw(*Program);
  ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
  EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
  EXPECT_EQ(Raw->ReturnValue, 42u);
  for (size_t Slot : {FirstCallX, SecondCallX}) {
    auto At = llvm::find_if(
        Raw->Trace, [&](const auto &Step) { return Step.Slot == Slot; });
    ASSERT_NE(At, Raw->Trace.end());
    ASSERT_NE(std::next(At), Raw->Trace.end());
    EXPECT_EQ(std::next(At)->Slot, Target);
  }
}

TEST(SBFCallXContexts, UnknownVariantDoesNotHideANewImageBinding) {
  constexpr size_t Count = kCallXContextSpecializationsPerEntry + 1;
  std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
  const auto AddLiteral = [&](unsigned Register, uint64_t Value) {
    const auto Words = test::loadImm64(Register, Value);
    Text.insert(Text.end(), Words.begin(), Words.end());
  };
  std::vector<size_t> Entries, Calls;
  for (size_t I = 0; I < Count; ++I) {
    Entries.push_back(Text.size());
    AddLiteral(9, kInputStart);
    Text.push_back(encode(Opcode::MOV64_IMM, 8, 0, 0, I));
    Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
    Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16 * (I + 1)));
    if (I == 0)
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 9));
    else
      AddLiteral(2, kRodataStartV3 + (I + 1 == Count ? 8 : 0));
    AddLiteral(4, kInputStart + 8);
    Calls.push_back(Text.size());
    Text.push_back({});
    Text.push_back(encode(Opcode::EXIT));
  }
  const size_t Helper = Text.size();
  Text.push_back(encode(Opcode::LD_DW_REG, 6, 4));
  Text.push_back(encode(Opcode::ST_DW_REG, 1, 8));
  Text.push_back(encode(Opcode::ST_DW_IMM, 6));
  Text.push_back(encode(Opcode::LD_DW_REG, 5, 2));
  const size_t CallX = Text.size();
  Text.push_back(encode(Opcode::CALL_REG, 5));
  Text.push_back(encode(Opcode::ST_DW_IMM, 6));
  Text.push_back(encode(Opcode::MOV64_IMM, 0));
  Text.push_back(encode(Opcode::EXIT));
  std::array<size_t, 2> Targets;
  for (unsigned I = 0; I < Targets.size(); ++I) {
    Targets[I] = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42 + I));
    Text.push_back(encode(Opcode::EXIT));
  }
  for (size_t Call : Calls)
    Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Call - 1);
  std::vector<uint8_t> Table(16);
  for (unsigned I = 0; I < Targets.size(); ++I)
    llvm::support::endian::write64le(Table.data() + I * 8,
                                     kBytecodeStart + Targets[I] * 8);
  auto Image = test::makeImage(Text, Table);
  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  checkSummaryReferences(*Report);
  for (size_t I = 1; I < Count; ++I)
    EXPECT_TRUE(llvm::any_of(Report->Contexts,
                             [&](const auto &Site) {
                               return !Site.EntryPrefixOnly &&
                                      !Site.GeneralizedInputs &&
                                      Site.CallerSlot == Calls[I] &&
                                      Site.CallXSlot == CallX &&
                                      Site.TargetSlot ==
                                          Targets[I + 1 == Count ? 1 : 0];
                             }))
        << "caller=" << I;
  for (size_t I = 0; I < Count; ++I) {
    Image.Entry = kBytecodeStart + Entries[I] * 8;
    auto Selected = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Selected))
        << llvm::toString(Selected.takeError());
    ExecutionEnvironment Environment;
    std::vector<uint8_t> Input(16);
    llvm::support::endian::write64le(Input.data(), kRodataStartV3);
    llvm::support::endian::write64le(Input.data() + 8, kHeapStart);
    Environment.Memory.push_back({kInputStart, Input, false, "pointers"});
    Environment.Memory.push_back(
        {kHeapStart, std::vector<uint8_t>(8), true, "output"});
    auto Raw = executeRaw(*Selected, std::move(Environment));
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, 0u);
    auto At = llvm::find_if(
        Raw->Trace, [&](const auto &Step) { return Step.Slot == CallX; });
    ASSERT_NE(At, Raw->Trace.end());
    ASSERT_NE(std::next(At), Raw->Trace.end());
    EXPECT_EQ(std::next(At)->Slot, Targets[I + 1 == Count ? 1 : 0]);
  }
}

TEST(SBFStoredRegisterMemory, FrameTranslationIsTypedAndAtomic) {
  auto Program = analyze(test::makeImage({encode(Opcode::EXIT)}, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const uint64_t FP =
      initialFramePointer(Program->Low.TheVersion, Program->Config);
  const int64_t Stride =
      automaticFrameStride(Program->Low.TheVersion, Program->Config);
  ASSERT_GT(Stride, 0);
  StoredRegisterMemory Memory;
  Memory.setFrameTranslationLimit(
      frameTranslationLimit(Program->Low.TheVersion, Program->Config));
  const RegisterValue Cell{RegisterValue::Kind::StackAddress, FP, -16};
  const RegisterValue Pointer{RegisterValue::Kind::StackAddress, FP, -32};
  const RegisterValue Literal{RegisterValue::Kind::Constant, FP, 0};
  const RegisterValue Heap{RegisterValue::Kind::Constant, kHeapStart, 0};
  const RegisterValue OtherHeap{RegisterValue::Kind::Constant, kHeapStart + 8,
                                0};
  Memory.writeValues(Cell, Pointer);
  Memory.writeValues(Heap, Literal);
  Memory.writeValues(OtherHeap, Pointer);
  ASSERT_EQ(Memory.size(), 3u);
  ASSERT_TRUE(Memory.translateFrameCoordinates(-Stride));
  const RegisterValue MovedCell{RegisterValue::Kind::StackAddress, FP,
                                -16 - Stride};
  const RegisterValue MovedPointer{RegisterValue::Kind::StackAddress, FP,
                                   -32 - Stride};
  EXPECT_EQ(Memory.readValues(MovedCell).singleton(), MovedPointer);
  EXPECT_EQ(Memory.readValues(OtherHeap).singleton(), MovedPointer);
  EXPECT_EQ(Memory.readValues(Heap).singleton(), Literal);
  EXPECT_TRUE(Memory.readValues(Cell).unknown());
  EXPECT_TRUE(Memory.canModelRange(MovedCell, 8));
  EXPECT_FALSE(Memory.canModelRange(
      {RegisterValue::Kind::Constant, FP - 16 - Stride, 0}, 8));
  EXPECT_FALSE(Memory.canModelRange(
      {RegisterValue::Kind::StackAddress, kBytecodeStart, 0}, 8));
  EXPECT_FALSE(Memory.canModelRange(
      {RegisterValue::Kind::Constant, kBytecodeStart, 0}, 8));
  Memory.writeValues(
      {RegisterValue::Kind::Constant, kHeapStart + 16, 0},
      RegisterValue{RegisterValue::Kind::StackAddress, FP, INT64_MAX});
  const auto Before = Memory;
  EXPECT_FALSE(Memory.translateFrameCoordinates(1));
  EXPECT_EQ(Memory, Before);
  Memory.invalidate(RegisterValue{}, 8);
  EXPECT_TRUE(Memory.empty());
  Memory.writeValues({RegisterValue::Kind::StackAddress, UINT64_MAX - 16, 0},
                     Literal);
  const auto BeforeWindowWrap = Memory;
  EXPECT_FALSE(Memory.translateFrameCoordinates(12));
  EXPECT_EQ(Memory, BeforeWindowWrap);
}

TEST(SBFCallXContexts, EquivalentCallerFramesDoNotConsumeExtraVariants) {
  for (bool PointerReturn : {false, true})
    for (bool UnknownSelector : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "pointer=" << PointerReturn
                                        << ", unknown=" << UnknownSelector);
      constexpr size_t Count = kCallXContextSpecializationsPerEntry + 2;
      std::vector<test::EncodedInstruction> Text;
      const auto AddLiteral = [&](unsigned Register, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(Register, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      std::vector<size_t> Calls, CallXs;
      AddLiteral(9, kInputStart);
      for (size_t I = 0; I < Count; ++I) {
        // Forget prior calls' scratch facts to isolate frame-coordinate reuse.
        // The raw pointer names the writable heap; the analyzer cannot load it.
        Text.push_back(encode(Opcode::LD_DW_REG, 5, 9, Count * 8));
        Text.push_back(encode(Opcode::ST_DW_IMM, 5));
        Text.push_back(UnknownSelector ? encode(Opcode::LD_DW_REG, 1, 9, I * 8)
                                       : encode(Opcode::MOV64_IMM, 1));
        Text.push_back(encode(Opcode::MOV64_REG, 2, 9));
        AddLiteral(4, kHeapStart);
        Calls.push_back(Text.size());
        Text.push_back({});
        if (PointerReturn)
          Text.push_back(encode(Opcode::LD_DW_REG, 0, 0));
        CallXs.push_back(Text.size());
        Text.push_back(encode(Opcode::CALL_REG, 0));
      }
      Text.push_back(encode(Opcode::EXIT));
      const size_t Helper = Text.size();
      const size_t TargetLiteral = AddLiteral(3, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -16));
      // The formal pointer may alias this spill; its concrete heap binding
      // cannot. Returning scratch must restore its frame keys as well as r0.
      Text.push_back(encode(Opcode::ST_DW_IMM, 4));
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 1, -8));
      Text.push_back(encode(Opcode::LD_DW_REG, 1, 10, -8));
      const size_t Branch = Text.size();
      Text.push_back({});
      if (PointerReturn) {
        Text.push_back(encode(Opcode::MOV64_REG, 0, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 0, 0, 0, -16));
      } else {
        Text.push_back(encode(Opcode::MOV64_REG, 0, 3));
      }
      Text.push_back(encode(Opcode::EXIT));
      const size_t Opaque = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 2));
      Text.push_back(encode(Opcode::EXIT));
      Text[Branch] = encode(Opcode::JNE64_IMM, 1, 0, Opaque - Branch - 1);
      for (size_t I = 0; I < Count; ++I) {
        size_t Entry = Helper;
        for (size_t Layer = 0; Layer < I; ++Layer) {
          const size_t Wrapper = Text.size();
          Text.push_back(
              encode(Opcode::CALL_IMM, 0, 1, 0, Entry - Wrapper - 1));
          Text.push_back(encode(Opcode::EXIT));
          Entry = Wrapper;
        }
        Text[Calls[I]] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Entry - Calls[I] - 1);
      }
      const size_t Target = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const auto Words = test::loadImm64(3, kBytecodeStart + Target * 8);
      std::copy(Words.begin(), Words.end(), Text.begin() + TargetLiteral);
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      for (size_t Slot : CallXs) {
        const bool Proven =
            llvm::any_of(Report->Contexts, [&](const auto &Site) {
              return !Site.EntryPrefixOnly && Site.CallXSlot == Slot &&
                     Site.TargetSlot == Target;
            });
        EXPECT_EQ(Proven, !UnknownSelector) << "slot=" << Slot;
      }
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input((Count + 1) * 8);
      llvm::support::endian::write64le(Input.data() + Count * 8, kHeapStart);
      Environment.Memory.push_back({kInputStart, Input, false, "selectors"});
      Environment.Memory.push_back(
          {kHeapStart, std::vector<uint8_t>(8), true, "written heap"});
      auto Raw = executeRaw(*Program, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 42u);
      for (size_t Slot : CallXs) {
        auto At = llvm::find_if(
            Raw->Trace, [&](const auto &Step) { return Step.Slot == Slot; });
        ASSERT_NE(At, Raw->Trace.end());
        ASSERT_NE(std::next(At), Raw->Trace.end());
        EXPECT_EQ(std::next(At)->Slot, Target);
      }
    }
}

TEST(SBFCallXContexts, BranchLoadedFieldsKeepCompleteExpressionAlternatives) {
  // Modes cover returned and stored fields, mixed affine/load sources, an
  // unknown source, an aliasing write, and the existing complete-value limit.
  for (unsigned Mode = 0; Mode < 9; ++Mode) {
    SCOPED_TRACE(Mode);
    const bool Stored = Mode == 1 || Mode == 4;
    const size_t Choices = Mode == 5   ? kCallXContextValueAlternatives + 1
                           : Mode == 8 ? kCallXContextValueAlternatives
                                       : 2;
    const bool Complete = Mode < 3 || Mode == 6 || Mode == 8;
    constexpr size_t Count = kCallXContextSpecializationsPerEntry + 2;
    std::vector<test::EncodedInstruction> Text;
    struct LiteralPatch {
      size_t Slot;
      unsigned Register;
      size_t Target;
    };
    std::vector<LiteralPatch> Literals;
    std::vector<size_t> Calls, CallXs;
    const auto Literal = [&](unsigned Register, uint64_t Value) {
      const auto Words = test::loadImm64(Register, Value);
      Text.insert(Text.end(), Words.begin(), Words.end());
    };
    const auto TargetLiteral = [&](unsigned Register, size_t Target) {
      Literals.push_back({Text.size(), Register, Target});
      Literal(Register, 0);
    };
    Literal(9, kInputStart);
    for (size_t I = 0; I < Count; ++I) {
      Text.push_back(encode(Opcode::MOV64_REG, 6, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 6, 0, 0, -64 * (I + 1)));
      for (size_t J = 0; J < Choices; ++J) {
        TargetLiteral(0, J);
        Text.push_back(encode(Opcode::ST_DW_REG, 6, 0, J * 8));
      }
      Text.push_back(encode(Opcode::MOV64_REG, 7, 6));
      Text.push_back(encode(Opcode::ADD64_IMM, 7, 0, 0, 48));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 7));
      Text.push_back(encode(Opcode::MOV64_REG, 2, 6));
      Text.push_back(encode(Opcode::LD_DW_REG, 3, 9, I * 16));
      TargetLiteral(4, Mode == 4 || Mode == 6 || Mode == 7 ? Choices : 1);
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 9, I * 16 + 8));
      Calls.push_back(Text.size());
      Text.push_back({});
      if (Stored)
        Text.push_back(encode(Opcode::LD_DW_REG, 0, 7));
      CallXs.push_back(Text.size());
      Text.push_back(encode(Opcode::CALL_REG, 0));
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Setter = Text.size();
    std::vector<size_t> Branches, Bodies, Jumps;
    for (size_t J = 0; J + 1 < Choices; ++J) {
      Branches.push_back(Text.size());
      Text.push_back({});
    }
    const size_t Default = Text.size();
    Text.push_back({});
    for (size_t J = 0; J < Choices; ++J) {
      Bodies.push_back(Text.size());
      if ((Mode == 4 || Mode == 7) && J == 1)
        Text.push_back(encode(Opcode::ST_DW_REG, 5, 4));
      if (Mode == 2 && J == 1)
        Text.push_back(encode(Opcode::MOV64_REG, 0, 4));
      else
        Text.push_back(encode(Opcode::LD_DW_REG, 0, Mode == 3 && J == 1 ? 5 : 2,
                              Mode == 3 && J == 1 ? 0 : J * 8));
      if (Mode == 6 && J == 1)
        Text.push_back(encode(Opcode::ST_DW_REG, 5, 4));
      if (Stored)
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 0));
      Jumps.push_back(Text.size());
      Text.push_back({});
    }
    const size_t Join = Text.size();
    // Exercise a unary transfer above a union, rather than only leaf unions.
    if (Mode == 8)
      Text.push_back(encode(Opcode::ADD64_IMM, 0, 0, 0, 0));
    Text.push_back(encode(Opcode::EXIT));
    for (size_t J = 0; J < Branches.size(); ++J)
      Text[Branches[J]] =
          encode(Opcode::JEQ64_IMM, 3, 0, Bodies[J] - Branches[J] - 1, J);
    Text[Default] = encode(Opcode::JA, 0, 0, Bodies.back() - Default - 1);
    for (size_t Jump : Jumps)
      Text[Jump] = encode(Opcode::JA, 0, 0, Join - Jump - 1);
    size_t Entry = Setter;
    for (unsigned Layer = 0; Layer < 2; ++Layer) {
      const size_t Wrapper = Text.size();
      Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, Entry - Wrapper - 1));
      Text.push_back(encode(Opcode::EXIT));
      Entry = Wrapper;
    }
    for (size_t Call : Calls)
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Entry - Call - 1);
    std::vector<size_t> Targets;
    for (size_t J = 0; J <= Choices; ++J) {
      Targets.push_back(Text.size());
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42 + J));
      Text.push_back(encode(Opcode::EXIT));
    }
    for (const auto &Patch : Literals) {
      const auto Words = test::loadImm64(
          Patch.Register, kBytecodeStart + Targets[Patch.Target] * 8);
      std::copy(Words.begin(), Words.end(), Text.begin() + Patch.Slot);
    }
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
      return Edge.CallerSlot == Calls.back() && Edge.GeneralizedInputs;
    }));
    if (Complete && !Stored) {
      EXPECT_GT(Report->InstantiatedReturnInputExpressions, 0u);
      EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [](const auto &Edge) {
        return Edge.InstantiatedReturnInputExpression;
      }));
      const auto JSON = callXContextReportJSON(*Program, *Report);
      EXPECT_EQ(JSON.getInteger("instantiated_return_input_expressions"),
                Report->InstantiatedReturnInputExpressions);
    }
    for (size_t I = 0; I < Count; ++I) {
      const auto Proof = llvm::find_if(Report->Contexts, [&](const auto &Site) {
        return !Site.EntryPrefixOnly && Site.CallXSlot == CallXs[I] &&
               Site.Result == CallXContextResult::ProvenTargetSet;
      });
      if (Complete) {
        ASSERT_NE(Proof, Report->Contexts.end()) << "call=" << I;
        std::vector<uint64_t> Values;
        for (const auto &Value : Proof->TargetValueAlternatives.values()) {
          ASSERT_EQ(Value.ValueKind, RegisterValue::Kind::Constant);
          Values.push_back(Value.Value);
        }
        std::sort(Values.begin(), Values.end());
        std::vector<uint64_t> Expected;
        for (size_t J = 0; J < Choices; ++J)
          Expected.push_back(kBytecodeStart + Targets[J] * 8);
        EXPECT_EQ(Values, Expected);
      } else {
        EXPECT_EQ(Proof, Report->Contexts.end());
        for (const auto &Site : Report->Contexts)
          if (!Site.EntryPrefixOnly && Site.CallXSlot == CallXs[I])
            EXPECT_FALSE(Site.TargetSlot);
      }
    }
    for (size_t Selector = 0; Selector < Choices; ++Selector) {
      std::vector<uint8_t> Input(Count * 16);
      const uint64_t FP =
          initialFramePointer(Program->Low.TheVersion, Program->Config);
      for (size_t I = 0; I < Count; ++I) {
        llvm::support::endian::write64le(Input.data() + I * 16, Selector);
        llvm::support::endian::write64le(Input.data() + I * 16 + 8,
                                         FP - 64 * (I + 1) + 8);
      }
      ExecutionEnvironment Environment;
      Environment.Memory.push_back(
          {kInputStart, Input, false, "branch inputs"});
      auto Raw = executeRaw(*Program, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      const size_t Expected =
          (Mode == 4 || Mode == 7) && Selector == 1 ? Choices : Selector;
      EXPECT_EQ(Raw->ReturnValue, 42 + Expected);
      for (size_t CallX : CallXs) {
        const auto At = llvm::find_if(
            Raw->Trace, [&](const auto &Step) { return Step.Slot == CallX; });
        ASSERT_NE(At, Raw->Trace.end());
        ASSERT_NE(std::next(At), Raw->Trace.end());
        EXPECT_EQ(std::next(At)->Slot, Targets[Expected]);
      }
    }
  }
}

TEST(SBFCallXContexts, LoadedFieldValuesCrossGeneralizedOutputWrappers) {
  for (unsigned Mode = 0; Mode < 7; ++Mode)
    for (unsigned Layers : {0u, 2u}) {
      SCOPED_TRACE(::testing::Message()
                   << "mode=" << Mode << ", layers=" << Layers);
      constexpr size_t Count = kCallXContextSpecializationsPerEntry + 2;
      std::vector<test::EncodedInstruction> Text;
      struct LiteralPatch {
        size_t Slot;
        unsigned Register;
        unsigned Target;
      };
      std::vector<LiteralPatch> Literals;
      std::vector<size_t> Calls, CallXs, Checks;
      const auto AddLiteral = [&](unsigned Register, uint64_t Value) {
        const auto Words = test::loadImm64(Register, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
      };
      const auto AddTarget = [&](unsigned Register, unsigned Target) {
        Literals.push_back({Text.size(), Register, Target});
        AddLiteral(Register, 0);
      };
      AddLiteral(9, kInputStart);
      for (size_t I = 0; I < Count; ++I) {
        if (Mode == 6) {
          AddLiteral(6, kHeapStart + 16 * I);
        } else {
          Text.push_back(encode(Opcode::MOV64_REG, 6, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 6, 0, 0, -16 * (I + 1)));
        }
        Text.push_back(encode(Opcode::MOV64_REG, 7, 6));
        Text.push_back(encode(Opcode::ADD64_IMM, 7, 0, 0, 8));
        if (Mode == 5)
          Text.push_back(encode(Opcode::MOV64_REG, 2, 10));
        else
          AddTarget(2, I % 2);
        Text.push_back(encode(Opcode::ST_DW_REG, 6, 2));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 7));
        Text.push_back(encode(Opcode::MOV64_REG, 2, 6));
        AddTarget(3, 1 - I % 2);
        Text.push_back(encode(Opcode::LD_DW_REG, 4, 9, I * sizeof(uint64_t)));
        Calls.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 7));
        CallXs.push_back(Text.size());
        Text.push_back(encode(Opcode::CALL_REG, 2));
        Text.push_back(encode(Opcode::LD_DW_REG, 4, 6));
        if (Mode == 5)
          Text.push_back(encode(Opcode::MOV64_REG, 5, 10));
        else
          AddTarget(5, Mode == 1 || Mode == 2 ? 1 - I % 2 : I % 2);
        Checks.push_back(Text.size());
        Text.push_back(encode(Opcode::JNE64_REG, 4, 5));
      }
      Text.push_back(encode(Opcode::EXIT));
      const size_t Failure = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 0xBAD));
      Text.push_back(encode(Opcode::EXIT));
      for (size_t Check : Checks)
        Text[Check] = encode(Opcode::JNE64_REG, 4, 5, Failure - Check - 1);

      const size_t Setter = Text.size();
      size_t RelayCall = 0;
      if (Mode == 4) {
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 2));
        RelayCall = Text.size();
        Text.push_back({});
      } else {
        if (Mode == 2)
          Text.push_back(encode(Opcode::ST_DW_REG, 4, 3));
        Text.push_back(encode(Opcode::LD_DW_REG, 7, 2));
        if (Mode == 1)
          Text.push_back(encode(Opcode::ST_DW_REG, 2, 3));
        if (Mode == 3) {
          Text.push_back(encode(Opcode::ST_DW_REG, 10, 7, -8));
          Text.push_back(encode(Opcode::MOV64_IMM, 7));
          Text.push_back(encode(Opcode::LD_DW_REG, 7, 10, -8));
        }
        if (Mode == 6) {
          Text.push_back(encode(Opcode::ST_DW_REG, 2, 7));
          Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0, 8));
          Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                                getSyscallInfo(Syscall::Memmove)->Hash));
        } else {
          Text.push_back(encode(Opcode::ST_DW_REG, 1, 7));
        }
      }
      Text.push_back(encode(Opcode::EXIT));
      if (Mode == 4) {
        const size_t AffineSetter = Text.size();
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 2));
        Text.push_back(encode(Opcode::EXIT));
        Text[RelayCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, AffineSetter - RelayCall - 1);
      }
      size_t Entry = Setter;
      for (unsigned Layer = 0; Layer < Layers; ++Layer) {
        const size_t Wrapper = Text.size();
        Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, Entry - Wrapper - 1));
        Text.push_back(encode(Opcode::EXIT));
        Entry = Wrapper;
      }
      for (size_t Call : Calls)
        Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Entry - Call - 1);
      std::array<size_t, 2> Targets;
      for (unsigned I = 0; I < Targets.size(); ++I) {
        Targets[I] = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42 + I));
        Text.push_back(encode(Opcode::EXIT));
      }
      for (const auto &Patch : Literals) {
        const auto Words = test::loadImm64(
            Patch.Register,
            kBytecodeStart + Targets[Patch.Target] * kInstructionSize);
        std::copy(Words.begin(), Words.end(), Text.begin() + Patch.Slot);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      for (size_t I = 0; I < Count; ++I) {
        const bool Proven =
            llvm::any_of(Report->Contexts, [&](const auto &Site) {
              return !Site.EntryPrefixOnly && Site.CallXSlot == CallXs[I] &&
                     Site.TargetSlot == Targets[I % 2];
            });
        EXPECT_EQ(Proven, Mode != 2 && Mode != 5) << "call=" << I;
        if (Mode == 2 || Mode == 5)
          for (const auto &Site : Report->Contexts)
            if (!Site.EntryPrefixOnly && Site.CallXSlot == CallXs[I])
              EXPECT_FALSE(Site.TargetSlot) << "call=" << I;
      }
      EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
        return Edge.CallerSlot == Calls.back() && Edge.GeneralizedInputs &&
               Edge.InputGeneralization ==
                   CallXInputGeneralization::SpecializationLimit;
      }));
      const auto JSON = callXContextReportJSON(*Program, *Report);
      if (Mode != 2)
        EXPECT_GT(JSON.getInteger("instantiated_stored_value_input_expressions")
                      .value_or(0),
                  0);

      std::vector<uint8_t> Input(Count * sizeof(uint64_t));
      const uint64_t FP =
          initialFramePointer(Program->Low.TheVersion, Program->Config);
      for (size_t I = 0; I < Count; ++I)
        llvm::support::endian::write64le(Input.data() + I * sizeof(uint64_t),
                                         FP - 16 * (I + 1));
      ExecutionEnvironment Environment;
      Environment.Memory.push_back(
          {kInputStart, Input, false, "write pointers"});
      if (Mode == 6) {
        Environment.Memory.push_back(
            {kHeapStart, std::vector<uint8_t>(Count * 16), true, "copy cells"});
        // The interpreter owns these same vector buffers after the move.
        // This host performs the actual full-word copy in its mapped heap.
        auto *Bytes = Environment.Memory.back().Bytes.data();
        Environment.HostSyscall = [Bytes](uint32_t Hash,
                                          const SyscallArguments &Args) {
          if (Hash != getSyscallInfo(Syscall::Memmove)->Hash || Args[2] != 8 ||
              Args[0] < kHeapStart || Args[0] - kHeapStart > Count * 16 - 8 ||
              Args[1] < kHeapStart || Args[1] - kHeapStart > Count * 16 - 8)
            return SyscallOutcome::unregistered();
          std::memmove(Bytes + (Args[0] - kHeapStart),
                       Bytes + (Args[1] - kHeapStart), 8);
          return SyscallOutcome::returned(0);
        };
      }
      InterpreterOptions Options;
      Options.MaxSteps = 4096;
      auto Raw = executeRaw(*Program, std::move(Environment), Options);
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      if (Mode == 5) {
        EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted) << Raw->Error;
        EXPECT_TRUE(llvm::any_of(Raw->Trace, [&](const auto &Step) {
          return Step.Slot == CallXs.front();
        }));
        continue;
      }
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, Mode == 2 ? 42u : 43u);
      for (size_t I = 0; I < Count; ++I) {
        auto At = llvm::find_if(Raw->Trace, [&](const auto &Step) {
          return Step.Slot == CallXs[I];
        });
        ASSERT_NE(At, Raw->Trace.end());
        ASSERT_NE(std::next(At), Raw->Trace.end());
        EXPECT_EQ(std::next(At)->Slot, Targets[Mode == 2 ? 1 - I % 2 : I % 2]);
      }
    }
}

TEST(SBFCallXContexts, BranchLocalCapturedFieldsSurviveOnlyTheirOwnPaths) {
  for (unsigned Mode = 0; Mode < 10; ++Mode)
    for (bool ReverseEdges : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "mode=" << Mode << ", reverse=" << ReverseEdges);
      const unsigned Choices = Mode == 6 || Mode == 9 ? 5 : Mode == 8 ? 4 : 2;
      const bool Shared = Mode == 3 || Mode == 7;
      std::vector<test::EncodedInstruction> Text;
      std::vector<size_t> AllocateCalls, NullChecks, TargetLiterals, Jumps;
      const auto Literal = [&](unsigned Reg, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(Reg, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      const auto Acquire = [&](unsigned InputOffset, unsigned Result) {
        Literal(1, kInputStart + InputOffset);
        AllocateCalls.push_back(Text.size());
        Text.push_back({});
        NullChecks.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::MOV64_REG, Result, 0));
      };
      Literal(1, kInputStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 6, 1));
      if (Shared)
        Acquire(8, 7);
      std::optional<size_t> InitialTarget;
      if (Mode == 7) {
        InitialTarget = Literal(2, 0);
        Text.push_back(encode(Opcode::ST_DW_REG, 7, 2, 8));
      }
      const size_t Dispatch = Text.size();
      Text.resize(Text.size() + Choices);
      std::optional<size_t> OpaqueCall;
      for (unsigned Choice = 0; Choice < Choices; ++Choice) {
        const size_t Start = Text.size();
        const size_t Branch = Dispatch + Choice;
        Text[Branch] =
            Choice + 1 == Choices
                ? encode(Opcode::JA, 0, 0, Start - Branch - 1)
                : encode(Opcode::JEQ64_IMM, 6, 0, Start - Branch - 1, Choice);
        if (!Shared)
          Acquire(8, 7);
        if (Mode == 7) {
          if (Choice) {
            OpaqueCall = Text.size();
            Text.push_back({});
          }
        } else if (!(Mode == 4 && Choice)) {
          if (Mode == 3 && Choice) {
            Literal(1, kInputStart + 24);
            Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
          } else {
            TargetLiterals.push_back(Literal(2, 0));
          }
          Text.push_back(encode(Opcode::ST_DW_REG, 7, 2, 8));
        }
        Jumps.push_back(Text.size());
        Text.push_back({});
      }
      const size_t Join = Text.size();
      if (Mode == 5)
        Acquire(16, 8);
      if (Mode == 2 || Mode == 5) {
        Literal(1, kInputStart + 24);
        Text.push_back(encode(Opcode::LD_DW_REG, 3, 1));
        Text.push_back(encode(Opcode::ST_DW_REG, Mode == 5 ? 8 : 7, 3, 8));
      }
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 7, 8));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 2));
      const size_t Done = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t Allocate = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
      Literal(2, kHeapStart + 8);
      Text.push_back(encode(Opcode::JGE64_REG, 0, 2, 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Opaque = Text.size();
      Literal(1, kInputStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 1, 24));
      Text.push_back(encode(Opcode::LD_DW_REG, 1, 1, 8));
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 2, 8));
      Text.push_back(encode(Opcode::EXIT));
      const size_t A = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t B = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      std::vector<size_t> Targets{A, B};
      if (Mode == 9)
        for (unsigned I = 2; I != Choices; ++I) {
          Targets.push_back(Text.size());
          Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42 + I));
          Text.push_back(encode(Opcode::EXIT));
        }
      for (size_t Slot : AllocateCalls)
        Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Allocate - Slot - 1);
      for (size_t Slot : NullChecks)
        Text[Slot] = encode(Opcode::JEQ64_IMM, 0, 0, Done - Slot - 1);
      for (size_t Slot : Jumps)
        Text[Slot] = encode(Opcode::JA, 0, 0, Join - Slot - 1);
      if (OpaqueCall)
        Text[*OpaqueCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Opaque - *OpaqueCall - 1);
      if (InitialTarget)
        TargetLiterals.push_back(*InitialTarget);
      for (size_t I = 0; I < TargetLiterals.size(); ++I) {
        const size_t Target = Mode == 9 ? Targets[I] : Mode == 1 && I ? B : A;
        const auto Words = test::loadImm64(2, kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + TargetLiterals[I]);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (ReverseEdges)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      bool Singleton = false, Finite = false;
      for (const auto &Site : Report->Contexts)
        if (Site.CallXSlot == CallX) {
          Singleton |= Site.TargetSlot == A;
          Finite |= Site.Result == CallXContextResult::ProvenTargetSet;
          if (Mode != 0 && Mode != 1 && Mode != 6 && Mode != 8)
            EXPECT_FALSE(Site.TargetAddress ||
                         Site.Result == CallXContextResult::ProvenTargetSet);
        }
      // Five branch-local pointers can share one field value after identity
      // joins. Five distinct targets must still exceed the value-set limit.
      EXPECT_EQ(Singleton, Mode == 0 || Mode == 6 || Mode == 8);
      EXPECT_EQ(Finite, Mode == 1);
      for (unsigned Choice = 0; Choice < Choices; ++Choice)
        for (unsigned Alias = 0; Alias < (Mode == 5 ? 2u : 1u); ++Alias) {
          SCOPED_TRACE(::testing::Message()
                       << "choice=" << Choice << ", alias=" << Alias);
          std::vector<uint8_t> Input(32), Heap(128);
          llvm::support::endian::write64le(Input.data(), Choice);
          llvm::support::endian::write64le(Input.data() + 8, kHeapStart + 16);
          llvm::support::endian::write64le(Input.data() + 16,
                                           kHeapStart + (Alias ? 16 : 64));
          llvm::support::endian::write64le(Input.data() + 24,
                                           kBytecodeStart + B * 8);
          llvm::support::endian::write64le(Heap.data() + 24,
                                           kBytecodeStart + B * 8);
          ExecutionEnvironment Environment;
          Environment.Memory.push_back({kInputStart, Input, false, "input"});
          Environment.Memory.push_back({kHeapStart, Heap, true, "objects"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          const bool Replaced =
              Mode == 2 || (Mode == 5 && Alias) ||
              (Choice && (Mode == 1 || Mode == 3 || Mode == 4 || Mode == 7));
          EXPECT_EQ(Raw->ReturnValue, Mode == 9  ? 42u + Choice
                                      : Replaced ? 43u
                                                 : 42u);
          EXPECT_TRUE(llvm::any_of(Raw->Trace, [&](const auto &Step) {
            return Step.Slot == CallX;
          }));
        }
    }
}

TEST(SBFCallXContexts, ConditionalCapturedFieldsCrossFunctionReturns) {
  for (bool SecondCall : {false, true})
    for (bool ReverseEdges : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "second=" << SecondCall << ", reverse=" << ReverseEdges);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned Reg, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(Reg, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      Literal(1, kInputStart + 8);
      const size_t FirstLiteral = Literal(2, 0);
      const size_t FirstCall = Text.size();
      Text.push_back({});
      const size_t FirstNull = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
      std::optional<size_t> SecondLiteral, AnotherCall;
      if (SecondCall) {
        Literal(1, kInputStart + 16);
        SecondLiteral = Literal(2, 0);
        AnotherCall = Text.size();
        Text.push_back({});
      }
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 6, 8));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 2));
      const size_t Done = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t Factory = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 8, 1));
      Text.push_back(encode(Opcode::MOV64_REG, 9, 2));
      Literal(1, kInputStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 6, 1));
      const size_t Branch = Text.size();
      Text.push_back({});
      std::vector<size_t> Allocations, NullChecks, Jumps;
      size_t Right = 0;
      for (unsigned Choice = 0; Choice < 2; ++Choice) {
        if (Choice)
          Right = Text.size();
        Text.push_back(encode(Opcode::MOV64_REG, 1, 8));
        Allocations.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::MOV64_REG, 4, 0));
        Text.push_back(encode(Opcode::MOV64_REG, 5, 4));
        Text.push_back(encode(Opcode::MOV64_IMM, 4));
        NullChecks.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::ST_DW_REG, 0, 9, 8));
        Jumps.push_back(Text.size());
        Text.push_back({});
      }
      const size_t Return = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t Allocate = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
      Literal(2, kHeapStart + 8);
      Text.push_back(encode(Opcode::JGE64_REG, 0, 2, 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t A = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t B = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text[FirstCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Factory - FirstCall - 1);
      Text[FirstNull] = encode(Opcode::JEQ64_IMM, 0, 0, Done - FirstNull - 1);
      if (AnotherCall)
        Text[*AnotherCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Factory - *AnotherCall - 1);
      Text[Branch] = encode(Opcode::JNE64_IMM, 6, 0, Right - Branch - 1);
      for (size_t Slot : Allocations)
        Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Allocate - Slot - 1);
      for (size_t Slot : NullChecks)
        Text[Slot] = encode(Opcode::JEQ64_IMM, 5, 0, Return - Slot - 1);
      for (size_t Slot : Jumps)
        Text[Slot] = encode(Opcode::JA, 0, 0, Return - Slot - 1);
      Text.push_back(encode(Opcode::EXIT));
      for (auto [Slot, Target] :
           {std::pair{FirstLiteral, A},
            std::pair{SecondLiteral.value_or(FirstLiteral),
                      SecondLiteral ? B : A}}) {
        const auto Words = test::loadImm64(2, kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (ReverseEdges)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      EXPECT_EQ(llvm::any_of(Report->Contexts,
                             [&](const auto &Site) {
                               return Site.CallXSlot == CallX &&
                                      Site.TargetSlot == A;
                             }),
                !SecondCall);
      if (SecondCall)
        for (const auto &Site : Report->Contexts)
          if (Site.CallXSlot == CallX)
            EXPECT_FALSE(Site.TargetAddress ||
                         Site.Result == CallXContextResult::ProvenTargetSet);
      for (unsigned Choice = 0; Choice < 2; ++Choice)
        for (unsigned Trial = 0; Trial < (SecondCall ? 4u : 2u); ++Trial) {
          const bool FirstNull = Trial == (SecondCall ? 2u : 1u);
          const bool SecondNull = SecondCall && Trial == 3;
          const bool Alias = SecondCall && Trial == 1;
          SCOPED_TRACE(::testing::Message()
                       << "choice=" << Choice << ", trial=" << Trial);
          std::vector<uint8_t> Input(24);
          llvm::support::endian::write64le(Input.data(), Choice);
          llvm::support::endian::write64le(Input.data() + 8,
                                           FirstNull ? 0 : kHeapStart + 16);
          llvm::support::endian::write64le(
              Input.data() + 16,
              SecondNull ? 0 : kHeapStart + (Alias ? 16 : 64));
          ExecutionEnvironment Environment;
          Environment.Memory.push_back({kInputStart, Input, false, "pointers"});
          Environment.Memory.push_back(
              {kHeapStart, std::vector<uint8_t>(128), true, "objects"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, FirstNull ? 0u : Alias ? 43u : 42u);
          EXPECT_EQ(llvm::count_if(
                        Raw->Trace,
                        [&](const auto &Step) { return Step.Slot == CallX; }),
                    FirstNull ? 0 : 1);
        }
    }
}

TEST(SBFCallXContexts, NonzeroReturnsRestoreOnlyTheirOwnStoredFields) {
  for (unsigned Mode = 0; Mode < 7; ++Mode)
    for (bool ReverseEdges : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "mode=" << Mode << ", reverse=" << ReverseEdges);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      const size_t Call = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
      unsigned Pointer = 6;
      if (Mode == 2) {
        Text.push_back(encode(Opcode::MOV64_REG, 7, 6));
        Text.push_back(encode(Opcode::MOV64_IMM, 6));
        Text.push_back(encode(Opcode::MOV64_IMM, 0));
        Pointer = 7;
      }
      if (Mode == 3 || Mode == 4) {
        Literal(1, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 1, 8));
        if (Mode == 3)
          Literal(1, kHeapStart);
        else
          Text.push_back(encode(Opcode::LD_DW_REG, 1, 1, 16));
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 2));
      }
      if (Mode == 6) {
        Literal(1, kHeapStart);
        Text.push_back(encode(Opcode::ST_B_IMM, 1, 0, 0, 96));
      }
      const size_t Null = Text.size();
      Text.push_back({});
      const size_t Target = Literal(2, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, Pointer, 2, 8));
      Literal(1, kHeapStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 2, 8));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 2));
      const size_t Done = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t Forwarder = Text.size();
      const size_t ForwardCall = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
      const size_t Producer = Text.size();
      Literal(3, kInputStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 1, 3));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Literal(2, kHeapStart + 8);
      const size_t Invalid = Text.size();
      Text.push_back({});
      std::optional<size_t> OtherCall;
      std::optional<size_t> OtherNull;
      if (Mode == 5) {
        // A separately acquired value can have identical range bounds. Its
        // store must not be identified with the first value merely for that.
        Text.push_back(encode(Opcode::ST_DW_REG, 10, 1, -16));
        Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
        OtherCall = Text.size();
        Text.push_back({});
        OtherNull = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::MOV64_REG, 3, 0));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 6));
      }
      Literal(2, kHeapStart);
      Text.push_back(encode(Opcode::ST_DW_REG, 2, Mode == 5 ? 3 : 1));
      Text.push_back(encode(Opcode::MOV64_REG, 0, 1));
      const size_t Return = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t Other = Text.size();
      Literal(1, kInputStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 1, 8));
      Literal(2, kHeapStart + 8);
      const size_t OtherValid = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      const size_t OtherReturn = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t A = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t B = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0,
                          (Mode == 1 ? Forwarder : Producer) - Call - 1);
      Text[ForwardCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Producer - ForwardCall - 1);
      Text[Null] = encode(Opcode::JEQ64_IMM, Pointer, 0, Done - Null - 1);
      Text[Invalid] = encode(Opcode::JLT64_REG, 1, 2, Return - Invalid - 1);
      if (OtherCall)
        Text[*OtherCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Other - *OtherCall - 1);
      if (OtherNull)
        Text[*OtherNull] =
            encode(Opcode::JEQ64_IMM, 0, 0, Return - *OtherNull - 1);
      Text[OtherValid] =
          encode(Opcode::JGE64_REG, 0, 2, OtherReturn - OtherValid - 1);
      const auto Words = test::loadImm64(2, kBytecodeStart + A * 8);
      std::copy(Words.begin(), Words.end(), Text.begin() + Target);
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (ReverseEdges)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      bool Proven = false;
      for (const auto &Site : Report->Contexts)
        if (Site.CallXSlot == CallX) {
          Proven |= Site.TargetSlot == A;
          if (Mode >= 3)
            EXPECT_FALSE(Site.TargetAddress ||
                         Site.Result == CallXContextResult::ProvenTargetSet);
        }
      EXPECT_EQ(Proven, Mode < 3);
      for (unsigned Trial = 0; Trial < (Mode == 5 ? 4u : 3u); ++Trial) {
        SCOPED_TRACE(Trial);
        const bool IsNull = Trial == 0 || Trial == 3;
        const bool Alias = Trial == 1;
        std::vector<uint8_t> Input(24), Heap(256);
        llvm::support::endian::write64le(Input.data(),
                                         Trial == 0 ? 0 : kHeapStart + 32);
        llvm::support::endian::write64le(
            Input.data() + 8,
            Trial == 3 ? 0 : kHeapStart + ((Mode == 5 && Alias) ? 32 : 96));
        llvm::support::endian::write64le(Input.data() + 16,
                                         kHeapStart + (Alias ? 0 : 160));
        for (unsigned Offset : {40u, 104u})
          llvm::support::endian::write64le(Heap.data() + Offset,
                                           kBytecodeStart + B * 8);
        ExecutionEnvironment Environment;
        Environment.Memory.push_back({kInputStart, Input, false, "input"});
        Environment.Memory.push_back({kHeapStart, Heap, true, "objects"});
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        const bool CallsB = Mode == 3 || (Mode == 4 && Alias) ||
                            (Mode == 5 && !Alias) || Mode == 6;
        EXPECT_EQ(Raw->ReturnValue, IsNull ? 0u : CallsB ? 43u : 42u);
        EXPECT_EQ(llvm::count_if(
                      Raw->Trace,
                      [&](const auto &Step) { return Step.Slot == CallX; }),
                  IsNull ? 0 : 1);
      }
    }
}

TEST(SBFCallXContexts, StoredReturnIdentityFollowsTheMergedValue) {
  for (bool LiteralFallback : {false, true})
    for (bool ReverseEdges : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "literal=" << LiteralFallback
                                        << ", reverse=" << ReverseEdges);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      const size_t Call = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
      const size_t Null = Text.size();
      Text.push_back({});
      const size_t TargetLiteral = Literal(2, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
      Literal(1, kHeapStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 2, 8));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 2));
      const size_t Done = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t Producer = Text.size();
      Literal(3, kInputStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 1, 3));
      Text.push_back(encode(Opcode::LD_DW_REG, 4, 3, 8));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      const size_t Select = Text.size();
      Text.push_back({});
      Literal(2, kHeapStart + 8);
      const size_t LeftInvalid = Text.size();
      Text.push_back({});
      const size_t LeftValid = Text.size();
      Text.push_back({});
      const size_t Right = Text.size();
      std::optional<size_t> RightValid, RightInvalid;
      if (LiteralFallback) {
        Literal(1, kHeapStart + 128);
      } else {
        Literal(2, kHeapStart + 8);
        RightValid = Text.size();
        Text.push_back({});
        RightInvalid = Text.size();
        Text.push_back({});
      }
      const size_t Store = Text.size();
      Literal(2, kHeapStart);
      Text.push_back(encode(Opcode::ST_DW_REG, 2, 1));
      Text.push_back(encode(Opcode::MOV64_REG, 0, 1));
      const size_t Return = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t Target = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Producer - Call - 1);
      Text[Null] = encode(Opcode::JEQ64_IMM, 6, 0, Done - Null - 1);
      Text[Select] = encode(Opcode::JNE64_IMM, 4, 0, Right - Select - 1);
      Text[LeftInvalid] =
          encode(Opcode::JLT64_REG, 1, 2, Return - LeftInvalid - 1);
      Text[LeftValid] = encode(Opcode::JA, 0, 0, Store - LeftValid - 1);
      if (RightValid) {
        Text[*RightValid] =
            encode(Opcode::JGE64_REG, 1, 2, Store - *RightValid - 1);
        Text[*RightInvalid] =
            encode(Opcode::JA, 0, 0, Return - *RightInvalid - 1);
      }
      const auto Words = test::loadImm64(2, kBytecodeStart + Target * 8);
      std::copy(Words.begin(), Words.end(), Text.begin() + TargetLiteral);
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (ReverseEdges)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
        return Site.CallXSlot == CallX && Site.TargetSlot == Target;
      }));
      for (uint64_t Cursor : {uint64_t{0}, uint64_t{7}, kHeapStart + 256})
        for (uint64_t Choice : {uint64_t{0}, uint64_t{1}}) {
          SCOPED_TRACE(::testing::Message()
                       << "cursor=" << Cursor << ", choice=" << Choice);
          std::vector<uint8_t> Input(16);
          llvm::support::endian::write64le(Input.data(), Cursor);
          llvm::support::endian::write64le(Input.data() + 8, Choice);
          ExecutionEnvironment Environment;
          Environment.Memory.push_back({kInputStart, Input, false, "input"});
          Environment.Memory.push_back(
              {kHeapStart, std::vector<uint8_t>(512), true, "heap"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          const bool Success =
              Cursor >= kHeapStart + 8 || (LiteralFallback && Choice);
          EXPECT_EQ(Raw->ReturnValue, Success ? 42u : 0u);
          EXPECT_EQ(llvm::count_if(
                        Raw->Trace,
                        [&](const auto &Step) { return Step.Slot == CallX; }),
                    Success ? 1 : 0);
        }
    }
}

TEST(SBFCallXContexts, CapturedReturnsSurviveObjectFieldsAndCompleteCopies) {
  for (unsigned Mode = 0; Mode < 11; ++Mode) {
    SCOPED_TRACE(Mode);
    std::vector<test::EncodedInstruction> Text;
    const auto Literal = [&](unsigned R, uint64_t V) {
      const auto Instructions = test::loadImm64(R, V);
      const size_t Slot = Text.size();
      Text.insert(Text.end(), Instructions.begin(), Instructions.end());
      return Slot;
    };
    Literal(1, kInputStart);
    const size_t AllocateCall = Text.size();
    Text.push_back({});
    const size_t NullCheck = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
    const size_t TargetLiteral = Literal(2, 0);
    Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
    Text.push_back(encode(Opcode::ST_DW_REG, 10, Mode == 6 ? 2 : 6, -24));
    std::vector<size_t> WarmCopies;
    std::optional<size_t> ReinitializeTarget;
    if (Mode == 4 || Mode == 6 || Mode >= 7)
      for (unsigned I = 0; I < kCallXContextSpecializationsPerEntry; ++I) {
        Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -256 - I * 32));
        Text.push_back(encode(Opcode::MOV64_REG, 2, 10));
        Text.push_back(
            encode(Opcode::ADD64_IMM, 2, 0, 0, Mode >= 7 ? -128 : -24));
        Text.push_back(
            encode(Opcode::MOV64_IMM, 3, 0, 0, Mode >= 7 ? 8 : 8 + I * 8));
        WarmCopies.push_back(Text.size());
        Text.push_back({});
      }
    if (Mode >= 7) {
      // Keep the real copy's input known even when a warm call had unknown
      // effects. In particular, the unknown-selector control must reach the
      // path-instantiation boundary with a captured source field.
      ReinitializeTarget = Literal(2, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 6, -24));
    }
    Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
    Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
    Text.push_back(encode(Opcode::MOV64_REG, 2, 10));
    Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, -24));
    Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0,
                          Mode >= 7   ? 16
                          : Mode == 1 ? 7
                                      : 8));
    const size_t CopyCall = Text.size();
    Text.push_back({});
    if (Mode == 2)
      Text.push_back(encode(Opcode::ST_B_IMM, 6, 0, 9));
    if (Mode == 3 || Mode == 9) {
      Literal(1, kInputStart + 8);
      const size_t OtherCall = Text.size();
      Text.push_back(encode(Opcode::CALL_IMM, 0, 1));
      // This capture can equal the first one. Different identities prove no
      // freshness, and a write through it must invalidate the object field.
      Text.push_back(encode(Opcode::ST_DW_IMM, 0, 0, 8));
      (void)OtherCall;
    }
    if (Mode == 6) {
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -16));
    } else {
      Text.push_back(encode(Opcode::LD_DW_REG, 1, 10, -16));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 1, 8));
    }
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 2));
    const size_t Done = Text.size();
    Text.push_back(encode(Opcode::EXIT));
    const size_t Allocate = Text.size();
    Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
    Literal(2, kHeapStart + 8);
    const size_t Below = Text.size();
    Text.push_back({});
    Literal(2, kHeapStart + 128);
    const size_t Above = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::EXIT));
    const size_t Null = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Caller = Text.size();
    size_t ClobberCall = 0, InnerCopyCall = 0;
    if (Mode == 6) {
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
      Text.push_back(encode(Opcode::MOV64_REG, 7, 2));
      Text.push_back(encode(Opcode::MOV64_REG, 8, 3));
      Literal(1, kInputStart + 8);
      ClobberCall = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 1, 6));
      Text.push_back(encode(Opcode::MOV64_REG, 2, 7));
      Text.push_back(encode(Opcode::MOV64_REG, 3, 8));
      InnerCopyCall = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
    }
    const size_t Clobber = Text.size();
    if (Mode == 6) {
      Text.push_back(encode(Opcode::LD_DW_REG, 3, 1));
      Text.push_back(encode(Opcode::LD_DW_REG, 4, 1, 8));
      Text.push_back(encode(Opcode::ST_DW_REG, 3, 4));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
    }
    const size_t Copy = Text.size();
    if (Mode == 6) {
      Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0, 8));
      Text.push_back(encode(Opcode::LD_DW_REG, 4, 2));
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 4));
    }
    if (Mode >= 7) {
      // Fill the specialization cache before the real copy. A short copy
      // follows a concrete word loop; its large-count sibling is a syscall.
      // The extra unknown selector must prevent path instantiation in mode 8.
      size_t UnknownBranch = 0;
      if (Mode == 8) {
        Literal(4, kInputStart + 8);
        Text.push_back(encode(Opcode::LD_DW_REG, 4, 4));
        UnknownBranch = Text.size();
        Text.push_back({});
      }
      const size_t LargeCopy = Text.size();
      Text.push_back({});
      size_t SpinBranch = 0;
      if (Mode == 10) {
        SpinBranch = Text.size();
        Text.push_back({});
      }
      Text.push_back(encode(Opcode::MOV64_IMM, 4));
      const size_t Loop = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 2));
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 5));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, 8));
      Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, 8));
      Text.push_back(encode(Opcode::ADD64_IMM, 4, 0, 0, 8));
      const size_t Back = Text.size();
      Text.push_back(encode(Opcode::JLT64_REG, 4, 3, Loop - Back - 1));
      const size_t Exit = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t Syscall = Text.size();
      Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                            getSyscallInfo(Syscall::Memmove)->Hash));
      if (Mode == 10) {
        Text.push_back(encode(Opcode::EXIT));
        const size_t Spin = Text.size();
        Text.push_back(encode(Opcode::LD_DW_REG, 4, 2));
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 4));
        Text.push_back(encode(Opcode::JA, 0, 0, -3));
        Text[SpinBranch] =
            encode(Opcode::JEQ64_IMM, 3, 0, Spin - SpinBranch - 1, 16);
      }
      Text[LargeCopy] =
          encode(Opcode::JGT64_IMM, 3, 0, Syscall - LargeCopy - 1, 64);
      if (Mode == 8)
        Text[UnknownBranch] =
            encode(Opcode::JNE64_IMM, 4, 0, Exit - UnknownBranch - 1);
    } else if (Mode == 5) {
      Text.push_back(encode(Opcode::LD_DW_REG, 4, 2));
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 4));
    } else {
      Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                            getSyscallInfo(Syscall::Memmove)->Hash));
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Target = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    const size_t ReplacementTarget = Text.size();
    if (Mode == 6) {
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
    }
    for (size_t Slot = 0; Slot < Allocate; ++Slot)
      if (Text[Slot][0] == getOpcodeInfo(Opcode::CALL_IMM)->Encoding &&
          (Text[Slot][1] >> 4) == 1)
        Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Allocate - Slot - 1);
    Text[AllocateCall] =
        encode(Opcode::CALL_IMM, 0, 1, 0, Allocate - AllocateCall - 1);
    Text[CopyCall] = encode(Opcode::CALL_IMM, 0, 1, 0,
                            (Mode == 6 ? Caller : Copy) - CopyCall - 1);
    if (Mode == 6) {
      Text[ClobberCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Clobber - ClobberCall - 1);
      Text[InnerCopyCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Copy - InnerCopyCall - 1);
    }
    for (size_t Slot : WarmCopies)
      Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Copy - Slot - 1);
    Text[NullCheck] = encode(Opcode::JEQ64_IMM, 0, 0, Done - NullCheck - 1);
    Text[Below] = encode(Opcode::JLT64_REG, 0, 2, Null - Below - 1);
    Text[Above] = encode(Opcode::JGT64_REG, 0, 2, Null - Above - 1);
    const auto TargetAddress = test::loadImm64(2, kBytecodeStart + Target * 8);
    std::copy(TargetAddress.begin(), TargetAddress.end(),
              Text.begin() + TargetLiteral);
    if (ReinitializeTarget)
      std::copy(TargetAddress.begin(), TargetAddress.end(),
                Text.begin() + *ReinitializeTarget);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    bool Proven = false;
    for (const auto &Site : Report->Contexts)
      Proven |= Site.CallXSlot == CallX && Site.TargetSlot == Target;
    EXPECT_EQ(Proven, Mode == 0 || Mode == 4 || Mode == 5 || Mode == 7);
    if (Mode == 4)
      EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
        return Edge.CallerSlot == CopyCall && Edge.InstantiatedCopySlot;
      }));
    if (Mode == 7)
      EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
        return Edge.CallerSlot == CopyCall &&
               Edge.InstantiatedMemoryPathStores == 2 &&
               Edge.InstantiatedMemoryPathInstructions > 1 &&
               !Edge.InstantiatedCopySlot;
      }));
    if (Mode == 8 || Mode == 10)
      EXPECT_FALSE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
        return Edge.CallerSlot == CopyCall &&
               Edge.InstantiatedMemoryPathStores != 0;
      }));
    if (Mode >= 7) {
      EXPECT_LE(llvm::count_if(Report->Summaries,
                               [&](const auto &Summary) {
                                 return Summary.EntrySlot == Copy &&
                                        !Summary.GeneralizedInputs;
                               }),
                kCallXContextSpecializationsPerEntry);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
    }
    EXPECT_FALSE(Report->CapturedAddresses.empty());
    if (Mode == 6) {
      // The unknown write changes the source after the caller's entry. A
      // later generic copy cannot restore its old entry value through another
      // specialized summary. Raw execution reaches the replacement target.
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input(24);
      llvm::support::endian::write64le(Input.data(), kHeapStart + 16);
      llvm::support::endian::write64le(
          Input.data() + 8,
          initialFramePointer(Program->Low.TheVersion, Program->Config) - 24);
      llvm::support::endian::write64le(Input.data() + 16,
                                       kBytecodeStart + ReplacementTarget * 8);
      Environment.Memory.push_back({kInputStart, Input, false, "input"});
      Environment.Memory.push_back(
          {kHeapStart, std::vector<uint8_t>(256), true, "heap"});
      // Every invocation has already copied its complete eight-byte window
      // with guest instructions. Returning without another write is the same
      // memory result for these particular memmove invocations.
      Environment.HostSyscall = [](uint32_t Hash,
                                   const SyscallArguments &Args) {
        return Hash == getSyscallInfo(Syscall::Memmove)->Hash && Args[2] == 8
                   ? SyscallOutcome::returned(0)
                   : SyscallOutcome::unregistered();
      };
      auto Raw = executeRaw(*Program, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 43u);
      EXPECT_TRUE(llvm::any_of(
          Raw->Trace, [&](const auto &Step) { return Step.Slot == CallX; }));
    }
    if (Mode == 5 || Mode >= 7)
      for (uint64_t Pointer : {uint64_t{0}, kHeapStart + 16}) {
        ExecutionEnvironment Environment;
        std::vector<uint8_t> Input(16);
        llvm::support::endian::write64le(Input.data(), Pointer);
        if (Mode == 9)
          llvm::support::endian::write64le(Input.data() + 8, Pointer);
        Environment.Memory.push_back({kInputStart, Input, false, "pointer"});
        Environment.Memory.push_back(
            {kHeapStart, std::vector<uint8_t>(256), true, "heap"});
        InterpreterOptions Options;
        Options.MaxSteps = 4096;
        auto Raw = executeRaw(*Program, std::move(Environment), Options);
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        if (Pointer != 0 && (Mode == 9 || Mode == 10)) {
          EXPECT_EQ(Raw->Status, Mode == 9 ? ExecutionStatus::Faulted
                                           : ExecutionStatus::StepLimit)
              << Raw->Error;
          EXPECT_EQ(llvm::any_of(
                        Raw->Trace,
                        [&](const auto &Step) { return Step.Slot == CallX; }),
                    Mode == 9);
          continue;
        }
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, Pointer == 0 ? 0u : 42u);
        EXPECT_EQ(
            llvm::any_of(Raw->Trace,
                         [&](const auto &Step) { return Step.Slot == CallX; }),
            Pointer != 0);
      }
  }
}

TEST(SBFCallXContexts,
     KnownStackCopiesPreserveUnrelatedSpillsAfterGeneralization) {
  for (unsigned Mode : {0u, 1u, 2u}) {
    SCOPED_TRACE(Mode);
    std::vector<test::EncodedInstruction> Text;
    const auto Literal = [&](unsigned R, uint64_t Value) {
      const auto Slots = test::loadImm64(R, Value);
      Text.insert(Text.end(), Slots.begin(), Slots.end());
    };
    Literal(6, 0);
    for (int Offset : {-128, -120, -56, -48})
      Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, Offset, 0));
    std::vector<size_t> Calls;
    for (size_t I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 6, -8));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -128));
      Text.push_back(encode(Opcode::MOV64_REG, 2, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, -56));
      if (I == kCallXContextSpecializationsPerEntry && Mode) {
        const unsigned R = Mode == 1 ? 1 : 2;
        Literal(R, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, R, R));
      }
      Text.push_back(
          encode(Opcode::MOV64_IMM, 3, 0, 0,
                 I == kCallXContextSpecializationsPerEntry ? 16 : I + 1));
      Calls.push_back(Text.size());
      Text.push_back({});
    }
    Text.push_back(encode(Opcode::LD_DW_REG, 0, 10, -8));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Copy = Text.size();
    const size_t Branch = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::LD_DW_REG, 4, 2));
    Text.push_back(encode(Opcode::ST_DW_REG, 1, 4));
    Text.push_back(encode(Opcode::LD_DW_REG, 4, 2, 8));
    Text.push_back(encode(Opcode::ST_DW_REG, 1, 4, 8));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Syscall = Text.size();
    Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                          getSyscallInfo(Syscall::Memmove)->Hash));
    Text.push_back(encode(Opcode::EXIT));
    Text[Branch] = encode(Opcode::JNE64_IMM, 3, 0, Syscall - Branch - 1, 16);
    const size_t Target = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    const auto TargetAddress = test::loadImm64(6, kBytecodeStart + Target * 8);
    std::copy(TargetAddress.begin(), TargetAddress.end(), Text.begin());
    for (size_t Slot : Calls)
      Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Copy - Slot - 1);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    EXPECT_EQ(llvm::any_of(Report->Contexts,
                           [&](const auto &Site) {
                             return Site.CallXSlot == CallX &&
                                    Site.TargetSlot == Target;
                           }),
              Mode == 0);
    EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
      return Edge.CallerSlot == Calls.back() && Edge.GeneralizedInputs &&
             Edge.InputGeneralization ==
                 CallXInputGeneralization::SpecializationLimit &&
             Edge.InstantiatedMemoryPathStores == (Mode == 0 ? 2 : 0);
    }));
    checkSummaryReferences(*Report);
    ExecutionEnvironment Environment;
    std::vector<uint8_t> Input(8);
    const auto Frame =
        initialFramePointer(Program->Low.TheVersion, Program->Config);
    llvm::support::endian::write64le(Input.data(),
                                     Frame - (Mode == 1 ? 16 : 56));
    Environment.Memory.push_back({kInputStart, Input, false, "copy address"});
    // The four warm-up calls copy explicitly zeroed bytes onto zeroed bytes.
    // The final 16-byte transfer uses guest instructions, including its alias
    // control, so this host callback does not supply the property under test.
    Environment.HostSyscall = [Frame](uint32_t Hash,
                                      const SyscallArguments &Args) {
      return Hash == getSyscallInfo(Syscall::Memmove)->Hash &&
                     Args[0] == Frame - 128 && Args[1] == Frame - 56 &&
                     Args[2] >= 1 && Args[2] <= 4
                 ? SyscallOutcome::returned(0)
                 : SyscallOutcome::unregistered();
    };
    auto Raw = executeRaw(*Program, std::move(Environment));
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    if (Mode == 1) {
      EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted);
      EXPECT_EQ(Raw->Fault, FaultCode::UnknownIndirectCall);
    } else {
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 42u);
    }
  }
}

TEST(SBFCallXContexts, NarrowDiscriminantsCrossOutputMemory) {
  for (unsigned Bytes : {1u, 2u, 4u})
    for (unsigned Mode = 0; Mode < 3; ++Mode) {
      SCOPED_TRACE(Bytes);
      SCOPED_TRACE(Mode);
      const Opcode Load = Bytes == 1   ? Opcode::LD_B_REG
                          : Bytes == 2 ? Opcode::LD_H_REG
                                       : Opcode::LD_W_REG;
      const Opcode Store = Bytes == 1   ? Opcode::ST_B_REG
                           : Bytes == 2 ? Opcode::ST_H_REG
                                        : Opcode::ST_W_REG;
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const auto Words = test::loadImm64(R, Value);
        const size_t Slot = Text.size();
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -32));
      const size_t Call = Text.size();
      Text.push_back({});
      if (Mode == 2) {
        Literal(1, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 1, 1));
        Text.push_back(encode(Opcode::MOV64_IMM, 2));
        // An unknown pointer can alias the byte tag just returned by callee.
        Text.push_back(encode(Store, 1, 2));
      }
      Text.push_back(encode(Load, 1, 10, -32));
      Text.push_back(encode(Opcode::JEQ64_IMM, 1, 0, 1, 1));
      Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -24));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -24));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 2));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Callee = Text.size();
      if (Mode == 1) {
        Literal(2, kInputStart);
        Text.push_back(encode(Load, 2, 2));
      } else {
        // The store must truncate, rather than preserving the register's
        // high bits in the tag or interpreting them as a pointer fragment.
        Literal(2, (uint64_t{1} << (Bytes * 8)) | 1);
      }
      Text.push_back(encode(Store, 1, 2));
      const size_t TargetLiteral = Literal(2, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 2, 8));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Target = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Callee - Call - 1);
      const auto Address = test::loadImm64(2, kBytecodeStart + Target * 8);
      std::copy(Address.begin(), Address.end(), Text.begin() + TargetLiteral);
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      EXPECT_EQ(llvm::any_of(Report->Contexts,
                             [&](const auto &Context) {
                               return Context.CallXSlot == CallX &&
                                      Context.TargetSlot == Target;
                             }),
                Mode == 0);
      for (uint64_t Tag : {0u, 1u}) {
        ExecutionEnvironment Environment;
        std::vector<uint8_t> Input(8);
        const uint64_t Value =
            Mode == 2 ? initialFramePointer(Program->Low.TheVersion,
                                            Program->Config) -
                            32
                      : Tag;
        llvm::support::endian::write64le(Input.data(), Value);
        Environment.Memory.push_back({kInputStart, Input, false, "tag input"});
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        const bool Returns = Mode == 0 || (Mode == 1 && Tag == 1);
        EXPECT_EQ(Raw->Status, Returns ? ExecutionStatus::Returned
                                       : ExecutionStatus::Faulted)
            << Raw->Error;
        if (Returns)
          EXPECT_EQ(Raw->ReturnValue, 42u);
      }
    }
}

TEST(SBFCallXContexts, TaggedReturnBranchesRecoverOnlyTheirOwnPayload) {
  size_t RawExecutions = 0;
  for (unsigned Bytes : {1u, 2u, 4u, 8u})
    for (unsigned Mode = 0; Mode < 9; ++Mode) {
      SCOPED_TRACE(Bytes);
      SCOPED_TRACE(Mode);
      const Opcode Load = Bytes == 1   ? Opcode::LD_B_REG
                          : Bytes == 2 ? Opcode::LD_H_REG
                          : Bytes == 4 ? Opcode::LD_W_REG
                                       : Opcode::LD_DW_REG;
      const Opcode Store = Bytes == 1   ? Opcode::ST_B_REG
                           : Bytes == 2 ? Opcode::ST_H_REG
                           : Bytes == 4 ? Opcode::ST_W_REG
                                        : Opcode::ST_DW_REG;
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -32));
      const size_t Call = Text.size();
      Text.push_back({});
      if (Mode == 1) {
        Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, 1));
        Text.push_back(encode(Store, 10, 2, -32));
      } else if (Mode == 2 || Mode == 3 || Mode == 8) {
        Literal(1, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 1, 8));
        if (Mode != 2) {
          // This pointer can alias either returned field.
          Text.push_back(encode(Opcode::LD_DW_REG, 1, 1, 16));
          Text.push_back(encode(Opcode::ST_DW_REG, 1, 2));
        } else {
          Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -24));
        }
      }
      Text.push_back(encode(Load, 7, 10, -32));
      unsigned TagRegister = 7;
      if (Mode == 4) {
        Text.push_back(encode(Opcode::MOV64_REG, 8, 7));
        TagRegister = 8;
      } else if (Mode == 5) {
        Literal(1, kInputStart);
        Text.push_back(encode(Load, 7, 1, 24));
      }
      const size_t Branch = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -24));
      if (Mode == 7 || Mode == 8)
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 2));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 2));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Success = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 5));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Callee = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
      Literal(3, kInputStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 3));
      const size_t Select = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 3, 8));
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
      Text.push_back(encode(Opcode::MOV64_IMM, 1));
      const size_t Join = Text.size();
      Text.push_back({});
      const size_t Error = Text.size();
      std::optional<size_t> AllocationCall;
      if (Mode == 7 || Mode == 8) {
        Literal(1, kInputStart + 32);
        AllocationCall = Text.size();
        Text.push_back({});
      }
      const size_t TargetLiteral = Literal(2, 0);
      if (Mode == 7 || Mode == 8) {
        Text.push_back(encode(Opcode::ST_DW_REG, 0, 2));
        Text.push_back(encode(Opcode::ST_DW_REG, 6, 0, 8));
      } else {
        Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
      }
      Text.push_back(Mode == 6 ? encode(Load, 1, 3, 24)
                               : encode(Opcode::MOV64_IMM, 1, 0, 0, 1));
      const size_t Tail = Text.size();
      Text.push_back(encode(Store, 6, 1));
      Text.push_back(encode(Opcode::EXIT));
      if (AllocationCall) {
        const size_t Allocate = Text.size();
        Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
        Literal(2, kHeapStart + 8);
        const size_t Below = Text.size();
        Text.push_back({});
        Literal(2, kHeapStart + 128);
        const size_t Above = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::EXIT));
        const size_t Fallback = Literal(0, kHeapStart + 32);
        Text.push_back(encode(Opcode::EXIT));
        Text[*AllocationCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Allocate - *AllocationCall - 1);
        Text[Below] = encode(Opcode::JLT64_REG, 0, 2, Fallback - Below - 1);
        Text[Above] = encode(Opcode::JGT64_REG, 0, 2, Fallback - Above - 1);
      }
      const size_t A = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t B = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Callee - Call - 1);
      Text[Branch] =
          encode(Opcode::JEQ64_IMM, TagRegister, 0, Success - Branch - 1);
      Text[Select] = encode(Opcode::JNE64_IMM, 2, 0, Error - Select - 1);
      Text[Join] = encode(Opcode::JA, 0, 0, Tail - Join - 1);
      const auto Address = test::loadImm64(2, kBytecodeStart + A * 8);
      std::copy(Address.begin(), Address.end(), Text.begin() + TargetLiteral);
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      EXPECT_EQ(
          llvm::any_of(Report->Contexts,
                       [&](const auto &Site) {
                         return Site.CallXSlot == CallX && Site.TargetSlot == A;
                       }),
          Mode == 0 || Mode == 4 || Mode == 7 || (Mode == 6 && Bytes < 8));
      for (uint64_t ErrorInput : {0u, 1u})
        for (unsigned TagTrial = 0; TagTrial < (Mode == 6 ? 3u : 1u);
             ++TagTrial) {
          const uint64_t TagInput = TagTrial == 0   ? 1
                                    : TagTrial == 1 ? 0
                                                    : UINT64_MAX;
          SCOPED_TRACE(TagInput);
          ExecutionEnvironment Environment;
          std::vector<uint8_t> Input(40);
          llvm::support::endian::write64le(Input.data(), ErrorInput);
          llvm::support::endian::write64le(Input.data() + 8,
                                           kBytecodeStart + B * 8);
          llvm::support::endian::write64le(
              Input.data() + 16,
              Mode == 8 ? kHeapStart + 32
                        : initialFramePointer(Program->Low.TheVersion,
                                              Program->Config) -
                              24);
          llvm::support::endian::write64le(Input.data() + 24, TagInput);
          llvm::support::endian::write64le(Input.data() + 32, kHeapStart + 32);
          Environment.Memory.push_back(
              {kInputStart, Input, false, "tag input"});
          Environment.Memory.push_back(
              {kHeapStart, std::vector<uint8_t>(256), true, "heap"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          ++RawExecutions;
          const bool TakesCall =
              (ErrorInput && (Mode != 6 || TagInput)) || Mode == 1 || Mode == 5;
          EXPECT_EQ(Raw->ReturnValue,
                    !TakesCall                                           ? 5u
                    : !ErrorInput || Mode == 2 || Mode == 3 || Mode == 8 ? 43u
                                                                         : 42u);
        }
    }
  RecordProperty("raw_executions", std::to_string(RawExecutions));
}

TEST(SBFCallXContexts, TaggedCaptureDefinitionsFollowTheSelectedCase) {
  for (unsigned Mode = 0; Mode < 7; ++Mode)
    for (bool ReverseEdges : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "mode=" << Mode << ", reverse=" << ReverseEdges);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      std::vector<size_t> Allocations, TargetLiterals;
      const auto Allocate = [&](unsigned InputOffset) {
        Literal(1, kInputStart + InputOffset);
        Allocations.push_back(Text.size());
        Text.push_back({});
      };
      if (Mode == 6) {
        Allocate(8);
        Text.push_back(encode(Opcode::MOV64_REG, 8, 0));
        Text.push_back(encode(Opcode::MOV64_REG, 2, 8));
      }
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -32));
      const size_t ProducerCall = Text.size();
      Text.push_back({});
      if (Mode == 3)
        Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -32, 1));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -32));
      if (Mode == 4) {
        Literal(1, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 1, 24));
      }
      if (Mode >= 5)
        Text.push_back(encode(Opcode::LD_DW_REG, 8, 10, -24));
      const size_t Branch = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_DW_REG, 7, 10, -24));
      const size_t ErrorJump = Text.size();
      Text.push_back({});
      const size_t Success = Text.size();
      Allocate(16);
      Text.push_back(encode(Opcode::MOV64_REG, 7, 0));
      const size_t OtherTarget = Literal(2, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 7, 2, 8));
      if (Mode >= 5)
        Text.push_back(encode(Opcode::MOV64_REG, 7, 8));
      const size_t Join = Text.size();
      if (Mode == 2) {
        Literal(1, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 1, 32));
        Text.push_back(encode(Opcode::LD_DW_REG, 1, 1, 40));
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 2, 8));
      }
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 7, 8));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 2));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Producer = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
      if (Mode == 6)
        Text.push_back(encode(Opcode::MOV64_REG, 7, 2));
      if (Mode == 5) {
        // The same name was introduced before either tag, including the
        // path whose field is unknown. Selecting a tag cannot erase it.
        Allocate(8);
        Text.push_back(encode(Opcode::MOV64_REG, 7, 0));
      }
      Literal(3, kInputStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 3));
      const size_t Select = Text.size();
      Text.push_back({});
      if (Mode >= 5) {
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 3, 32));
        Text.push_back(encode(Opcode::ST_DW_REG, 7, 2, 8));
        Text.push_back(encode(Opcode::ST_DW_REG, 6, 7, 8));
      } else {
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 3, 8));
        Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
      }
      Text.push_back(encode(Opcode::MOV64_IMM, 1));
      const size_t SuccessJump = Text.size();
      Text.push_back({});
      const size_t Error = Text.size();
      if (Mode < 5) {
        Allocate(8);
        Text.push_back(encode(Opcode::MOV64_REG, 7, 0));
      }
      TargetLiterals.push_back(Literal(2, 0));
      Text.push_back(encode(Opcode::ST_DW_REG, 7, 2, 8));
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 7, 8));
      Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, 1));
      const size_t Tail = Text.size();
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 1));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Allocator = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
      Literal(2, kHeapStart + 8);
      const size_t Below = Text.size();
      Text.push_back({});
      Literal(2, kHeapStart + 128);
      const size_t Above = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
      const size_t Fallback = Literal(0, kHeapStart + 32);
      Text.push_back(encode(Opcode::EXIT));
      const size_t A = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t B = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      Text[ProducerCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Producer - ProducerCall - 1);
      Text[Branch] = encode(Opcode::JEQ64_IMM, 2, 0, Success - Branch - 1);
      Text[ErrorJump] = encode(Opcode::JA, 0, 0, Join - ErrorJump - 1);
      Text[Select] = encode(Opcode::JNE64_IMM, 2, 0, Error - Select - 1);
      Text[SuccessJump] = encode(Opcode::JA, 0, 0, Tail - SuccessJump - 1);
      for (size_t Slot : Allocations)
        Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Allocator - Slot - 1);
      Text[Below] = encode(Opcode::JLT64_REG, 0, 2, Fallback - Below - 1);
      Text[Above] = encode(Opcode::JGT64_REG, 0, 2, Fallback - Above - 1);
      for (size_t Slot : TargetLiterals) {
        const auto Words = test::loadImm64(2, kBytecodeStart + A * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      }
      const auto Words =
          test::loadImm64(2, kBytecodeStart + (Mode == 1 ? B : A) * 8);
      std::copy(Words.begin(), Words.end(), Text.begin() + OtherTarget);
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (ReverseEdges)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      EXPECT_TRUE(llvm::any_of(Report->Summaries, [&](const auto &Summary) {
        return Summary.EntrySlot == Producer && Summary.OutputTaggedCases == 2;
      }));
      bool Singleton = false, Finite = false;
      for (const auto &Site : Report->Contexts)
        if (Site.CallXSlot == CallX) {
          Singleton |= Site.TargetSlot == A;
          Finite |= Site.Result == CallXContextResult::ProvenTargetSet;
          if (Mode >= 2)
            EXPECT_FALSE(Site.TargetAddress ||
                         Site.Result == CallXContextResult::ProvenTargetSet);
        }
      EXPECT_EQ(Singleton, Mode == 0);
      EXPECT_EQ(Finite, Mode == 1);
      for (unsigned ErrorInput : {0u, 1u})
        for (bool Alias : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << "error=" << ErrorInput << ", alias=" << Alias);
          std::vector<uint8_t> Input(48), Heap(256);
          const uint64_t FirstObject = kHeapStart + 32;
          const uint64_t SecondObject = kHeapStart + (Alias ? 32 : 96);
          llvm::support::endian::write64le(Input.data(), ErrorInput);
          llvm::support::endian::write64le(Input.data() + 8, FirstObject);
          llvm::support::endian::write64le(Input.data() + 16, SecondObject);
          llvm::support::endian::write64le(Input.data() + 24, 1);
          llvm::support::endian::write64le(Input.data() + 32,
                                           kBytecodeStart + B * 8);
          llvm::support::endian::write64le(
              Input.data() + 40, Alias
                                     ? (ErrorInput ? FirstObject : SecondObject)
                                     : kHeapStart + 160);
          llvm::support::endian::write64le(Heap.data() + 40,
                                           kBytecodeStart + B * 8);
          ExecutionEnvironment Environment;
          Environment.Memory.push_back({kInputStart, Input, false, "input"});
          Environment.Memory.push_back({kHeapStart, Heap, true, "objects"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          const bool CallsB = (Mode == 1 && !ErrorInput) ||
                              (Mode == 2 && Alias) ||
                              ((Mode == 3 || Mode == 4) && !ErrorInput) ||
                              (Mode >= 5 && !ErrorInput && !Alias);
          EXPECT_EQ(Raw->ReturnValue, CallsB ? 43u : 42u);
          EXPECT_EQ(llvm::count_if(
                        Raw->Trace,
                        [&](const auto &Step) { return Step.Slot == CallX; }),
                    1);
        }
    }
}

TEST(SBFCallXContexts, TaggedReturnsKeepTheCompleteAlternativeLimit) {
  for (unsigned Count :
       {kCallXContextValueAlternatives - 1, kCallXContextValueAlternatives,
        kCallXContextValueAlternatives + 1}) {
    SCOPED_TRACE(Count);
    std::vector<test::EncodedInstruction> Text;
    const auto Literal = [&](unsigned R, uint64_t Value) {
      const size_t Slot = Text.size();
      const auto Words = test::loadImm64(R, Value);
      Text.insert(Text.end(), Words.begin(), Words.end());
      return Slot;
    };
    Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
    Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
    const size_t Call = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::LD_DW_REG, 1, 10, -16));
    Text.push_back(encode(Opcode::JNE64_IMM, 1, 0, 3, Count - 1));
    Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -8));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 2));
    Text.push_back(encode(Opcode::EXIT));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Callee = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    Literal(3, kInputStart);
    Text.push_back(encode(Opcode::LD_DW_REG, 2, 3));
    std::vector<size_t> Selects, Entries, Joins;
    for (unsigned I = 0; I + 1 < Count; ++I) {
      Selects.push_back(Text.size());
      Text.push_back({});
    }
    const size_t Default = Text.size();
    Text.push_back({});
    size_t TargetLiteral = 0;
    for (unsigned I = 0; I < Count; ++I) {
      Entries.push_back(Text.size());
      if (I + 1 == Count)
        TargetLiteral = Literal(2, 0);
      else
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 3, 8));
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
      Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, I));
      Joins.push_back(Text.size());
      Text.push_back({});
    }
    const size_t Tail = Text.size();
    Text.push_back(encode(Opcode::ST_DW_REG, 6, 1));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Target = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Callee - Call - 1);
    for (unsigned I = 0; I + 1 < Count; ++I)
      Text[Selects[I]] =
          encode(Opcode::JEQ64_IMM, 2, 0, Entries[I] - Selects[I] - 1, I);
    Text[Default] = encode(Opcode::JA, 0, 0, Entries.back() - Default - 1);
    for (size_t At : Joins)
      Text[At] = encode(Opcode::JA, 0, 0, Tail - At - 1);
    const auto Address = test::loadImm64(2, kBytecodeStart + Target * 8);
    std::copy(Address.begin(), Address.end(), Text.begin() + TargetLiteral);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_EQ(llvm::any_of(Report->Contexts,
                           [&](const auto &Site) {
                             return Site.CallXSlot == CallX &&
                                    Site.TargetSlot == Target;
                           }),
              Count <= kCallXContextValueAlternatives);
    for (unsigned InputCase = 0; InputCase <= Count; ++InputCase) {
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input(16);
      llvm::support::endian::write64le(Input.data(), InputCase);
      Environment.Memory.push_back({kInputStart, Input, false, "tag selector"});
      auto Raw = executeRaw(*Program, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, InputCase + 1 >= Count ? 42u : 0u);
    }
  }
}

TEST(SBFStoredRegisterMemory, NarrowCopiesKeepWidthsAndRejectPointerFragments) {
  const RegisterValue Source{RegisterValue::Kind::StackAddress,
                             kStackStart + 32, 0};
  const RegisterValue Destination{RegisterValue::Kind::StackAddress,
                                  kStackStart + 64, 0};
  const RegisterValue Pointer{RegisterValue::Kind::SymbolicAddress, 0, 0};
  for (unsigned Width : {1u, 2u, 4u}) {
    SCOPED_TRACE(Width);
    StoredRegisterMemory Memory;
    Memory.setFrameTranslationLimit(4096);
    Memory.writeValues(Source,
                       RegisterValue{RegisterValue::Kind::Constant,
                                     (uint64_t{1} << (Width * 8)) | 1, 0},
                       Width);
    Memory.writeValues(*memoryAddress(Source, 8), Pointer);
    Memory.copyFrom(Memory, Source, Destination, 16);
    EXPECT_EQ(Memory.readValues(Destination, Width).singleton().Value, 1u);
    EXPECT_TRUE(Memory.readValues(Destination).unknown());
    EXPECT_EQ(Memory.readValues(*memoryAddress(Destination, 8)).singleton(),
              Pointer);
    // A partial copy cannot create a complete scalar cell or pointer tag.
    const RegisterValue Partial{RegisterValue::Kind::StackAddress,
                                kStackStart + 96, 0};
    Memory.copyFrom(Memory, Source, Partial, Width - 1);
    EXPECT_TRUE(Memory.readValues(Partial, Width).unknown());
    Memory.writeValues(Destination, Pointer, Width);
    EXPECT_TRUE(Memory.readValues(Destination, Width).unknown());
    EXPECT_EQ(Memory.readValues(*memoryAddress(Destination, 8)).singleton(),
              Pointer);
    Memory.invalidate(RegisterValue{}, 1);
    EXPECT_TRUE(Memory.empty());

    callx_detail::MemoryPostconditions Post;
    const RegisterValue FormalSource{RegisterValue::Kind::StackAddress, 2, 0};
    const RegisterValue FormalDestination{RegisterValue::Kind::StackAddress, 3,
                                          0};
    auto Program = analyze(test::makeImage({encode(Opcode::EXIT)}, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    Post.store(FormalSource, Width,
               RegisterValue{RegisterValue::Kind::Constant, 1, 0});
    Post.copyFrom(FormalSource, FormalDestination, Width, *Program);
    EXPECT_EQ(Post.read(FormalDestination, Width).singleton().Value, 1u);
    EXPECT_TRUE(Post.read(FormalDestination).unknown());
    Post.store(FormalDestination, Width, Pointer);
    EXPECT_TRUE(Post.read(FormalDestination, Width).unknown());
  }
}

TEST(SBFStoredRegisterMemory, CapturesProveEqualityWithoutAssumingFreshness) {
  const RegisterValue Number{RegisterValue::Kind::Constant, 42, 0};
  const RegisterValue First{RegisterValue::Kind::SymbolicAddress, 0, 0};
  const RegisterValue SameRange{RegisterValue::Kind::SymbolicAddress, 1, 0};
  auto Addresses = std::make_shared<SymbolicAddressSpace>();
  Addresses->push_back({kHeapStart + 8, UINT64_MAX});
  Addresses->push_back({kHeapStart + 8, UINT64_MAX});
  StoredRegisterMemory Memory;
  Memory.setSymbolicAddresses(Addresses);
  Memory.setFrameTranslationLimit(4096);
  Memory.writeValues(First, Number);
  EXPECT_EQ(Memory.readValues(First).singleton(), Number);
  EXPECT_TRUE(Memory.readValues(SameRange).unknown());
  // A wrapped byte footprint can touch address zero, but not this frame.
  Memory.write(kStackStart + 8, Number, MemoryCoordinate::FrameRelative);
  Memory.writeValues(First, Number);
  EXPECT_EQ(Memory.read(kStackStart + 8, MemoryCoordinate::FrameRelative),
            Number);
  Memory.invalidate(SameRange, 8);
  EXPECT_TRUE(Memory.readValues(First).unknown());
  EXPECT_EQ(Memory.read(kStackStart + 8, MemoryCoordinate::FrameRelative),
            Number);
  // A negative displacement can reach a caller frame; its original heap lower
  // bound must not survive that arithmetic unchanged.
  const RegisterValue Shifted{RegisterValue::Kind::SymbolicAddress, 1,
                              -static_cast<int64_t>(kHeapStart - kStackStart)};
  Memory.invalidate(Shifted, 8);
  EXPECT_TRUE(Memory.empty());
  EXPECT_FALSE(effectiveAddress(First, 0));
}

TEST(SBFCallXMemoryPostconditions,
     InputCopiesCaptureOnlyUnmodifiedEntryFields) {
  using namespace callx_detail;
  auto Program = analyze(test::makeImage({encode(Opcode::EXIT)}, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  const RegisterValue Source{RegisterValue::Kind::StackAddress, 2, 0};
  const RegisterValue Destination{RegisterValue::Kind::StackAddress, 3, 0};
  const RegisterValue Target{RegisterValue::Kind::Constant, 42, 0};
  std::array<ProvenValueSet, kRegisterCount> Entry{};
  Entry[1] = RegisterValue{RegisterValue::Kind::Constant, kHeapStart + 16, 0};
  Entry[2] = RegisterValue{RegisterValue::Kind::Constant, kHeapStart + 20, 0};
  ScratchState Before;
  Before.Values.write(kHeapStart + 16, Target);
  for (unsigned Mode = 0; Mode < 6; ++Mode) {
    SCOPED_TRACE(Mode);
    MemoryPostconditions Post;
    MemoryEffects Writes;
    if (Mode == 1)
      Writes.addWrite(Source, 1, *Program);
    if (Mode == 2)
      Writes.Unknown = true;
    Post.recordInputCopy(Source, Destination, Mode == 3 ? 7 : 8, Writes);
    if (Mode == 4)
      Post.store({RegisterValue::Kind::StackAddress, 3, 7}, 1, {});
    if (Mode == 5)
      Post.meet(MemoryPostconditions{});
    ScratchState After;
    Post.apply(After, Entry, *Program, &Before);
    EXPECT_EQ(After.Values.read(kHeapStart + 20),
              Mode == 0 ? Target : RegisterValue{});
    // The original entry snapshot remains intact even for overlapping move.
    EXPECT_EQ(Before.Values.read(kHeapStart + 16), Target);
  }
}

TEST(SBFCallXMemoryPostconditions, RebasesWritesThroughCapturedPointers) {
  using namespace callx_detail;
  auto Program = analyze(test::makeImage({encode(Opcode::EXIT)}, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Addresses = std::make_shared<SymbolicAddressSpace>();
  Addresses->push_back({kHeapStart + 8, UINT64_MAX});
  const RegisterValue Pointer{RegisterValue::Kind::SymbolicAddress, 0, 0};
  const RegisterValue Field{RegisterValue::Kind::SymbolicAddress, 0, 16};
  const RegisterValue Value{RegisterValue::Kind::Constant, 42, 0};
  MemoryEffects Effects;
  Effects.addWrite({RegisterValue::Kind::StackAddress, 2, 0}, 8, *Program);
  auto Origins = symbolicInputs();
  Origins[1] = Pointer;
  MemoryEffects Rebased;
  Rebased.include(Effects, Origins, *Program, Addresses.get());
  EXPECT_FALSE(Rebased.Unknown);
  std::array<ProvenValueSet, kRegisterCount> Entry{};
  Entry[1] = Pointer;
  ScratchState Scratch;
  Scratch.Values.setSymbolicAddresses(Addresses);
  Scratch.Values.setFrameTranslationLimit(4096);
  Scratch.Values.write(kStackStart + 16, Value,
                       MemoryCoordinate::FrameRelative);
  Scratch.Values.writeValues(Pointer, Value);
  Scratch.Values.writeValues(Field, Value);
  EXPECT_TRUE(Effects.invalidate(Scratch, Entry, *Program));
  EXPECT_TRUE(Scratch.Values.readValues(Pointer).unknown());
  EXPECT_EQ(Scratch.Values.readValues(Field).singleton(), Value);
  EXPECT_EQ(
      Scratch.Values.read(kStackStart + 16, MemoryCoordinate::FrameRelative),
      Value);
  // The rebased may-write footprint keeps the frame too. Its different
  // object identity is not an exact strong-update address.
  EXPECT_TRUE(Rebased.invalidate(Scratch, Entry, *Program));
  EXPECT_EQ(
      Scratch.Values.read(kStackStart + 16, MemoryCoordinate::FrameRelative),
      Value);
}

TEST(SBFCallXContexts,
     ForwardedImageBindingsCanReplaceRedundantOpaqueVariants) {
  for (bool DiverseImageInputs : {false, true}) {
    SCOPED_TRACE(DiverseImageInputs);
    for (bool RepeatUnknownInputs : {false, true}) {
      SCOPED_TRACE(RepeatUnknownInputs);
      const size_t Count = (kCallXContextSpecializationsPerEntry + 1) *
                           (RepeatUnknownInputs ? 5 : 1);
      constexpr uint32_t TableAddress = 0x10000;
      std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
      std::vector<size_t> Entries, Calls;
      for (size_t I = 0; I < Count; ++I) {
        Entries.push_back(Text.size());
        Text.push_back(
            (I + 1) % (kCallXContextSpecializationsPerEntry + 1) == 0
                ? encode(Opcode::MOV64_IMM, 8, 0, 0, RepeatUnknownInputs)
                : encode(Opcode::LD_DW_REG, 8, 1));
        Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0,
                              TableAddress + (DiverseImageInputs ? I * 8 : 0)));
        Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, I));
        Calls.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Dispatcher = Text.size();
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -8));
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 1));
      Text.push_back(encode(Opcode::JEQ64_IMM, 8, 0, 1));
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 4));
      const size_t Forward = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                            getSyscallInfo(Syscall::Memset)->Hash));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Leaf = Text.size();
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 5));
      Text.push_back(encode(Opcode::EXIT));
      Text[Forward] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Forward - 1);
      const size_t Target = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      for (size_t Call : Calls)
        Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Dispatcher - Call - 1);
      std::vector<uint8_t> Table(Count * sizeof(uint64_t));
      for (size_t I = 0; I < Count; ++I)
        llvm::support::endian::write64le(Table.data() + I * sizeof(uint64_t),
                                         kBytecodeStart +
                                             Target * kInstructionSize);
      auto Image = test::makeImage(Text, Table);
      Image.Sections[1].VA = TableAddress;
      Image.SBF->RodataVM = {TableAddress, Table.size()};
      auto Program = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      EXPECT_EQ(llvm::any_of(Report->Contexts,
                             [&](const auto &Site) {
                               return !Site.EntryPrefixOnly &&
                                      Site.CallerSlot == Forward &&
                                      Site.CallXSlot == CallX &&
                                      Site.TargetSlot == Target;
                             }),
                DiverseImageInputs && !RepeatUnknownInputs);
      EXPECT_EQ(Report->GeneralizedPublishedSummaries,
                DiverseImageInputs ? 1u : 0u);
      checkSummaryReferences(*Report);
      for (size_t I = 0; I < Entries.size(); ++I)
        for (uint64_t Selector : {0u, 1u}) {
          SCOPED_TRACE(::testing::Message() << I << ":" << Selector);
          Image.Entry = kBytecodeStart + Entries[I] * kInstructionSize;
          auto Selected = analyze(Image);
          ASSERT_TRUE(static_cast<bool>(Selected))
              << llvm::toString(Selected.takeError());
          ExecutionEnvironment Environment;
          std::vector<uint8_t> Input(sizeof(uint64_t));
          llvm::support::endian::write64le(Input.data(), Selector);
          Environment.Memory.push_back({kInputStart, Input, false, "selector"});
          Environment.HostSyscall = [](uint32_t Hash,
                                       const SyscallArguments &Args) {
            if (Hash != getSyscallInfo(Syscall::Memset)->Hash)
              return SyscallOutcome::unregistered();
            // r3 is zero on these raw paths; this is a faithful empty memset.
            EXPECT_EQ(Args[2], 0u);
            return SyscallOutcome::returned(0);
          };
          auto Raw = executeRaw(*Selected, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          const bool ReachesTarget =
              (I + 1) % (kCallXContextSpecializationsPerEntry + 1) == 0
                  ? !RepeatUnknownInputs
                  : Selector == 0;
          EXPECT_EQ(Raw->Status, ReachesTarget ? ExecutionStatus::Returned
                                               : ExecutionStatus::Faulted)
              << Raw->Error;
          EXPECT_EQ(llvm::any_of(
                        Raw->Trace,
                        [&](const auto &Step) { return Step.Slot == Target; }),
                    ReachesTarget);
          if (ReachesTarget)
            EXPECT_EQ(Raw->ReturnValue, 0u);
        }
    }
  }
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

TEST(SBFCallXContexts, FullVariantCacheKeepsOnlySelectedFirstCallProofs) {
  unsigned RawExecutions = 0;
  for (unsigned Mode : {0u, 1u, 2u, 3u, 4u}) {
    SCOPED_TRACE(Mode);
    std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
    const auto Literal = [&](unsigned R, uint64_t V) {
      const size_t Slot = Text.size();
      const auto Words = test::loadImm64(R, V);
      Text.insert(Text.end(), Words.begin(), Words.end());
      return Slot;
    };
    std::vector<size_t> Entries, Calls, Targets, EarlyTargets;
    for (unsigned I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
      Entries.push_back(Text.size());
      Targets.push_back(Literal(2, 0));
      EarlyTargets.push_back(Literal(4, 0));
      Literal(5, kInputStart);
      if (I == kCallXContextSpecializationsPerEntry && Mode == 2)
        Text.push_back(encode(Opcode::LD_DW_REG, 3, 5));
      else
        Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0,
                              I == kCallXContextSpecializationsPerEntry
                                  ? Mode == 1   ? 1
                                    : Mode == 3 ? 2
                                                : 0
                                  : 0));
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 5, 8));
      Calls.push_back(Text.size());
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
    }
    const size_t Leaf = Text.size();
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -8));
    Text.push_back(encode(Opcode::MOV64_REG, 6, 5));
    Text.push_back(encode(Opcode::MOV64_REG, 7, 4));
    const size_t SelectLoop = Text.size();
    Text.push_back({});
    const size_t SkipEarly = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::CALL_REG, 7));
    const size_t JumpJoin = Text.size();
    Text.push_back({});
    const size_t Loop = Text.size();
    Text.push_back(encode(Opcode::JA, 0, 0, -1));
    const size_t Join = Text.size();
    if (Mode == 4)
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 7));
    Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -8));
    const size_t Site = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 2));
    Text.push_back(encode(Opcode::EXIT));
    Text[SelectLoop] =
        encode(Opcode::JEQ64_IMM, 3, 0, Loop - SelectLoop - 1, 2);
    Text[SkipEarly] = encode(Opcode::JEQ64_IMM, 3, 0, Join - SkipEarly - 1, 0);
    Text[JumpJoin] = encode(Opcode::JA, 0, 0, Join - JumpJoin - 1);
    std::vector<size_t> TargetEntries;
    for (size_t I = 0; I < Calls.size(); ++I) {
      TargetEntries.push_back(Text.size());
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42 + I));
      Text.push_back(encode(Opcode::EXIT));
    }
    for (size_t I = 0; I < Calls.size(); ++I) {
      Text[Calls[I]] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Calls[I] - 1);
      for (auto [Slot, R, Target] :
           {std::tuple{Targets[I], 2u, TargetEntries[I]},
            std::tuple{EarlyTargets[I], 4u, TargetEntries.front()}}) {
        const auto Words =
            test::loadImm64(R, kBytecodeStart + Target * kInstructionSize);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      }
    }
    auto Image = test::makeImage(Text, {});
    for (size_t Entry : Entries) {
      Symbol Function;
      Function.Name = "path_caller_" + std::to_string(Entry);
      Function.Addr = kBytecodeStart + Entry * kInstructionSize;
      Function.IsFunc = true;
      Image.Symbols.push_back(std::move(Function));
    }
    auto Program = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_EQ(llvm::count_if(Report->Summaries,
                             [&](const auto &S) {
                               return S.EntrySlot == Leaf &&
                                      !S.GeneralizedInputs;
                             }),
              kCallXContextSpecializationsPerEntry);
    EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
      return Edge.CallerSlot == Calls.back() && Edge.GeneralizedInputs &&
             Edge.InputGeneralization ==
                 CallXInputGeneralization::SpecializationLimit;
    }));
    EXPECT_EQ(llvm::any_of(Report->Contexts,
                           [&](const auto &Proof) {
                             return Proof.CallerSlot == Calls.back() &&
                                    Proof.CallXSlot == Site &&
                                    Proof.CallerSeededFirstCall &&
                                    Proof.TargetSlot == TargetEntries.back();
                           }),
              Mode == 0);
    for (const auto &Summary : Report->Summaries)
      if (Summary.EntrySlot == Entries.back() && !Summary.GeneralizedInputs &&
          Mode == 0)
        EXPECT_EQ(Summary.ReturnValue.ValueKind, RegisterValue::Kind::Unknown);
    auto JSON = callXContextReportJSON(*Program, *Report);
    bool Scope = false;
    for (const auto &V : *JSON.getArray("contexts")) {
      const auto &Row = *V.getAsObject();
      Scope |=
          Row.getInteger("caller_slot") == Calls.back() &&
          Row.getInteger("callx_slot") == Site &&
          Row.getString("evidence_scope") == "caller_seeded_first_call_path";
    }
    EXPECT_EQ(Scope, Mode == 0);
    Image.Entry = kBytecodeStart + Entries.back() * kInstructionSize;
    auto Selected = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Selected))
        << llvm::toString(Selected.takeError());
    for (uint64_t Selector : {0u, 1u}) {
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input(16);
      llvm::support::endian::write64le(Input.data(), Selector);
      llvm::support::endian::write64le(Input.data() + 8, kHeapStart);
      Environment.Memory.push_back({kInputStart, Input, false, "input"});
      Environment.Memory.push_back(
          {kHeapStart, std::vector<uint8_t>(16), true, "heap"});
      InterpreterOptions Options;
      Options.MaxSteps = 100;
      auto Raw = executeRaw(*Selected, std::move(Environment), Options);
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, Mode == 3 ? ExecutionStatus::StepLimit
                                       : ExecutionStatus::Returned)
          << Raw->Error;
      if (Mode != 3)
        EXPECT_EQ(Raw->ReturnValue, 42u + kCallXContextSpecializationsPerEntry);
      ++RawExecutions;
    }
  }
  RecordProperty("raw_executions", RawExecutions);
}

TEST(SBFCallXContexts, FullVariantCacheReplaysKnownFixedMemoryPaths) {
  unsigned RawExecutions = 0;
  for (unsigned Mode = 0; Mode < 5; ++Mode)
    for (bool Wrapped : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Mode << ", wrapped=" << Wrapped);
      std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      std::vector<size_t> Entries, Calls, Sites, TargetLiterals, OtherTargets;
      uint64_t LastCursor = 0;
      for (unsigned I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
        const bool Last = I == kCallXContextSpecializationsPerEntry;
        const uint64_t Cursor = I ? kHeapStart + 128 + 32 * I : 0;
        const uint64_t Size = 8 + 8 * I;
        const uint64_t Object = (Cursor ? Cursor : kHeapStart + 512) - Size;
        LastCursor = Cursor;
        Entries.push_back(Text.size());
        Literal(3, kHeapStart);
        if (Last && Mode == 1) {
          Literal(2, kInputStart);
          Text.push_back(encode(Opcode::LD_DW_REG, 2, 2));
        } else {
          Literal(2, Cursor);
        }
        Text.push_back(encode(Opcode::ST_DW_REG, 3, 2));
        Literal(3, Object);
        TargetLiterals.push_back(Literal(5, 0));
        Text.push_back(encode(Opcode::ST_DW_REG, 3, 5));
        OtherTargets.push_back(Literal(5, 0));
        Text.push_back(encode(Opcode::ST_DW_REG, 3, 5, 64));
        if (Last && Mode == 2) {
          Literal(4, kInputStart);
          Text.push_back(encode(Opcode::LD_DW_REG, 4, 4));
        } else {
          Literal(4, kHeapStart + 800);
        }
        Literal(5, Cursor + 64);
        Literal(1, Last && Mode == 4 ? UINT64_MAX : Size);
        Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, 8));
        Calls.push_back(Text.size());
        Text.push_back({});
        const size_t Failed = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 0));
        Sites.push_back(Text.size());
        Text.push_back(encode(Opcode::CALL_REG, 2));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Done = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 5));
        Text.push_back(encode(Opcode::EXIT));
        Text[Failed] = encode(Opcode::JEQ64_IMM, 0, 0, Done - Failed - 1);
      }
      const size_t Wrapper = Text.size();
      std::optional<size_t> NestedCall;
      if (Wrapped) {
        NestedCall = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Leaf = Text.size();
      if (Mode == 2)
        Text.push_back(encode(Opcode::ST_DW_REG, 4, 5));
      if (Mode == 3) {
        Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, 2));
        const size_t Loop = Text.size();
        Text.push_back(encode(Opcode::ADD64_IMM, 9, 0, 0, -1));
        Text.push_back(encode(Opcode::JNE64_IMM, 9, 0, Loop - Text.size() - 1));
      }
      Literal(3, kHeapStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 3, 3));
      Literal(4, kHeapStart + 512);
      Text.push_back(encode(Opcode::JEQ64_IMM, 3, 0, 1));
      Text.push_back(encode(Opcode::MOV64_REG, 4, 3));
      Text.push_back(encode(Opcode::MOV64_REG, 3, 4));
      Text.push_back(encode(Opcode::SUB64_REG, 3, 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      const size_t Underflow = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::NEG64, 2));
      Text.push_back(encode(Opcode::AND64_REG, 3, 2));
      Literal(2, kHeapStart + 8);
      const size_t Exhausted = Text.size();
      Text.push_back({});
      Literal(2, kHeapStart);
      Text.push_back(encode(Opcode::ST_DW_REG, 2, 3));
      Text.push_back(encode(Opcode::MOV64_REG, 0, 3));
      const size_t Return = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      Text[Underflow] = encode(Opcode::JGT64_REG, 3, 4, Return - Underflow - 1);
      Text[Exhausted] = encode(Opcode::JLT64_REG, 3, 2, Return - Exhausted - 1);
      if (NestedCall)
        Text[*NestedCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - *NestedCall - 1);
      std::vector<size_t> Targets;
      for (size_t I = 0; I < Calls.size(); ++I) {
        Targets.push_back(Text.size());
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42 + I));
        Text.push_back(encode(Opcode::EXIT));
      }
      for (size_t I = 0; I < Calls.size(); ++I) {
        Text[Calls[I]] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
        for (auto [Slot, Target] :
             {std::pair{TargetLiterals[I], Targets[I]},
              std::pair{OtherTargets[I], Targets.front()}}) {
          const auto Words =
              test::loadImm64(5, kBytecodeStart + Target * kInstructionSize);
          std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
        }
      }
      auto Image = test::makeImage(Text, {});
      for (size_t Entry : Entries) {
        Symbol Function;
        Function.Name = "fixed_memory_caller_" + std::to_string(Entry);
        Function.Addr = kBytecodeStart + Entry * kInstructionSize;
        Function.IsFunc = true;
        Image.Symbols.push_back(std::move(Function));
      }
      auto Program = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      EXPECT_EQ(llvm::count_if(Report->Summaries,
                               [&](const auto &S) {
                                 return S.EntrySlot == Wrapper &&
                                        !S.GeneralizedInputs;
                               }),
                kCallXContextSpecializationsPerEntry);
      EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
        return Edge.CallerSlot == Calls.back() && Edge.GeneralizedInputs &&
               Edge.InputGeneralization ==
                   CallXInputGeneralization::SpecializationLimit;
      }));
      bool Proven = false;
      for (const auto &Site : Report->Contexts)
        if (Site.CallXSlot == Sites.back() && Site.TargetSlot) {
          EXPECT_EQ(*Site.TargetSlot, Targets.back());
          Proven = true;
        }
      EXPECT_EQ(Proven, Mode == 0);
      Image.Entry = kBytecodeStart + Entries.back() * kInstructionSize;
      auto Selected = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Selected))
          << llvm::toString(Selected.takeError());
      for (unsigned Trial : {0u, 1u}) {
        std::vector<uint8_t> Input(8);
        llvm::support::endian::write64le(
            Input.data(), Mode == 2 ? (Trial ? kHeapStart : kHeapStart + 800)
                                    : LastCursor + 64 * Trial);
        ExecutionEnvironment Environment;
        Environment.Memory.push_back({kInputStart, Input, false, "input"});
        Environment.Memory.push_back(
            {kHeapStart, std::vector<uint8_t>(1024), true, "heap"});
        auto Raw = executeRaw(*Selected, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        ++RawExecutions;
        EXPECT_EQ(Raw->ReturnValue,
                  Mode == 4 ? 5u
                  : ((Mode == 1 || Mode == 2) && Trial)
                      ? 42u
                      : 42u + kCallXContextSpecializationsPerEntry);
      }
    }
  RecordProperty("raw_executions", RawExecutions);
}

TEST(SBFCallXContexts, FixedMemoryReturnsRetainConditionalObjectFields) {
  std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
  const auto Literal = [&](unsigned R, uint64_t Value) {
    const size_t Slot = Text.size();
    const auto Words = test::loadImm64(R, Value);
    Text.insert(Text.end(), Words.begin(), Words.end());
    return Slot;
  };
  std::vector<size_t> Entries, Calls, TargetLiterals;
  size_t Site = 0;
  for (unsigned I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
    Entries.push_back(Text.size());
    Literal(3, kHeapStart);
    Literal(2, kHeapStart + 128);
    Text.push_back(encode(Opcode::ST_DW_REG, 3, 2));
    Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, 8 * (I + 1)));
    if (I < kCallXContextSpecializationsPerEntry) {
      Calls.push_back(Text.size());
      Text.push_back({});
      TargetLiterals.push_back(Literal(2, 0));
      Text.push_back(encode(Opcode::ST_DW_REG, 0, 2, 8));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 0, 8));
      Text.push_back(encode(Opcode::CALL_REG, 2));
      Text.push_back(encode(Opcode::EXIT));
      continue;
    }
    Literal(9, kInputStart);
    Text.push_back(encode(Opcode::LD_DW_REG, 9, 9));
    const size_t Select = Text.size();
    Text.push_back({});
    Calls.push_back(Text.size());
    Text.push_back({});
    TargetLiterals.push_back(Literal(2, 0));
    Text.push_back(encode(Opcode::ST_DW_REG, 0, 2, 8));
    const size_t Jump = Text.size();
    Text.push_back({});
    const size_t Other = Text.size();
    Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, 8));
    Calls.push_back(Text.size());
    Text.push_back({});
    TargetLiterals.push_back(Literal(2, 0));
    Text.push_back(encode(Opcode::ST_DW_REG, 0, 2, 8));
    const size_t Join = Text.size();
    Text.push_back(encode(Opcode::LD_DW_REG, 2, 0, 8));
    Site = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 2));
    Text.push_back(encode(Opcode::EXIT));
    Text[Select] = encode(Opcode::JNE64_IMM, 9, 0, Other - Select - 1);
    Text[Jump] = encode(Opcode::JA, 0, 0, Join - Jump - 1);
  }
  const size_t Helper = Text.size();
  Literal(3, kHeapStart);
  Text.push_back(encode(Opcode::LD_DW_REG, 0, 3));
  Text.push_back(encode(Opcode::ADD64_REG, 0, 1));
  Literal(2, kHeapStart + 8);
  Text.push_back(encode(Opcode::JGE64_REG, 0, 2, 2));
  Text.push_back(encode(Opcode::MOV64_IMM, 0));
  Text.push_back(encode(Opcode::EXIT));
  Text.push_back(encode(Opcode::ST_DW_REG, 3, 0));
  Text.push_back(encode(Opcode::EXIT));
  std::vector<size_t> Targets;
  for (size_t I = 0; I < TargetLiterals.size(); ++I) {
    Targets.push_back(Text.size());
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42 + I));
    Text.push_back(encode(Opcode::EXIT));
    const auto Words =
        test::loadImm64(2, kBytecodeStart + Targets.back() * kInstructionSize);
    std::copy(Words.begin(), Words.end(), Text.begin() + TargetLiterals[I]);
  }
  for (size_t Call : Calls)
    Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Call - 1);
  auto Image = test::makeImage(Text, {});
  for (size_t Entry : Entries) {
    Symbol Function;
    Function.Name = "fixed_memory_join_" + std::to_string(Entry);
    Function.Addr = kBytecodeStart + Entry * kInstructionSize;
    Function.IsFunc = true;
    Image.Symbols.push_back(std::move(Function));
  }
  auto Program = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  checkSummaryReferences(*Report);
  EXPECT_FALSE(Report->SpecializationBudgetExhausted);
  EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
    return Edge.CallerSlot == Calls.back() &&
           Edge.InputGeneralization ==
               CallXInputGeneralization::SpecializationLimit;
  }));
  EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Context) {
    if (Context.CallXSlot != Site ||
        Context.TargetValueAlternatives.values().size() != 2)
      return false;
    return llvm::all_of(
        llvm::ArrayRef(Targets).take_back(2), [&](size_t Target) {
          return llvm::any_of(
              Context.TargetValueAlternatives.values(), [&](const auto &Value) {
                return Value.ValueKind == RegisterValue::Kind::Constant &&
                       Value.Value ==
                           kBytecodeStart + Target * kInstructionSize;
              });
        });
  }));
  Image.Entry = kBytecodeStart + Entries.back() * kInstructionSize;
  auto Selected = analyze(Image);
  ASSERT_TRUE(static_cast<bool>(Selected))
      << llvm::toString(Selected.takeError());
  for (unsigned Choice : {0u, 1u}) {
    std::vector<uint8_t> Input(8);
    llvm::support::endian::write64le(Input.data(), Choice);
    ExecutionEnvironment Environment;
    Environment.Memory.push_back({kInputStart, Input, false, "choice"});
    Environment.Memory.push_back(
        {kHeapStart, std::vector<uint8_t>(512), true, "heap"});
    auto Raw = executeRaw(*Selected, std::move(Environment));
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue,
              42u + kCallXContextSpecializationsPerEntry + Choice);
  }
  RecordProperty("raw_executions", 2);
}

TEST(SBFStoredRegisterMemory, ExactCapturesCanReadUnconditionalAbsoluteCells) {
  const auto Literal = [](uint64_t V) {
    return RegisterValue{RegisterValue::Kind::Constant, V, 0};
  };
  auto Addresses = std::make_shared<SymbolicAddressSpace>();
  Addresses->push_back({kHeapStart + 32, kHeapStart + 32});
  Addresses->push_back({kHeapStart + 32, kHeapStart + 40});
  StoredRegisterMemory Memory;
  Memory.setSymbolicAddresses(Addresses);
  const RegisterValue Exact{RegisterValue::Kind::SymbolicAddress, 0, 8};
  const RegisterValue Range{RegisterValue::Kind::SymbolicAddress, 1, 8};
  Memory.writeValues(Literal(kHeapStart + 40), Literal(42));
  EXPECT_EQ(Memory.readValues(Exact).singleton(), Literal(42));
  EXPECT_TRUE(Memory.readValues(Exact, 1).unknown());
  EXPECT_TRUE(Memory.readValues(Range).unknown());
  Memory.writeValues(Exact, Literal(43));
  EXPECT_EQ(Memory.readValues(Exact).singleton(), Literal(43));
  // A captured field may be conditional on that identity's defining path.
  // Its literal numeric alias cannot use it on unrelated paths.
  EXPECT_TRUE(Memory.readValues(Literal(kHeapStart + 40)).unknown());
  Memory.writeValues(Literal(kHeapStart + 44), Literal(0), 1);
  EXPECT_TRUE(Memory.readValues(Exact).unknown());
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
    EXPECT_GT(Report->GeneralizedPublishedSummaries, 0u);
    if (!IndependentTarget)
      EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [](const auto &Edge) {
        return Edge.InputGeneralization ==
               CallXInputGeneralization::EquivalentSpecializedResults;
      }));
    checkSummaryReferences(*Report);
  }
}

TEST(SBFCallXContexts, UnreadDescriptorBytesDoNotDisplaceTypedCallerFacts) {
  for (unsigned Mode : {0u, 1u, 2u, 3u}) {
    SCOPED_TRACE(Mode);
    const bool ReadsNarrow = Mode == 2;
    const unsigned Count =
        ReadsNarrow ? 2 : kCallXContextSpecializationsPerEntry + 2;
    std::vector<test::EncodedInstruction> Text;
    const auto TargetAddress = test::loadImm64(6, 0);
    Text.insert(Text.end(), TargetAddress.begin(), TargetAddress.end());
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 6, -16));
    std::vector<size_t> Calls;
    for (unsigned I = 0; I < Count; ++I) {
      Text.push_back(encode(Opcode::ST_B_IMM, 10, 0, -64, I));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Calls.push_back(Text.size());
      Text.push_back({});
      if (!ReadsNarrow)
        Text.push_back(encode(Opcode::LD_B_REG, 0, 10, -64));
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Leaf = Text.size();
    if (ReadsNarrow || Mode == 3)
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    if (Mode == 1)
      Text.push_back(encode(Opcode::ST_B_IMM, 10, 0, -8, 99));
    Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
    Text.push_back(encode(Opcode::CALL_REG, 2));
    if (ReadsNarrow)
      Text.push_back(encode(Opcode::LD_B_REG, 0, 6, -48));
    if (Mode == 3)
      Text.push_back(encode(Opcode::ST_B_IMM, 6, 0, -48, 17));
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
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
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
    EXPECT_EQ(Raw->ReturnValue, Mode == 3 ? 17 : Count - 1);
    EXPECT_TRUE(llvm::any_of(Report->Summaries, [&](const auto &Summary) {
      if (Summary.EntrySlot != 0 || Summary.GeneralizedInputs)
        return false;
      // Incoming narrow memory is intentionally projected out. A callee that
      // reads it retains only the byte-width bound, never a concrete byte
      // from an older invocation.
      if (ReadsNarrow)
        return Summary.ReturnValue == unsignedRangeValue({0, UINT8_MAX});
      return Summary.ReturnValue.ValueKind == RegisterValue::Kind::Constant &&
             Summary.ReturnValue.Value == (Mode == 3 ? 17 : Count - 1);
    }));
  }
}

TEST(SBFProvenValueSet, CoveredLiteralsDoNotSplitNumericRanges) {
  const auto Range = unsignedRangeValue({kHeapStart + 8, UINT64_MAX});
  const RegisterValue Literal{RegisterValue::Kind::Constant, kHeapStart + 128,
                              0};
  for (bool Reverse : {false, true}) {
    ProvenValueSet Values(Reverse ? Literal : Range);
    Values.join(Reverse ? Range : Literal);
    EXPECT_EQ(Values.singleton(), Range);
    Values.join(Values);
    EXPECT_EQ(Values.singleton(), Range);
    Values.join(RegisterValue{RegisterValue::Kind::Constant, 0, 0});
    EXPECT_EQ(Values.values().size(), 2u);
    Values.join(
        RegisterValue{RegisterValue::Kind::StackAddress, kHeapStart, 0});
    Values.join(RegisterValue{RegisterValue::Kind::SymbolicAddress, 0, 0});
    EXPECT_EQ(Values.values().size(), kCallXContextValueAlternatives);
    Values.join(RegisterValue{RegisterValue::Kind::Constant, 7, 0});
    EXPECT_TRUE(Values.unknown());
  }
  ProvenValueSet Values(RegisterValue{RegisterValue::Kind::Constant, 0, 0});
  for (unsigned I = 1; I < kCallXContextValueAlternatives; ++I)
    Values.join(
        RegisterValue{RegisterValue::Kind::Constant, kHeapStart + I * 16, 0});
  Values.join(Range);
  EXPECT_EQ(Values.values().size(), 2u);
  const auto Wrapped = unsignedRangeValue({UINT64_MAX - 7, 7});
  Values = Wrapped;
  Values.join(RegisterValue{RegisterValue::Kind::Constant, 0, 0});
  Values.join(RegisterValue{RegisterValue::Kind::Constant, UINT64_MAX, 0});
  EXPECT_EQ(Values.singleton(), Wrapped);
  Values.join(RegisterValue{RegisterValue::Kind::Constant, 8, 0});
  EXPECT_EQ(Values.values().size(), 2u);
  Values.join(ProvenValueSet{});
  EXPECT_TRUE(Values.unknown());
}

TEST(SBFProvenValueSet, CoveredRangesKeepCompleteNumericUnions) {
  const RegisterValue Zero{RegisterValue::Kind::Constant, 0, 0};
  SymbolicAddressSpace Addresses;
  for (unsigned I = 0; I != 5; ++I)
    Addresses.push_back({kHeapStart + 8 + I * 16, kHeapStart + 256});
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    ProvenValueSet Values(Zero);
    for (unsigned I = 0; I != Addresses.size(); ++I)
      Values.join(RegisterValue{RegisterValue::Kind::SymbolicAddress,
                                Reverse ? Addresses.size() - I - 1 : I, 0},
                  &Addresses);
    ASSERT_FALSE(Values.unknown());
    // A later capture may still keep its identity while the domain fits.
    // Adding the same numeric enclosure must remove every covered member.
    Values.join(unsignedRangeValue({kHeapStart + 8, kHeapStart + 256}),
                &Addresses);
    EXPECT_EQ(Values.values().size(), 2u);
    EXPECT_TRUE(llvm::is_contained(Values.values(), Zero));
    EXPECT_TRUE(llvm::is_contained(
        Values.values(),
        unsignedRangeValue({kHeapStart + 8, kHeapStart + 256})));
  }
  struct Case {
    UnsignedValueRange Outer;
    UnsignedValueRange Inner;
    size_t Alternatives;
  };
  for (const auto &C :
       {Case{{16, 48}, {24, 32}, 1}, Case{{16, 48}, {32, 64}, 2},
        Case{{16, 48}, {16, 48}, 1}, Case{{UINT64_MAX - 15, 15}, {0, 7}, 1},
        Case{{UINT64_MAX - 15, 15}, {UINT64_MAX - 7, UINT64_MAX}, 1},
        Case{{UINT64_MAX - 15, 15}, {UINT64_MAX - 7, 7}, 1},
        Case{{1, UINT64_MAX - 1}, {UINT64_MAX - 7, 7}, 2},
        Case{{UINT64_MAX - 15, 15}, {8, UINT64_MAX - 8}, 2}})
    for (bool Capture : {false, true})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << C.Outer.Lower << ", " << C.Outer.Upper << "; "
                     << C.Inner.Lower << ", " << C.Inner.Upper
                     << ", capture=" << Capture << ", reverse=" << Reverse);
        SymbolicAddressSpace Bounds{C.Inner};
        const auto A = unsignedRangeValue(C.Outer);
        const auto B =
            Capture ? RegisterValue{RegisterValue::Kind::SymbolicAddress, 0, 0}
                    : unsignedRangeValue(C.Inner);
        ProvenValueSet Values(Reverse ? B : A);
        Values.join(Reverse ? A : B, &Bounds);
        ASSERT_FALSE(Values.unknown());
        EXPECT_EQ(Values.values().size(), C.Alternatives);
        const auto Before = Values;
        Values.join(Values, &Bounds);
        EXPECT_EQ(Values, Before);
        const auto Includes = [](UnsignedValueRange Range, uint64_t V) {
          return Range.Lower <= Range.Upper
                     ? Range.Lower <= V && V <= Range.Upper
                     : Range.Lower <= V || V <= Range.Upper;
        };
        for (uint64_t V :
             {uint64_t{0}, uint64_t{1}, uint64_t{7}, uint64_t{8}, uint64_t{15},
              uint64_t{16}, uint64_t{23}, uint64_t{24}, uint64_t{32},
              uint64_t{48}, uint64_t{49}, uint64_t{63}, uint64_t{64},
              UINT64_MAX - 16, UINT64_MAX - 8, UINT64_MAX - 7, UINT64_MAX}) {
          const bool Expected = Includes(C.Outer, V) || Includes(C.Inner, V);
          EXPECT_EQ(llvm::any_of(Values.values(),
                                 [&](const auto &Value) {
                                   return Includes(
                                       *unsignedValueRange(Value, &Bounds), V);
                                 }),
                    Expected)
              << V;
        }
      }
}

TEST(SBFCallXContexts, CoveredAllocatorLiteralKeepsOneReturnedPointer) {
  std::vector<test::EncodedInstruction> Text;
  const auto Literal = [&](unsigned R, uint64_t Value) {
    const auto Slots = test::loadImm64(R, Value);
    const size_t At = Text.size();
    Text.insert(Text.end(), Slots.begin(), Slots.end());
    return At;
  };
  Literal(1, kInputStart);
  const size_t Call = Text.size();
  Text.push_back({});
  const size_t Null = Text.size();
  Text.push_back({});
  Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
  const size_t TargetLiteral = Literal(2, 0);
  Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
  Text.push_back(encode(Opcode::LD_DW_REG, 5, 6, 8));
  const size_t CallX = Text.size();
  Text.push_back(encode(Opcode::CALL_REG, 5));
  Text.push_back(encode(Opcode::EXIT));
  const size_t Error = Text.size();
  Text.push_back(encode(Opcode::MOV64_IMM, 0));
  Text.push_back(encode(Opcode::EXIT));
  const size_t Allocate = Text.size();
  Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
  const size_t Default = Text.size();
  Text.push_back({});
  Literal(2, kHeapStart + 8);
  Text.push_back(encode(Opcode::JGE64_REG, 0, 2, 1));
  Text.push_back(encode(Opcode::MOV64_IMM, 0));
  Text.push_back(encode(Opcode::EXIT));
  const size_t DefaultValue = Literal(0, kHeapStart + 128);
  Text.push_back(encode(Opcode::EXIT));
  const size_t Target = Text.size();
  Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
  Text.push_back(encode(Opcode::EXIT));
  Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Allocate - Call - 1);
  Text[Null] = encode(Opcode::JEQ64_IMM, 0, 0, Error - Null - 1);
  Text[Default] = encode(Opcode::JEQ64_IMM, 0, 0, DefaultValue - Default - 1);
  const auto TargetAddress = test::loadImm64(2, kBytecodeStart + Target * 8);
  std::copy(TargetAddress.begin(), TargetAddress.end(),
            Text.begin() + TargetLiteral);
  auto Program = analyze(test::makeImage(Text, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  auto Report = recoverCallXContexts(*Program);
  ASSERT_TRUE(static_cast<bool>(Report)) << llvm::toString(Report.takeError());
  EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
    return Site.CallXSlot == CallX && Site.TargetSlot == Target;
  }));
  for (uint64_t Cursor : {uint64_t{0}, kHeapStart + 256, uint64_t{7}}) {
    SCOPED_TRACE(Cursor);
    ExecutionEnvironment Environment;
    std::vector<uint8_t> Input(sizeof(uint64_t));
    llvm::support::endian::write64le(Input.data(), Cursor);
    Environment.Memory.push_back(
        {kInputStart, Input, false, "allocation input"});
    Environment.Memory.push_back(
        {kHeapStart, std::vector<uint8_t>(512), true, "heap"});
    auto Raw = executeRaw(*Program, std::move(Environment));
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, Cursor == 7 ? 0u : 42u);
    EXPECT_EQ(
        llvm::any_of(Raw->Trace,
                     [&](const auto &Step) { return Step.Slot == Target; }),
        Cursor != 7);
  }
}

TEST(SBFCallXContexts, NonReturningCallsExcludeOnlyTheirDeadContinuations) {
  for (unsigned Mode : {0u, 1u, 2u, 3u}) {
    SCOPED_TRACE(Mode);
    std::vector<test::EncodedInstruction> Text;
    const auto Literal = [&](unsigned Register, uint64_t Value) {
      const size_t Slot = Text.size();
      const auto Words = test::loadImm64(Register, Value);
      Text.insert(Text.end(), Words.begin(), Words.end());
      return Slot;
    };
    Literal(8, kInputStart);
    Literal(3, kInputStart);
    const size_t TargetLiteral = Literal(4, 0);
    Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, 91));
    const size_t RootCall = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::EXIT));
    const size_t Wrapper = Text.size();
    const size_t TerminalCall = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::LD_DW_REG, 5, 8));
    const size_t DeadCallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 5));
    Text.push_back(encode(Opcode::MOV64_REG, 0, 9));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Terminal = Text.size();
    if (Mode == 1)
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 3));
    const size_t LiveCallX = Text.size();
    if (Mode == 2)
      Text.push_back(encode(Opcode::CALL_REG, 4));
    Text.push_back(Mode == 3 ? encode(Opcode::EXIT)
                             : encode(Opcode::JA, 0, 0, -1));
    const size_t Target = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    Text[RootCall] = encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - RootCall - 1);
    Text[TerminalCall] =
        encode(Opcode::CALL_IMM, 0, 1, 0, Terminal - TerminalCall - 1);
    const auto TargetAddress = test::loadImm64(4, kBytecodeStart + Target * 8);
    std::copy(TargetAddress.begin(), TargetAddress.end(),
              Text.begin() + TargetLiteral);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    const auto Summary = llvm::find_if(Report->Summaries, [&](const auto &S) {
      return S.EntrySlot == Wrapper && S.GeneralizedInputs;
    });
    ASSERT_NE(Summary, Report->Summaries.end());
    EXPECT_EQ(bool(Summary->InputRegisterMask & (uint16_t{1} << 8)), Mode >= 2);
    EXPECT_EQ(bool(Summary->InputRegisterMask & (uint16_t{1} << 9)), Mode >= 2);
    EXPECT_EQ(Summary->ReadsInputMemory, Mode != 0);
    if (Mode == 1)
      EXPECT_TRUE(Summary->InputRegisterMask & (uint16_t{1} << 3));
    if (Mode == 2)
      EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
        return Site.CallXSlot == LiveCallX && Site.TargetSlot == Target;
      }));
    ExecutionEnvironment Environment;
    std::vector<uint8_t> Input(8);
    llvm::support::endian::write64le(Input.data(), kBytecodeStart + Target * 8);
    Environment.Memory.push_back({kInputStart, Input, false, "target"});
    InterpreterOptions Options;
    Options.MaxSteps = 100;
    auto Raw = executeRaw(*Program, std::move(Environment), Options);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, Mode == 3 ? ExecutionStatus::Returned
                                     : ExecutionStatus::StepLimit)
        << Raw->Error;
    EXPECT_EQ(
        llvm::any_of(Raw->Trace,
                     [&](const auto &Step) { return Step.Slot == DeadCallX; }),
        Mode == 3);
    if (Mode == 3)
      EXPECT_EQ(Raw->ReturnValue, 91u);
  }
}

TEST(SBFCallXContexts,
     EqualFiniteResultsRetainCallersAcrossFurtherImageInputs) {
  for (bool DistinctResults : {false, true})
    for (bool KnownField : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "distinct=" << DistinctResults
                                        << ", field=" << KnownField);
      std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
      const auto Literal = [&](unsigned Register, uint64_t Value) {
        const auto Words = test::loadImm64(Register, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
      };
      const size_t Count = kCallXContextSpecializationsPerEntry + 2;
      std::vector<size_t> Entries, Calls;
      for (size_t I = 0; I < Count; ++I) {
        Entries.push_back(Text.size());
        Literal(1, kRodataStartV3 + I * 16);
        Literal(4, kInputStart);
        if (I + 1 == Count && KnownField)
          Literal(2, kRodataStartV3 + Count * 16);
        else
          Text.push_back(encode(Opcode::LD_DW_REG, 2, 4, 16));
        Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -56));
        Text.push_back(encode(Opcode::MOV64_IMM, 2));
        Text.push_back(encode(Opcode::MOV64_REG, 7, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 7, 0, 0, -96));
        Calls.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Wrapper = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 6, 4));
      Text.push_back(encode(Opcode::LD_DW_REG, 8, 1, 8));
      Text.push_back(encode(Opcode::LD_DW_REG, 9, 1));
      Text.push_back(encode(Opcode::JEQ64_IMM, 9, 0, 1));
      const size_t ChildCall = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_DW_REG, 3, 6));
      Text.push_back(encode(Opcode::ST_DW_REG, 3, 8));
      Text.push_back(encode(Opcode::LD_DW_REG, 3, 6, 8));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::JEQ64_IMM, 3, 0, 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 1));
      Text.push_back(encode(Opcode::ADD64_REG, 0, 8));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Child = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 1, 7, 40));
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 1, 24));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 5));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Target = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      for (size_t Call : Calls)
        Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Call - 1);
      Text[ChildCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Child - ChildCall - 1);
      std::vector<uint8_t> Table(Count * 16 + 32);
      for (size_t I = 0; I < Count; ++I) {
        llvm::support::endian::write64le(Table.data() + I * 16, I + 1 == Count);
        llvm::support::endian::write64le(Table.data() + I * 16 + 8,
                                         DistinctResults ? I * 2 : 0);
      }
      llvm::support::endian::write64le(Table.data() + Count * 16 + 24,
                                       kBytecodeStart + Target * 8);
      auto Image = test::makeImage(Text, Table);
      for (size_t Entry : Entries) {
        Symbol Function;
        Function.Name = "finite_caller_" + std::to_string(Entry);
        Function.Addr = kBytecodeStart + Entry * kInstructionSize;
        Function.IsFunc = true;
        Image.Symbols.push_back(std::move(Function));
      }
      auto Program = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_EQ(llvm::any_of(Report->SummaryEdges,
                             [&](const auto &Edge) {
                               return Edge.CallerSlot == Calls.back() &&
                                      !Edge.GeneralizedInputs;
                             }),
                !DistinctResults);
      EXPECT_EQ(llvm::any_of(Report->Contexts,
                             [&](const auto &Site) {
                               return Site.CallerSlot == ChildCall &&
                                      Site.CallXSlot == CallX &&
                                      Site.TargetSlot == Target;
                             }),
                !DistinctResults && KnownField);
      EXPECT_LE(llvm::count_if(Report->Summaries,
                               [&](const auto &Summary) {
                                 return Summary.EntrySlot == Wrapper &&
                                        !Summary.GeneralizedInputs;
                               }),
                kCallXContextSpecializationsPerEntry);
      // Every retained completed caller must still see its own finite return
      // set, including through rewritten edges. Distinct results cannot share
      // a summary even when both scalar singleton views are unknown.
      for (size_t I = 0; I < Count; ++I) {
        if (I < kCallXContextSpecializationsPerEntry || !DistinctResults) {
          const auto Edge =
              llvm::find_if(Report->SummaryEdges, [&](const auto &E) {
                return E.CallerSlot == Calls[I] && !E.GeneralizedInputs;
              });
          ASSERT_NE(Edge, Report->SummaryEdges.end());
          const auto Summary =
              llvm::find_if(Report->Summaries, [&](const auto &S) {
                return Edge->CalleeSummaryID == S.ID;
              });
          ASSERT_NE(Summary, Report->Summaries.end());
          std::set<uint64_t> Values;
          for (const auto &V : Summary->ReturnAlternatives.values()) {
            EXPECT_EQ(V.ValueKind, RegisterValue::Kind::Constant);
            Values.insert(V.Value);
          }
          const uint64_t Bias = DistinctResults ? I * 2 : 0;
          EXPECT_EQ(Values, (std::set<uint64_t>{Bias, Bias + 1}));
        }
        for (uint64_t Selector : {0u, 1u}) {
          Image.Entry = kBytecodeStart + Entries[I] * kInstructionSize;
          auto Selected = analyze(Image);
          ASSERT_TRUE(static_cast<bool>(Selected))
              << llvm::toString(Selected.takeError());
          ExecutionEnvironment Environment;
          std::vector<uint8_t> Input(24);
          llvm::support::endian::write64le(Input.data(), kHeapStart);
          llvm::support::endian::write64le(Input.data() + 8, Selector);
          llvm::support::endian::write64le(Input.data() + 16,
                                           kRodataStartV3 + Count * 16);
          Environment.Memory.push_back({kInputStart, Input, false, "inputs"});
          Environment.Memory.push_back(
              {kHeapStart, std::vector<uint8_t>(8), true, "output"});
          auto Raw = executeRaw(*Selected, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, (DistinctResults ? I * 2 : 0) + Selector);
          EXPECT_EQ(llvm::any_of(
                        Raw->Trace,
                        [&](const auto &Step) { return Step.Slot == Target; }),
                    I + 1 == Count);
        }
      }
    }
}

TEST(SBFCallXContexts, CompletedObjectResultsKeepCallerCaptureBindings) {
  for (unsigned Mode : {0u, 1u, 2u}) {
    SCOPED_TRACE(Mode);
    const bool DifferentFields = Mode == 1;
    const bool DifferentSource = Mode == 2;
    const bool Equivalent = Mode == 0;
    std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
    const auto Literal = [&](unsigned R, uint64_t Value) {
      const size_t Slot = Text.size();
      const auto Words = test::loadImm64(R, Value);
      Text.insert(Text.end(), Words.begin(), Words.end());
      return Slot;
    };
    constexpr unsigned Count = kCallXContextSpecializationsPerEntry + 3;
    std::vector<size_t> Entries, Calls, Sites;
    for (unsigned I = 0; I < Count; ++I) {
      Entries.push_back(Text.size());
      Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -16,
                            DifferentFields || DifferentSource ? I + 1 : 2));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -48 - I * 16));
      Text.push_back(
          encode(Opcode::MOV64_IMM, 2, 0, 0,
                 I < kCallXContextSpecializationsPerEntry
                     ? 1
                     : I - kCallXContextSpecializationsPerEntry + 2));
      Text.push_back(encode(Opcode::MOV64_REG, 3, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 3, 0, 0, -16));
      Calls.push_back(Text.size());
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_DW_REG, 1, 10, -40 - I * 16));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 1, 24));
      Sites.push_back(Text.size());
      Text.push_back(encode(Opcode::CALL_REG, 2));
      Text.push_back(encode(Opcode::EXIT));
    }
    const size_t Constructor = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    Text.push_back(encode(Opcode::MOV64_REG, 7, 2));
    Text.push_back(encode(Opcode::LD_DW_REG, 9, 3));
    Literal(4, kInputStart);
    Text.push_back(encode(Opcode::LD_DW_REG, 3, 4, 8));
    Text.push_back(encode(Opcode::ST_DW_IMM, 3));
    Text.push_back(encode(Opcode::MOV64_REG, 1, 4));
    std::vector<size_t> AllocationBranches, AllocationCalls, AllocationJoins;
    if (DifferentSource)
      for (unsigned I = 0; I < 3; ++I) {
        AllocationBranches.push_back(Text.size());
        Text.push_back({});
      }
    for (unsigned I = 0; I < (DifferentSource ? 4 : 1); ++I) {
      AllocationCalls.push_back(Text.size());
      Text.push_back({});
      AllocationJoins.push_back(Text.size());
      Text.push_back({});
    }
    const size_t AllocationJoin = Text.size();
    for (size_t I = 0; I < AllocationBranches.size(); ++I)
      Text[AllocationBranches[I]] =
          encode(Opcode::JEQ64_IMM, 9, 0,
                 AllocationCalls[I + 1] - AllocationBranches[I] - 1, I + 1);
    for (size_t At : AllocationJoins)
      Text[At] = encode(Opcode::JA, 0, 0, AllocationJoin - At - 1);
    Text.push_back(encode(Opcode::MOV64_REG, 8, 0));
    Text.push_back(encode(Opcode::ST_DW_IMM, 8, 0, 0, 2));
    Text.push_back(DifferentSource ? encode(Opcode::ST_B_IMM, 8, 0, 8, 2)
                                   : encode(Opcode::ST_B_REG, 8, 9, 8));
    Text.push_back(encode(Opcode::ST_W_REG, 8, 7, 16));
    const size_t FirstAddress = Literal(2, 0);
    const size_t Select = Text.size();
    Text.push_back({});
    const size_t SecondAddress = Literal(2, 0);
    const size_t StoreTarget = Text.size();
    Text.push_back(encode(Opcode::ST_DW_REG, 8, 2, 24));
    Text.push_back(encode(Opcode::ST_DW_IMM, 6));
    Text.push_back(encode(Opcode::ST_DW_REG, 6, 8, 8));
    Text.push_back(encode(Opcode::MOV64_REG, 0, 8));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Allocate = Text.size();
    Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
    Literal(2, kHeapStart + 8);
    const size_t Below = Text.size();
    Text.push_back({});
    Literal(2, kHeapStart + 128);
    const size_t Above = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::EXIT));
    const size_t Fallback = Literal(0, kHeapStart + 32);
    Text.push_back(encode(Opcode::EXIT));
    const size_t A = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    const size_t B = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
    Text.push_back(encode(Opcode::EXIT));
    for (size_t Call : Calls)
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Constructor - Call - 1);
    for (size_t At : AllocationCalls)
      Text[At] = encode(Opcode::CALL_IMM, 0, 1, 0, Allocate - At - 1);
    Text[Select] = encode(Opcode::JEQ64_IMM, 7, 0, StoreTarget - Select - 1, 1);
    Text[Below] = encode(Opcode::JLT64_REG, 0, 2, Fallback - Below - 1);
    Text[Above] = encode(Opcode::JGT64_REG, 0, 2, Fallback - Above - 1);
    for (auto [At, Target] :
         {std::pair{FirstAddress, A}, std::pair{SecondAddress, B}}) {
      const auto Words = test::loadImm64(2, kBytecodeStart + Target * 8);
      std::copy(Words.begin(), Words.end(), Text.begin() + At);
    }
    // Recognize each disconnected entry as a function with an automatic
    // frame anchor. These later call roots do not seed its memory facts.
    for (size_t Entry : Entries) {
      const size_t Call = Text.size();
      Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, Entry - Call - 1));
      Text.push_back(encode(Opcode::EXIT));
    }
    auto Image = test::makeImage(Text, {});
    auto Program = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_FALSE(Report->SpecializationBudgetExhausted);
    size_t BoundCalls = 0;
    std::set<size_t> OriginalCaptures;
    for (const auto &Edge : Report->SummaryEdges) {
      if (!Edge.CompletedResultBinding)
        continue;
      const auto It = llvm::find(Calls, Edge.CallerSlot);
      ASSERT_NE(It, Calls.end());
      const unsigned I = It - Calls.begin();
      const auto &Binding = *Edge.CompletedResultBinding;
      EXPECT_EQ(Edge.InputGeneralization,
                CallXInputGeneralization::EquivalentSpecializedResults);
      EXPECT_EQ(Binding.FrameOffset,
                (kCallXContextSpecializationsPerEntry - I - 1) * 16);
      EXPECT_NE(Binding.SummaryCapture, Binding.OriginalCapture);
      EXPECT_LT(Binding.SummaryCapture, Report->CapturedAddresses.size());
      EXPECT_LT(Binding.OriginalCapture, Report->CapturedAddresses.size());
      EXPECT_GT(Edge.AppliedStoredValues, 0u);
      OriginalCaptures.insert(Binding.OriginalCapture);
      ++BoundCalls;
    }
    EXPECT_EQ(BoundCalls, Equivalent ? 3u : 0u);
    EXPECT_EQ(OriginalCaptures.size(), BoundCalls);
    EXPECT_LE(llvm::count_if(Report->Summaries,
                             [&](const auto &S) {
                               return S.EntrySlot == Constructor &&
                                      !S.GeneralizedInputs;
                             }),
              kCallXContextSpecializationsPerEntry);
    for (unsigned I = 0; I < Count; ++I) {
      const size_t Target = I < kCallXContextSpecializationsPerEntry ? A : B;
      const bool Proven = llvm::any_of(Report->Contexts, [&](const auto &Site) {
        return Site.CallXSlot == Sites[I] && Site.TargetSlot == Target;
      });
      EXPECT_EQ(Proven, Equivalent || I < kCallXContextSpecializationsPerEntry)
          << "caller " << I;
      Image.Entry = kBytecodeStart + Entries[I] * kInstructionSize;
      auto Selected = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Selected))
          << llvm::toString(Selected.takeError());
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input(16);
      llvm::support::endian::write64le(Input.data(), kHeapStart + 32);
      llvm::support::endian::write64le(Input.data() + 8, kHeapStart + 176);
      Environment.Memory.push_back({kInputStart, Input, false, "input"});
      Environment.Memory.push_back(
          {kHeapStart, std::vector<uint8_t>(256), true, "heap"});
      auto Raw = executeRaw(*Selected, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue,
                I < kCallXContextSpecializationsPerEntry ? 42u : 43u);
      EXPECT_TRUE(llvm::any_of(
          Raw->Trace, [&](const auto &Step) { return Step.Slot == Target; }));
    }
  }
}

TEST(SBFCallXContexts, NewCapturedInputCanUseEquivalentCompletedResults) {
  for (bool CapturedInput : {false, true})
    for (bool OpaqueReturn : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "capture=" << CapturedInput
                                        << ", opaque=" << OpaqueReturn);
      std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
      const auto Literal = [&](unsigned Register, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(Register, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      std::vector<size_t> Entries, Calls, Targets;
      std::optional<size_t> AllocationCall;
      for (size_t I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
        Entries.push_back(Text.size());
        if (CapturedInput && I == kCallXContextSpecializationsPerEntry) {
          Literal(1, kInputStart);
          AllocationCall = Text.size();
          Text.push_back({});
          Text.push_back(encode(Opcode::MOV64_REG, 9, 0));
        } else {
          Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
        }
        Targets.push_back(Literal(2, 0));
        // Each disconnected entry establishes its own backing address. An
        // unknown entry frame must not reduce this to expression-only proof.
        Literal(1, kHeapStart + 96);
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 2));
        Literal(4, kInputStart);
        Calls.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Leaf = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 6, 9));
      Text.push_back(encode(Opcode::MOV64_REG, 7, 4));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 2));
      Text.push_back(encode(Opcode::LD_DW_REG, 3, 7));
      Text.push_back(encode(Opcode::ST_DW_IMM, 3));
      if (OpaqueReturn)
        Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t FirstTarget = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t SecondTarget = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      if (AllocationCall) {
        const size_t Allocate = Text.size();
        Text[*AllocationCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Allocate - *AllocationCall - 1);
        Text.push_back(encode(Opcode::LD_DW_REG, 0, 1, 8));
        Literal(2, kHeapStart + 8);
        Text.push_back(encode(Opcode::JGE64_REG, 0, 2, 2));
        Literal(0, kHeapStart + 64);
        Text.push_back(encode(Opcode::EXIT));
      }
      for (size_t I = 0; I < Calls.size(); ++I) {
        Text[Calls[I]] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Calls[I] - 1);
        const size_t Target =
            I + 1 == Calls.size() ? SecondTarget : FirstTarget;
        const auto Words = test::loadImm64(2, kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Targets[I]);
      }
      auto Image = test::makeImage(Text, {});
      auto Program = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      const bool Coalesced = CapturedInput;
      EXPECT_EQ(llvm::any_of(Report->SummaryEdges,
                             [&](const auto &Edge) {
                               return Edge.CallerSlot == Calls.back() &&
                                      !Edge.GeneralizedInputs;
                             }),
                Coalesced);
      EXPECT_EQ(
          llvm::any_of(
              Report->SummaryEdges,
              [](const auto &Edge) {
                return Edge.InputGeneralization ==
                       CallXInputGeneralization::EquivalentSpecializedResults;
              }),
          Coalesced);
      EXPECT_LE(llvm::count_if(Report->Summaries,
                               [&](const auto &Summary) {
                                 return Summary.EntrySlot == Leaf &&
                                        !Summary.GeneralizedInputs;
                               }),
                kCallXContextSpecializationsPerEntry);
      for (size_t I = 0; I < Calls.size(); ++I) {
        const size_t Target =
            I + 1 == Calls.size() ? SecondTarget : FirstTarget;
        EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
          return Site.CallerSlot == Calls[I] && Site.CallXSlot == CallX &&
                 Site.TargetSlot == Target;
        }));
        Image.Entry = kBytecodeStart + Entries[I] * kInstructionSize;
        auto Selected = analyze(Image);
        ASSERT_TRUE(static_cast<bool>(Selected))
            << llvm::toString(Selected.takeError());
        ExecutionEnvironment Environment;
        std::vector<uint8_t> Input(16);
        llvm::support::endian::write64le(Input.data(), kHeapStart);
        llvm::support::endian::write64le(Input.data() + 8, kHeapStart + 64);
        Environment.Memory.push_back({kInputStart, Input, false, "allocation"});
        Environment.Memory.push_back(
            {kHeapStart, std::vector<uint8_t>(128), true, "heap"});
        auto Raw = executeRaw(*Selected, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, OpaqueReturn            ? 0u
                                    : I + 1 == Calls.size() ? 43u
                                                            : 42u);
        EXPECT_TRUE(llvm::any_of(
            Raw->Trace, [&](const auto &Step) { return Step.Slot == Target; }));
      }
    }
}

TEST(SBFCallXContexts, UnreadWordCellsReuseAfterSpecializationLimit) {
  for (unsigned Mode : {0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u}) {
    SCOPED_TRACE(Mode);
    const bool ReadsOtherWord = Mode == 2;
    const bool NeedsDistinctMemory = ReadsOtherWord || Mode >= 6;
    const unsigned Count = kCallXContextSpecializationsPerEntry + 2;
    std::vector<test::EncodedInstruction> Text;
    const auto TargetAddress = test::loadImm64(6, 0);
    Text.insert(Text.end(), TargetAddress.begin(), TargetAddress.end());
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 6, -16));
    std::vector<size_t> Calls;
    for (unsigned I = 0; I < Count; ++I) {
      Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -64, I));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
      if (Mode == 7) {
        // This never-returning child publishes a concrete target value from
        // the changing word. Its reads must still guard summary reuse.
        Text.push_back(encode(Opcode::MOV64_REG, 4, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 4, 0, 0, -64));
      }
      if (Mode >= 4)
        Text.push_back(encode(Opcode::MOV64_REG, 3, 7));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Calls.push_back(Text.size());
      Text.push_back({});
      if (!ReadsOtherWord)
        Text.push_back(encode(Opcode::LD_DW_REG, 0, 10, -64));
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Leaf = Text.size();
    std::optional<size_t> PanicCall;
    if (Mode >= 4) {
      Text.push_back(encode(Opcode::JEQ64_IMM, 3, 0, 2));
      PanicCall = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
    }
    if (ReadsOtherWord || Mode == 3)
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    if (Mode == 1)
      Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -8, 99));
    Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
    Text.push_back(encode(Opcode::CALL_REG, 2));
    if (ReadsOtherWord)
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 6, -48));
    if (Mode == 3)
      Text.push_back(encode(Opcode::ST_DW_IMM, 6, 0, -48, 17));
    Text.push_back(encode(Opcode::EXIT));
    if (PanicCall) {
      const size_t Panic = Text.size();
      Text[*PanicCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Panic - *PanicCall - 1);
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 4));
      if (Mode == 5 || Mode == 7)
        Text.push_back(encode(Opcode::CALL_REG, 0));
      if (Mode == 4) {
        // The indirect call is after a structurally nonreturning child. It
        // must not make the panic subtree appear to produce target evidence.
        Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, 2));
        Text.push_back(encode(Opcode::CALL_REG, 0));
        Text.push_back(encode(Opcode::EXIT));
        Text.push_back(encode(Opcode::JA, 0, 0, -1));
      } else {
        Text.push_back(Mode == 6 ? encode(Opcode::EXIT)
                                 : encode(Opcode::JA, 0, 0, -1));
      }
    }
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
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    EXPECT_EQ(llvm::count_if(Report->Summaries,
                             [&](const auto &Summary) {
                               return Summary.EntrySlot == Leaf &&
                                      !Summary.GeneralizedInputs &&
                                      Summary.Inputs[1].ValueKind !=
                                          RegisterValue::Kind::Unknown;
                             }),
              kCallXContextSpecializationsPerEntry);
    const auto Root = llvm::find_if(Report->Summaries, [](const auto &Summary) {
      return Summary.EntrySlot == 0 && !Summary.GeneralizedInputs;
    });
    ASSERT_NE(Root, Report->Summaries.end());
    if (Mode == 5 || Mode == 7)
      EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
        return Edge.CallerSummaryID == Root->ID &&
               Edge.CallerSlot == Calls.back() &&
               Edge.CalleeEntrySlot == Leaf &&
               Edge.GeneralizedInputs == (Mode == 7);
      }));
    if (!NeedsDistinctMemory)
      for (size_t Slot : Calls)
        EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
          return Site.CallerSlot == Slot && Site.TargetSlot == Target;
        }));
    auto Raw = executeRaw(*Program);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, Mode == 3 ? 17 : Count - 1);
    EXPECT_TRUE(llvm::any_of(Report->Summaries, [&](const auto &Summary) {
      if (Summary.EntrySlot != 0 || Summary.GeneralizedInputs)
        return false;
      // Observed words may require distinct variants or a generic fallback.
      // A reused result must never retain an earlier invocation's value.
      if (NeedsDistinctMemory &&
          Summary.ReturnValue.ValueKind == RegisterValue::Kind::Unknown)
        return true;
      return Summary.ReturnValue.ValueKind == RegisterValue::Kind::Constant &&
             Summary.ReturnValue.Value == (Mode == 3 ? 17 : Count - 1);
    }));
  }
}

TEST(SBFCallXContexts, NonreturningInputsGuardOnlyRetainedEvidence) {
  for (unsigned Mode : {0u, 1u, 2u})
    for (bool ReverseEdges : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "mode=" << Mode << ", reverse=" << ReverseEdges);
      const bool Returns = Mode == 1;
      const bool ProvesTarget = Mode == 2;
      constexpr size_t Count = kCallXContextSpecializationsPerEntry + 2;
      std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
      std::vector<size_t> Entries, Literals, NormalLiterals, Calls, CallXs,
          Targets;
      for (size_t I = 0; I < Count; ++I) {
        Entries.push_back(Text.size());
        const auto Input = test::loadImm64(1, kInputStart);
        Text.insert(Text.end(), Input.begin(), Input.end());
        Text.push_back(encode(Opcode::LD_DW_REG, 3, 1));
        NormalLiterals.push_back(Text.size());
        const auto Normal = test::loadImm64(2, 0);
        Text.insert(Text.end(), Normal.begin(), Normal.end());
        Literals.push_back(Text.size());
        const auto Target = test::loadImm64(9, 0);
        Text.insert(Text.end(), Target.begin(), Target.end());
        Calls.push_back(Text.size());
        Text.push_back({});
        CallXs.push_back(Text.size());
        Text.push_back(encode(Opcode::CALL_REG, 0));
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Wrapper = Text.size();
      Text.push_back(encode(Opcode::JEQ64_IMM, 3, 0, 2));
      const size_t ChildCall = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
      Text.push_back(encode(Opcode::MOV64_REG, 0, 2));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Child = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 0, 9));
      const size_t ChildCallX = Text.size();
      if (ProvesTarget)
        Text.push_back(encode(Opcode::CALL_REG, 0));
      Text.push_back(Returns ? encode(Opcode::EXIT)
                             : encode(Opcode::JA, 0, 0, -1));
      for (size_t I = 0; I < Count; ++I) {
        Targets.push_back(Text.size());
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42 + I));
        Text.push_back(encode(Opcode::EXIT));
      }
      Text[ChildCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Child - ChildCall - 1);
      const auto NormalTarget =
          test::loadImm64(2, kBytecodeStart + Targets[0] * kInstructionSize);
      for (size_t I = 0; I < Count; ++I) {
        Text[Calls[I]] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
        std::copy(NormalTarget.begin(), NormalTarget.end(),
                  Text.begin() + NormalLiterals[I]);
        const auto Target =
            test::loadImm64(9, kBytecodeStart + Targets[I] * kInstructionSize);
        std::copy(Target.begin(), Target.end(), Text.begin() + Literals[I]);
      }
      auto Image = test::makeImage(Text, {});
      for (size_t Entry : Entries) {
        Symbol Function;
        Function.Name = "caller_" + std::to_string(Entry);
        Function.Addr = kBytecodeStart + Entry * kInstructionSize;
        Function.IsFunc = true;
        Image.Symbols.push_back(std::move(Function));
      }
      auto Program = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (ReverseEdges)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      const auto LastRoot =
          llvm::find_if(Report->Summaries, [&](const auto &S) {
            return S.EntrySlot == Entries.back() && !S.GeneralizedInputs;
          });
      ASSERT_NE(LastRoot, Report->Summaries.end());
      for (const auto &Summary : Report->Summaries)
        if (Summary.EntrySlot == Wrapper && !Summary.GeneralizedInputs)
          EXPECT_EQ(bool(Summary.ReuseInputRegisterMask & (uint16_t{1} << 9)),
                    Returns || ProvesTarget);
      bool SawLast = false;
      for (const auto &Edge : Report->SummaryEdges)
        if (Edge.CallerSlot == Calls.back() &&
            Edge.CallerSummaryID == LastRoot->ID) {
          SawLast = true;
          EXPECT_EQ(Edge.InputGeneralization ==
                        CallXInputGeneralization::EquivalentSpecializedInputs,
                    !Returns && !ProvesTarget);
        }
      EXPECT_TRUE(SawLast);
      if (!Returns)
        for (size_t CallX : CallXs)
          EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
            return Site.CallXSlot == CallX && Site.TargetSlot == Targets[0];
          }));
      for (size_t I = 0; I < Count; ++I) {
        Image.Entry = kBytecodeStart + Entries[I] * kInstructionSize;
        auto Selected = analyze(Image);
        ASSERT_TRUE(static_cast<bool>(Selected))
            << llvm::toString(Selected.takeError());
        for (size_t Selector : {0u, 1u}) {
          InterpreterOptions Options;
          Options.MaxSteps = 100;
          auto Raw = executeSelector(*Selected, Selector, Options);
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          const bool Loops = Selector && !Returns;
          EXPECT_EQ(Raw->Status, Loops ? ExecutionStatus::StepLimit
                                       : ExecutionStatus::Returned)
              << Raw->Error;
          if (!Loops)
            EXPECT_EQ(Raw->ReturnValue, Selector ? 42 + I : 42);
          EXPECT_EQ(llvm::count_if(Raw->Trace,
                                   [&](const auto &Step) {
                                     return Step.Slot == CallXs[I] ||
                                            (ProvesTarget &&
                                             Step.Slot == ChildCallX);
                                   }),
                    !Selector || Returns || ProvesTarget ? 1 : 0);
        }
      }
    }
}

TEST(SBFCallXContexts, NestedReadFootprintsKeepObservedWordsDistinct) {
  for (bool ReadsChangingWord : {false, true}) {
    SCOPED_TRACE(ReadsChangingWord);
    constexpr unsigned Count = kCallXContextSpecializationsPerEntry + 2;
    std::vector<test::EncodedInstruction> Text;
    const auto Placeholder = test::loadImm64(6, 0);
    Text.insert(Text.end(), Placeholder.begin(), Placeholder.end());
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 6, -16));
    std::vector<size_t> Calls;
    for (unsigned I = 0; I < Count; ++I) {
      Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -64, I));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Calls.push_back(Text.size());
      Text.push_back({});
      if (!ReadsChangingWord)
        Text.push_back(encode(Opcode::LD_DW_REG, 0, 10, -64));
    }
    Text.push_back(encode(Opcode::EXIT));
    const size_t Wrapper = Text.size();
    Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, 1));
    Text.push_back(encode(Opcode::EXIT));
    Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 2));
    if (ReadsChangingWord)
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 6, -48));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Target = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    const auto Literal = test::loadImm64(6, kBytecodeStart + Target * 8);
    std::copy(Literal.begin(), Literal.end(), Text.begin());
    for (size_t Slot : Calls)
      Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Slot - 1);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
      return Edge.CallerSlot == Calls.back() &&
             Edge.CalleeEntrySlot == Wrapper &&
             Edge.GeneralizedInputs == ReadsChangingWord &&
             Edge.InputGeneralization ==
                 (ReadsChangingWord
                      ? CallXInputGeneralization::SpecializationLimit
                      : CallXInputGeneralization::EquivalentSpecializedInputs);
    }));
    EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
      return Site.CallXSlot == CallX && Site.TargetSlot == Target;
    }));
    auto Raw = executeRaw(*Program);
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, Count - 1);
    EXPECT_TRUE(llvm::any_of(Report->Summaries, [&](const auto &Summary) {
      if (Summary.EntrySlot != 0 || Summary.GeneralizedInputs)
        return false;
      if (ReadsChangingWord &&
          Summary.ReturnValue.ValueKind == RegisterValue::Kind::Unknown)
        return true;
      return Summary.ReturnValue.ValueKind == RegisterValue::Kind::Constant &&
             Summary.ReturnValue.Value == Count - 1;
    }));
  }
}

TEST(SBFCallXContexts, RepeatedFrameBindingsCanMakeRoomForANewDescriptor) {
  for (bool NewFrameBinding : {false, true})
    for (bool OpaqueReturn : {false, true})
      for (bool MixedRetainedBindings : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << "frame=" << NewFrameBinding << ", opaque="
                     << OpaqueReturn << ", mixed=" << MixedRetainedBindings);
        std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
        std::vector<size_t> Entries, Calls, Literals;
        for (size_t I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
          Entries.push_back(Text.size());
          Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
          Literals.push_back(Text.size());
          const auto Target = test::loadImm64(2, 0);
          Text.insert(Text.end(), Target.begin(), Target.end());
          const int16_t Offset =
              MixedRetainedBindings && I == 1 ? -24
              : NewFrameBinding && I == kCallXContextSpecializationsPerEntry
                  ? -32
                  : -16;
          Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, Offset));
          Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, Offset));
          const auto Input = test::loadImm64(4, kInputStart);
          Text.insert(Text.end(), Input.begin(), Input.end());
          Calls.push_back(Text.size());
          Text.push_back({});
          Text.push_back(encode(Opcode::EXIT));
        }
        const size_t Wrapper = Text.size();
        Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, 1));
        Text.push_back(encode(Opcode::EXIT));
        Text.push_back(encode(Opcode::MOV64_REG, 6, 9));
        Text.push_back(encode(Opcode::MOV64_REG, 7, 4));
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
        const size_t CallX = Text.size();
        Text.push_back(encode(Opcode::CALL_REG, 2));
        Text.push_back(encode(Opcode::LD_DW_REG, 3, 7));
        Text.push_back(encode(Opcode::ST_DW_IMM, 3));
        if (OpaqueReturn)
          Text.push_back(encode(Opcode::MOV64_IMM, 0));
        Text.push_back(encode(Opcode::EXIT));
        const size_t FirstTarget = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
        Text.push_back(encode(Opcode::EXIT));
        const size_t SecondTarget = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
        Text.push_back(encode(Opcode::EXIT));
        for (size_t I = 0; I < Calls.size(); ++I) {
          Text[Calls[I]] =
              encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
          const auto Target = test::loadImm64(
              2, kBytecodeStart +
                     (I + 1 == Calls.size() ? SecondTarget : FirstTarget) * 8);
          std::copy(Target.begin(), Target.end(), Text.begin() + Literals[I]);
        }
        auto Image = test::makeImage(Text, {});
        // Named function entries receive the VM's canonical frame seed. Other
        // disconnected blocks deliberately retain unknown entry registers.
        for (size_t Entry : Entries) {
          Symbol Function;
          Function.Name = "caller_" + std::to_string(Entry);
          Function.Addr = kBytecodeStart + Entry * kInstructionSize;
          Function.IsFunc = true;
          Image.Symbols.push_back(std::move(Function));
        }
        auto Program = analyze(Image);
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        checkSummaryReferences(*Report);
        const bool Replaced = NewFrameBinding && !MixedRetainedBindings;
        EXPECT_EQ(Report->GeneralizedPublishedSummaries > 0, Replaced);
        EXPECT_LE(llvm::count_if(Report->Summaries,
                                 [&](const auto &Summary) {
                                   return Summary.EntrySlot == Wrapper &&
                                          !Summary.GeneralizedInputs;
                                 }),
                  kCallXContextSpecializationsPerEntry);
        EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
          return Site.CallXSlot == CallX && Site.TargetSlot == FirstTarget;
        }));
        EXPECT_EQ(llvm::any_of(Report->Contexts,
                               [&](const auto &Site) {
                                 return Site.CallXSlot == CallX &&
                                        Site.TargetSlot == SecondTarget;
                               }),
                  Replaced);
        EXPECT_EQ(llvm::any_of(Report->SummaryEdges,
                               [&](const auto &Edge) {
                                 return Edge.CallerSlot == Calls.back() &&
                                        !Edge.GeneralizedInputs;
                               }),
                  Replaced);
        for (size_t I = 0; I < Entries.size(); ++I) {
          Image.Entry = kBytecodeStart + Entries[I] * kInstructionSize;
          auto Selected = analyze(Image);
          ASSERT_TRUE(static_cast<bool>(Selected))
              << llvm::toString(Selected.takeError());
          ExecutionEnvironment Environment;
          std::vector<uint8_t> Input(8);
          llvm::support::endian::write64le(Input.data(), kHeapStart);
          Environment.Memory.push_back({kInputStart, Input, false, "pointer"});
          Environment.Memory.push_back(
              {kHeapStart, std::vector<uint8_t>(8), true, "output"});
          auto Raw = executeRaw(*Selected, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, OpaqueReturn            ? 0u
                                      : I + 1 == Calls.size() ? 43u
                                                              : 42u);
        }
      }
}

TEST(SBFCallXContexts, CapturedReturnsWidenOnlyAcrossActualCFGBackedges) {
  for (bool Loop : {false, true})
    for (bool ReverseEdges : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "loop=" << Loop << ", reverse=" << ReverseEdges);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned Register, uint64_t Value) {
        const auto Encoded = test::loadImm64(Register, Value);
        const size_t Slot = Text.size();
        Text.insert(Text.end(), Encoded.begin(), Encoded.end());
        return Slot;
      };
      Literal(7, kInputStart);
      Text.push_back(encode(Opcode::MOV64_IMM, 8));
      const size_t Enter = Text.size();
      Text.push_back({});
      const size_t Use = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 9, 0));
      const size_t LaterIteration = Text.size();
      Text.push_back({});
      const size_t FirstLiteral = Literal(2, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 9, 2, 8));
      Text.push_back(encode(Opcode::MOV64_REG, 6, 9));
      Text.push_back(encode(Opcode::MOV64_IMM, 8, 0, 0, 1));
      std::optional<size_t> Back;
      if (Loop) {
        Text.push_back(encode(Opcode::ADD64_IMM, 7, 0, 0, 8));
        Back = Text.size();
        Text.push_back({});
      }
      const size_t Finish = Text.size();
      std::optional<size_t> SecondLiteral;
      if (Loop) {
        SecondLiteral = Literal(2, 0);
        Text.push_back(encode(Opcode::ST_DW_REG, 9, 2, 8));
      }
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 6, 8));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 5));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Acquire = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 1, 7));
      const size_t AllocateCall = Text.size();
      Text.push_back({});
      const size_t NonNull = Text.size();
      Text.push_back(encode(Opcode::JNE64_IMM, 0, 0, Use - NonNull - 1));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Allocate = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
      Literal(2, kHeapStart + 8);
      Text.push_back(encode(Opcode::JGE64_REG, 0, 2, 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t FirstTarget = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t SecondTarget = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      Text[Enter] = encode(Opcode::JA, 0, 0, Acquire - Enter - 1);
      Text[LaterIteration] =
          encode(Opcode::JNE64_IMM, 8, 0, Finish - LaterIteration - 1);
      if (Back)
        Text[*Back] = encode(Opcode::JA, 0, 0, Acquire - *Back - 1);
      Text[AllocateCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Allocate - AllocateCall - 1);
      for (const auto &[Slot, Target] :
           {std::pair{FirstLiteral, FirstTarget},
            std::pair{SecondLiteral.value_or(FirstLiteral),
                      SecondLiteral ? SecondTarget : FirstTarget}}) {
        const auto Encoded = test::loadImm64(2, kBytecodeStart + Target * 8);
        std::copy(Encoded.begin(), Encoded.end(), Text.begin() + Slot);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (ReverseEdges)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      EXPECT_EQ(llvm::any_of(Report->Contexts,
                             [&](const auto &Site) {
                               return Site.CallXSlot == CallX &&
                                      Site.TargetSlot == FirstTarget;
                             }),
                !Loop);
      if (Loop)
        EXPECT_FALSE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
          return Site.CallXSlot == CallX &&
                 (Site.TargetAddress ||
                  Site.Result == CallXContextResult::ProvenTargetSet);
        }));
      for (bool Alias : {false, true}) {
        ExecutionEnvironment Environment;
        std::vector<uint8_t> Input(16);
        llvm::support::endian::write64le(Input.data(), kHeapStart + 16);
        llvm::support::endian::write64le(Input.data() + 8,
                                         kHeapStart + (Alias ? 16 : 64));
        Environment.Memory.push_back({kInputStart, Input, false, "pointers"});
        Environment.Memory.push_back(
            {kHeapStart, std::vector<uint8_t>(128), true, "objects"});
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, Loop && Alias ? 43u : 42u);
      }
    }
}

TEST(SBFCallXContexts, NarrowLoadBoundsExcludeImpossibleCalleeWrites) {
  for (const auto Load : {Opcode::LD_B_REG, Opcode::LD_H_REG, Opcode::LD_W_REG,
                          Opcode::LD_DW_REG}) {
    const unsigned Width = getOpcodeInfo(Load)->Width;
    SCOPED_TRACE(Width);
    const uint64_t Limit =
        Width == 64 ? UINT32_MAX : (uint64_t{1} << Width) - 1;
    std::vector<test::EncodedInstruction> Text;
    const auto Literal = [&](unsigned Register, uint64_t Value) {
      const auto Encoded = test::loadImm64(Register, Value);
      const size_t Slot = Text.size();
      Text.insert(Text.end(), Encoded.begin(), Encoded.end());
      return Slot;
    };
    Literal(6, kHeapStart + 16);
    const size_t FirstLiteral = Literal(2, 0);
    Text.push_back(encode(Opcode::ST_DW_REG, 6, 2));
    Literal(1, kInputStart);
    Text.push_back(encode(Load, 1, 1));
    const size_t Call = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::LD_DW_REG, 5, 6));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 5));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Helper = Literal(2, Limit);
    const size_t Branch = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Clobber = Literal(3, kInputStart + 8);
    Text.push_back(encode(Opcode::LD_DW_REG, 3, 3));
    const size_t SecondLiteral = Literal(2, 0);
    Text.push_back(encode(Opcode::ST_DW_REG, 3, 2));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t FirstTarget = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    const size_t SecondTarget = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
    Text.push_back(encode(Opcode::EXIT));
    Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Call - 1);
    Text[Branch] = encode(Opcode::JGT64_REG, 1, 2, Clobber - Branch - 1);
    for (const auto &[Slot, Target] :
         {std::pair{FirstLiteral, FirstTarget},
          std::pair{SecondLiteral, SecondTarget}}) {
      const auto Encoded = test::loadImm64(2, kBytecodeStart + Target * 8);
      std::copy(Encoded.begin(), Encoded.end(), Text.begin() + Slot);
    }
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_EQ(llvm::any_of(Report->Contexts,
                           [&](const auto &Site) {
                             return Site.CallXSlot == CallX &&
                                    Site.TargetSlot == FirstTarget;
                           }),
              Width < 64);
    for (uint64_t Value : {uint64_t{0}, Limit, Limit + 1}) {
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input(16);
      llvm::support::endian::write64le(Input.data(), Value);
      llvm::support::endian::write64le(Input.data() + 8, kHeapStart + 16);
      Environment.Memory.push_back({kInputStart, Input, false, "input"});
      Environment.Memory.push_back(
          {kHeapStart, std::vector<uint8_t>(32), true, "callback"});
      auto Raw = executeRaw(*Program, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, Width == 64 && Value > Limit ? 43u : 42u);
    }
  }
}

TEST(SBFCallXContexts, BoundedNumericInputsReuseCoveringSummariesAtCapacity) {
  for (unsigned Mode : {0u, 1u, 2u}) {
    SCOPED_TRACE(Mode);
    // Unknown and u16 inputs cover a later u8. A cached u8 summary cannot
    // justify a u16 caller whose high values select a different target.
    const auto FirstLoad = Mode == 0   ? Opcode::LD_DW_REG
                           : Mode == 1 ? Opcode::LD_H_REG
                                       : Opcode::LD_B_REG;
    const auto LastLoad = Mode == 2 ? Opcode::LD_H_REG : Opcode::LD_B_REG;
    std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
    const auto Literal = [&](unsigned R, uint64_t Value) {
      const auto Encoded = test::loadImm64(R, Value);
      Text.insert(Text.end(), Encoded.begin(), Encoded.end());
    };
    std::vector<size_t> Entries, Calls, Sites;
    for (unsigned I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
      Entries.push_back(Text.size());
      if (I == 0 || I == kCallXContextSpecializationsPerEntry) {
        Literal(2, kInputStart);
        Text.push_back(encode(I == 0 ? FirstLoad : LastLoad, 2, 2));
      } else {
        Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, I));
      }
      Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, 1));
      Calls.push_back(Text.size());
      Text.push_back({});
      Sites.push_back(Text.size());
      Text.push_back(encode(Opcode::CALL_REG, 0));
      Text.push_back(encode(Opcode::EXIT));
    }
    const size_t CallX = Sites.back();
    const size_t Helper = Text.size();
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -8));
    const size_t Select = Text.size();
    Text.push_back({});
    std::optional<size_t> HighValue;
    if (Mode == 2) {
      HighValue = Text.size();
      Text.push_back({});
    }
    const size_t ReturnA = Text.size();
    Literal(0, 0);
    Text.push_back(encode(Opcode::EXIT));
    const size_t ReturnB = Text.size();
    Literal(0, 0);
    Text.push_back(encode(Opcode::EXIT));
    Text[Select] = encode(Opcode::JNE64_IMM, 1, 0, ReturnB - Select - 1, 1);
    if (HighValue)
      Text[*HighValue] =
          encode(Opcode::JGT64_IMM, 2, 0, ReturnB - *HighValue - 1, UINT8_MAX);
    const size_t A = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    const size_t B = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
    Text.push_back(encode(Opcode::EXIT));
    for (size_t Slot : Calls)
      Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Slot - 1);
    for (const auto &[Slot, Target] :
         {std::pair{ReturnA, A}, std::pair{ReturnB, B}}) {
      const auto Encoded = test::loadImm64(0, kBytecodeStart + Target * 8);
      std::copy(Encoded.begin(), Encoded.end(), Text.begin() + Slot);
    }
    auto Image = test::makeImage(Text, {});
    for (size_t Entry : Entries) {
      Symbol Function;
      Function.Name = "caller_" + std::to_string(Entry);
      Function.Addr = kBytecodeStart + Entry * kInstructionSize;
      Function.IsFunc = true;
      Image.Symbols.push_back(std::move(Function));
    }
    auto Program = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_EQ(llvm::any_of(Report->Contexts,
                           [&](const auto &Site) {
                             return Site.CallXSlot == CallX &&
                                    Site.TargetSlot == A;
                           }),
              Mode != 2);
    EXPECT_EQ(llvm::count_if(Report->Summaries,
                             [&](const auto &Summary) {
                               return Summary.EntrySlot == Helper &&
                                      !Summary.GeneralizedInputs;
                             }),
              kCallXContextSpecializationsPerEntry);
    Image.Entry = kBytecodeStart + Entries.back() * kInstructionSize;
    auto Selected = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Selected))
        << llvm::toString(Selected.takeError());
    for (uint64_t Value : {0u, 255u, 256u}) {
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input(8);
      llvm::support::endian::write64le(Input.data(), Value);
      Environment.Memory.push_back({kInputStart, Input, false, "selector"});
      auto Raw = executeRaw(*Selected, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, Mode == 2 && Value > UINT8_MAX ? 43u : 42u);
    }
  }
}

} // namespace
TEST(SBFCallXContexts, ReclamationPreservesDistinctDescendantInputWitnesses) {
  for (bool DistinctFields : {false, true}) {
    SCOPED_TRACE(DistinctFields);
    constexpr uint32_t TableAddress = 0x10000;
    std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
    std::vector<size_t> Entries, LeafCalls, WrapperCalls, Literals;
    const auto Literal = [&](unsigned R, uint64_t Value) {
      const auto Encoded = test::loadImm64(R, Value);
      Text.insert(Text.end(), Encoded.begin(), Encoded.end());
    };
    // Pin the leaf's four variants before the wrapper supplies frame fields.
    for (size_t I = 0; I < kCallXContextSpecializationsPerEntry; ++I) {
      Entries.push_back(Text.size());
      Literal(1, TableAddress + I * 8);
      LeafCalls.push_back(Text.size());
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
    }
    for (size_t I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
      Entries.push_back(Text.size());
      const bool Last = I == kCallXContextSpecializationsPerEntry;
      const int16_t Offset = -16 - (DistinctFields ? I * 8 : 0);
      Literals.push_back(Text.size());
      Literal(5, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 5, Offset));
      Text.push_back(encode(Opcode::MOV64_REG, 2, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, Offset));
      Literal(1, TableAddress + (Last ? 8 : 0));
      Literal(4, kInputStart);
      Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, I));
      WrapperCalls.push_back(Text.size());
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
    }
    const size_t Wrapper = Text.size();
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 1, -8));
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 9, -16));
    Text.push_back(encode(Opcode::MOV64_REG, 7, 4));
    Text.push_back(encode(Opcode::MOV64_REG, 1, 2));
    const size_t Forward = Text.size();
    LeafCalls.push_back(Forward);
    Text.push_back({});
    Text.push_back(encode(Opcode::LD_DW_REG, 3, 7));
    Text.push_back(encode(Opcode::ST_DW_IMM, 3));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Leaf = Text.size();
    Text.push_back(encode(Opcode::LD_DW_REG, 5, 1));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 5));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Target = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    for (size_t Call : LeafCalls)
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Call - 1);
    for (size_t Call : WrapperCalls)
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Call - 1);
    for (size_t Slot : Literals) {
      const auto Encoded = test::loadImm64(5, kBytecodeStart + Target * 8);
      std::copy(Encoded.begin(), Encoded.end(), Text.begin() + Slot);
    }
    std::vector<uint8_t> Table(kCallXContextSpecializationsPerEntry * 8);
    for (size_t I = 0; I < kCallXContextSpecializationsPerEntry; ++I)
      llvm::support::endian::write64le(Table.data() + I * 8,
                                       kBytecodeStart + Target * 8);
    auto Image = test::makeImage(Text, Table);
    Image.Sections[1].VA = TableAddress;
    Image.SBF->RodataVM = {TableAddress, Table.size()};
    for (size_t Entry : Entries) {
      Symbol Function;
      Function.Name = "caller_" + std::to_string(Entry);
      Function.Addr = kBytecodeStart + Entry * kInstructionSize;
      Function.IsFunc = true;
      Image.Symbols.push_back(std::move(Function));
    }
    auto Program = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_EQ(Report->GeneralizedPublishedSummaries > 0, !DistinctFields);
    for (size_t I = 0; I < kCallXContextSpecializationsPerEntry; ++I) {
      const int64_t Offset =
          -16 - static_cast<int64_t>(DistinctFields ? I * 8 : 0) -
          static_cast<int64_t>(
              automaticFrameStride(Program->Low.TheVersion, Program->Config));
      EXPECT_TRUE(llvm::any_of(
          Report->Contexts,
          [&](const auto &Site) {
            return Site.CallerSlot == Forward && Site.CallXSlot == CallX &&
                   Site.TargetSlot == Target &&
                   llvm::any_of(
                       Site.InputExpressionValues, [&](const auto &Step) {
                         const auto Input = Step.Inputs.singleton();
                         return Input.ValueKind ==
                                    RegisterValue::Kind::StackAddress &&
                                Input.Offset == Offset;
                       });
          }))
          << "missing descriptor field " << I;
    }
    for (size_t Entry : {Wrapper, Leaf})
      EXPECT_LE(llvm::count_if(Report->Summaries,
                               [&](const auto &Summary) {
                                 return Summary.EntrySlot == Entry &&
                                        !Summary.GeneralizedInputs;
                               }),
                kCallXContextSpecializationsPerEntry);
    for (size_t Entry : Entries) {
      Image.Entry = kBytecodeStart + Entry * kInstructionSize;
      auto Selected = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Selected))
          << llvm::toString(Selected.takeError());
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input(8);
      llvm::support::endian::write64le(Input.data(), kHeapStart);
      Environment.Memory.push_back({kInputStart, Input, false, "pointer"});
      Environment.Memory.push_back(
          {kHeapStart, std::vector<uint8_t>(8), true, "output"});
      auto Raw = executeRaw(*Selected, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue,
                Entry < Entries[kCallXContextSpecializationsPerEntry] ? 42u
                                                                      : 0u);
      EXPECT_TRUE(llvm::any_of(
          Raw->Trace, [&](const auto &Step) { return Step.Slot == Target; }));
    }
  }
}

TEST(SBFCallXContexts,
     AdditionalCallerFactsDoNotInvalidateABroaderMemorySummary) {
  for (bool ChangesOldFact : {false, true}) {
    SCOPED_TRACE(ChangesOldFact);
    std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
    const auto Literal = [&](unsigned R, uint64_t Value) {
      const auto Encoded = test::loadImm64(R, Value);
      Text.insert(Text.end(), Encoded.begin(), Encoded.end());
    };
    std::vector<size_t> Entries, Calls, Sites;
    for (unsigned I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
      Entries.push_back(Text.size());
      const bool Last = I == kCallXContextSpecializationsPerEntry;
      if (ChangesOldFact)
        Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -8, Last));
      else if (Last) {
        Literal(5, kInputStart);
        Text.push_back(encode(Opcode::LD_B_REG, 5, 5));
        Text.push_back(encode(Opcode::ST_DW_REG, 10, 5, -8));
      }
      Literal(3, kInputStart + 8);
      Text.push_back(encode(Opcode::LD_DW_REG, 3, 3));
      Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, Last ? 0 : I));
      Text.push_back(encode(Opcode::MOV64_REG, 4, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 4, 0, 0, -8));
      Calls.push_back(Text.size());
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
      if (Last)
        Text.push_back(encode(Opcode::LD_DW_REG, 7, 10, -8));
      Sites.push_back(Text.size());
      Text.push_back(encode(Opcode::CALL_REG, 6));
      if (Last)
        Text.push_back(encode(Opcode::MOV64_REG, 0, 7));
      Text.push_back(encode(Opcode::EXIT));
    }
    const size_t Helper = Text.size();
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -16));
    Text.push_back(encode(Opcode::LD_DW_REG, 3, 3));
    Text.push_back(encode(Opcode::LD_DW_REG, 6, 3));
    Text.push_back(ChangesOldFact ? encode(Opcode::LD_DW_REG, 5, 4)
                                  : encode(Opcode::MOV64_REG, 5, 1));
    const size_t Select = Text.size();
    Text.push_back({});
    const size_t ReturnA = Text.size();
    Literal(0, 0);
    Text.push_back(encode(Opcode::EXIT));
    const size_t ReturnB = Text.size();
    Literal(0, 0);
    Text.push_back(encode(Opcode::EXIT));
    Text[Select] = encode(Opcode::JNE64_IMM, 5, 0, ReturnB - Select - 1,
                          ChangesOldFact ? 0 : 1);
    const size_t A = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    const size_t B = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
    Text.push_back(encode(Opcode::EXIT));
    for (size_t Slot : Calls)
      Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Slot - 1);
    for (const auto &[Slot, Target] :
         {std::pair{ReturnA, A}, std::pair{ReturnB, B}}) {
      const auto Encoded = test::loadImm64(0, kBytecodeStart + Target * 8);
      std::copy(Encoded.begin(), Encoded.end(), Text.begin() + Slot);
    }
    auto Image = test::makeImage(Text, {});
    for (size_t Entry : Entries) {
      Symbol Function;
      Function.Name = "caller_" + std::to_string(Entry);
      Function.Addr = kBytecodeStart + Entry * kInstructionSize;
      Function.IsFunc = true;
      Image.Symbols.push_back(std::move(Function));
    }
    auto Program = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_EQ(llvm::any_of(Report->Contexts,
                           [&](const auto &Site) {
                             return Site.CallXSlot == Sites.back() &&
                                    Site.TargetSlot == A;
                           }),
              !ChangesOldFact);
    EXPECT_EQ(llvm::count_if(Report->Summaries,
                             [&](const auto &Summary) {
                               return Summary.EntrySlot == Helper &&
                                      !Summary.GeneralizedInputs;
                             }),
              kCallXContextSpecializationsPerEntry);
    if (!ChangesOldFact)
      EXPECT_TRUE(llvm::any_of(Report->Summaries, [&](const auto &Summary) {
        return Summary.EntrySlot == Entries.back() &&
               !Summary.GeneralizedInputs &&
               Summary.ReturnValue == unsignedRangeValue({0, UINT8_MAX});
      }));
    Image.Entry = kBytecodeStart + Entries.back() * kInstructionSize;
    auto Selected = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Selected))
        << llvm::toString(Selected.takeError());
    for (uint64_t Value : {0u, 255u}) {
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input(16), Heap(16);
      llvm::support::endian::write64le(Input.data(), Value);
      llvm::support::endian::write64le(Input.data() + 8, kHeapStart);
      llvm::support::endian::write64le(Heap.data(), kHeapStart + 8);
      Environment.Memory.push_back({kInputStart, Input, false, "input"});
      Environment.Memory.push_back({kHeapStart, Heap, false, "read chain"});
      auto Raw = executeRaw(*Selected, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, ChangesOldFact ? 1u : Value);
      EXPECT_TRUE(llvm::any_of(Raw->Trace, [&](const auto &Step) {
        return Step.Slot == (ChangesOldFact ? B : A);
      }));
    }
  }
}

TEST(SBFCallXContexts, BranchBoundsFollowExactCopiesAndLosslessTruncations) {
  // Direct copy, truncation, lost high bits, independent equal domains,
  // overwritten source, overwritten shifted value, and a skipped prefix.
  for (unsigned Mode : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) {
    SCOPED_TRACE(Mode);
    constexpr uint64_t Character = 43;
    constexpr uint64_t Sentinel = 0x110000;
    const uint64_t Other =
        Mode == 2 ? (uint64_t{1} << 32) | Character : Sentinel;
    std::vector<test::EncodedInstruction> Text;
    const auto Literal = [&](unsigned R, uint64_t Value) {
      const auto Encoded = test::loadImm64(R, Value);
      Text.insert(Text.end(), Encoded.begin(), Encoded.end());
    };
    Literal(6, kInputStart);
    const auto Choose = [&](unsigned R, int16_t Offset, uint64_t Alternative) {
      Text.push_back(encode(Opcode::LD_DW_REG, 1, 6, Offset));
      const size_t Branch = Text.size();
      Text.push_back({});
      Literal(R, Alternative);
      const size_t Jump = Text.size();
      Text.push_back({});
      const size_t First = Text.size();
      Literal(R, Character);
      Text[Branch] = encode(Opcode::JEQ64_IMM, 1, 0, First - Branch - 1);
      Text[Jump] = encode(Opcode::JA, 0, 0, Text.size() - Jump - 1);
    };
    Choose(3, 0, Other);
    Choose(4, 8, Sentinel);
    Text.push_back(encode(Opcode::LD_DW_REG, 5, 6, 16));
    const size_t RootCall = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::EXIT));
    const size_t Helper = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 1, Mode == 3 ? 4 : 3));
    if (Mode) {
      if (Mode == 6)
        Text.push_back(encode(Opcode::JEQ64_IMM, 5, 0, 1));
      Text.push_back(encode(Opcode::LSH64_IMM, 1, 0, 0, 32));
      if (Mode == 4)
        Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0, Sentinel));
      if (Mode == 5)
        Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, Character));
      Text.push_back(encode(Opcode::RSH64_IMM, 1, 0, 0, 32));
    }
    const size_t Guard = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::MOV64_REG, 2, 3));
    const size_t Forward = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
    const size_t Site = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 6));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Skip = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Select = Text.size();
    Text.push_back({});
    const size_t ReturnB = Text.size();
    Literal(0, 0);
    Text.push_back(encode(Opcode::EXIT));
    const size_t ReturnA = Text.size();
    Literal(0, 0);
    Text.push_back(encode(Opcode::EXIT));
    const size_t TargetA = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    const size_t TargetB = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
    Text.push_back(encode(Opcode::EXIT));
    Text[RootCall] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - RootCall - 1);
    Text[Guard] = encode(Opcode::JEQ64_IMM, 1, 0, Skip - Guard - 1, Sentinel);
    Text[Forward] = encode(Opcode::CALL_IMM, 0, 1, 0, Select - Forward - 1);
    Text[Select] =
        encode(Opcode::JEQ64_IMM, 2, 0, ReturnA - Select - 1, Character);
    for (const auto &[Slot, Target] :
         {std::pair{ReturnA, TargetA}, std::pair{ReturnB, TargetB}}) {
      const auto Encoded = test::loadImm64(0, kBytecodeStart + Target * 8);
      std::copy(Encoded.begin(), Encoded.end(), Text.begin() + Slot);
    }
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_EQ(llvm::any_of(Report->Contexts,
                           [&](const auto &Evidence) {
                             return Evidence.CallXSlot == Site &&
                                    Evidence.TargetSlot == TargetA;
                           }),
              Mode <= 1);
    for (unsigned First : {0u, 1u})
      for (unsigned Second : {0u, 1u})
        for (unsigned Shift : {0u, 1u}) {
          SCOPED_TRACE(::testing::Message()
                       << First << ":" << Second << ":" << Shift);
          uint64_t R3 = First ? Other : Character;
          uint64_t R1 = Mode == 3 ? (Second ? Sentinel : Character) : R3;
          if (Mode) {
            if (Mode != 6 || Shift)
              R1 <<= 32;
            if (Mode == 4)
              R3 = Sentinel;
            if (Mode == 5)
              R1 = Character;
            R1 >>= 32;
          }
          const uint64_t Expected = R1 == Sentinel    ? 0
                                    : R3 == Character ? 42
                                                      : 43;
          std::vector<uint8_t> Input(24);
          llvm::support::endian::write64le(Input.data(), First);
          llvm::support::endian::write64le(Input.data() + 8, Second);
          llvm::support::endian::write64le(Input.data() + 16, Shift);
          ExecutionEnvironment Environment;
          Environment.Memory.push_back(
              {kInputStart, Input, false, "selectors"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, Expected);
          if (Expected)
            EXPECT_TRUE(llvm::any_of(Raw->Trace, [&](const auto &Step) {
              return Step.Slot == (Expected == 42 ? TargetA : TargetB);
            }));
        }
  }
}

TEST(SBFCallXContexts,
     KnownCopyLoopsPreserveSpillsWithAndWithoutSpecialization) {
  for (unsigned WarmCalls : {0u, 4u})
    // Known copy, unknown destination/source/branch, and an overlapping spill.
    for (unsigned Mode : {0u, 1u, 2u, 3u, 4u}) {
      SCOPED_TRACE(::testing::Message() << WarmCalls << ":" << Mode);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const auto Slots = test::loadImm64(R, Value);
        Text.insert(Text.end(), Slots.begin(), Slots.end());
      };
      Literal(6, 0);
      for (int Base : {-192, -128})
        for (int Offset = 0; Offset < 48; Offset += 8)
          Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, Base + Offset));
      std::vector<size_t> Calls;
      for (unsigned I = 0; I <= WarmCalls; ++I) {
        Text.push_back(encode(Opcode::ST_DW_REG, 10, 6, -8));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0,
                              I == WarmCalls && Mode == 4 ? -48 : -128));
        Text.push_back(encode(Opcode::MOV64_REG, 2, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, -192));
        if (I == WarmCalls && (Mode == 1 || Mode == 2)) {
          Literal(Mode, kInputStart);
          Text.push_back(encode(Opcode::LD_DW_REG, Mode, Mode));
        }
        Text.push_back(
            encode(Opcode::MOV64_IMM, 3, 0, 0, I == WarmCalls ? 48 : I + 1));
        Calls.push_back(Text.size());
        Text.push_back({});
      }
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 10, -8));
      const size_t Site = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Copy = Text.size();
      Text.push_back({});
      std::optional<size_t> UnknownBranch;
      if (Mode == 3) {
        Literal(4, kInputStart + 8);
        Text.push_back(encode(Opcode::LD_DW_REG, 4, 4));
        UnknownBranch = Text.size();
        Text.push_back({});
      }
      Text.push_back(encode(Opcode::MOV64_IMM, 5));
      const size_t Loop = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 4, 2));
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 4));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, 8));
      Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, 8));
      Text.push_back(encode(Opcode::ADD64_IMM, 5, 0, 0, 8));
      const size_t Back = Text.size();
      Text.push_back(encode(Opcode::JLT64_REG, 5, 3, Loop - Back - 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      if (UnknownBranch) {
        const size_t Poison = Text.size();
        Text.push_back(encode(Opcode::ST_DW_IMM, 1, 0, 120));
        Text.push_back(encode(Opcode::MOV64_IMM, 0));
        Text.push_back(encode(Opcode::EXIT));
        Text[*UnknownBranch] =
            encode(Opcode::JNE64_IMM, 4, 0, Poison - *UnknownBranch - 1);
      }
      const size_t Syscall = Text.size();
      Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                            getSyscallInfo(Syscall::Memmove)->Hash));
      Text.push_back(encode(Opcode::EXIT));
      Text[Copy] = encode(Opcode::JNE64_IMM, 3, 0, Syscall - Copy - 1, 48);
      const size_t Target = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const auto TargetAddress =
          test::loadImm64(6, kBytecodeStart + Target * 8);
      std::copy(TargetAddress.begin(), TargetAddress.end(), Text.begin());
      for (size_t Slot : Calls)
        Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Copy - Slot - 1);
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      const auto Root =
          llvm::find_if(Report->Summaries, [](const auto &Summary) {
            return Summary.EntrySlot == 0 && !Summary.GeneralizedInputs;
          });
      ASSERT_NE(Root, Report->Summaries.end());
      EXPECT_EQ(llvm::any_of(Report->Contexts,
                             [&](const auto &Evidence) {
                               return Evidence.SummaryID == Root->ID &&
                                      Evidence.CallXSlot == Site &&
                                      Evidence.TargetSlot == Target;
                             }),
                Mode == 0);
      EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
        return Edge.CallerSummaryID == Root->ID &&
               Edge.CallerSlot == Calls.back() &&
               Edge.GeneralizedInputs == (WarmCalls == 4) &&
               Edge.InstantiatedMemoryPathStores ==
                   (Mode == 0 || Mode == 4 ? 6 : 0);
      }));
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      EXPECT_LE(llvm::count_if(Report->Summaries,
                               [&](const auto &Summary) {
                                 return Summary.EntrySlot == Copy &&
                                        !Summary.GeneralizedInputs;
                               }),
                4);
      for (unsigned Selector = 0; Selector != (Mode == 3 ? 2u : 1u);
           ++Selector) {
        SCOPED_TRACE(Selector);
        const auto Frame =
            initialFramePointer(Program->Low.TheVersion, Program->Config);
        std::vector<uint8_t> Input(16);
        llvm::support::endian::write64le(Input.data(),
                                         Frame - (Mode == 1 ? 48 : 192));
        llvm::support::endian::write64le(Input.data() + 8, Selector);
        ExecutionEnvironment Environment;
        Environment.Memory.push_back(
            {kInputStart, Input, false, "copy controls"});
        // Warm-up calls copy zero bytes onto zero bytes; the full copy and
        // alias controls execute their actual guest stores without a callback.
        Environment.HostSyscall = [Frame](uint32_t Hash,
                                          const SyscallArguments &Args) {
          return Hash == getSyscallInfo(Syscall::Memmove)->Hash &&
                         Args[0] == Frame - 128 && Args[1] == Frame - 192 &&
                         Args[2] >= 1 && Args[2] <= 4
                     ? SyscallOutcome::returned(0)
                     : SyscallOutcome::unregistered();
        };
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        if (Mode == 1 || Mode == 4 || Selector) {
          EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted) << Raw->Error;
          EXPECT_EQ(Raw->Fault, FaultCode::UnknownIndirectCall);
        } else {
          EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, 42u);
          EXPECT_TRUE(llvm::any_of(Raw->Trace, [&](const auto &Step) {
            return Step.Slot == Target;
          }));
        }
      }
    }
}

TEST(SBFCallXContexts, NestedCopyPathsPreserveDescriptorFieldsAtVariantLimit) {
  // Empty/nonempty append, unknown descriptor/branch, recursive/spinning
  // nested body, depth refusal, and registered legacy runtime mutation.
  for (unsigned Mode = 0; Mode != 10; ++Mode) {
    SCOPED_TRACE(Mode);
    std::vector<test::EncodedInstruction> Text;
    const auto Literal = [&](unsigned R, uint64_t Value) {
      const size_t Slot = Text.size();
      const auto Instructions = test::loadImm64(R, Value);
      Text.insert(Text.end(), Instructions.begin(), Instructions.end());
      return Slot;
    };
    const size_t TargetLiteral = Literal(6, 0);
    for (int Offset = -512; Offset != -448; Offset += 8)
      Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, Offset));
    std::vector<size_t> Calls;
    for (unsigned I = 0; I != 5; ++I) {
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 6, -8));
      Text.push_back(encode(Opcode::MOV64_REG, 4, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 4, 0, 0, -96));
      if (I == 4 && Mode == 2) {
        Literal(4, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 4, 4));
      }
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 4, -32));
      Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -96, 64));
      Text.push_back(encode(Opcode::MOV64_REG, 4, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 4, 0, 0, -256));
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 4, -88));
      Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -80));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -32));
      Text.push_back(encode(Opcode::MOV64_REG, 2, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, -512));
      if (I == 4 && Mode == 0)
        Text.push_back(encode(Opcode::MOV64_IMM, 2));
      Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0,
                            I == 4 ? (Mode == 1 ? 40 : 0) : 8 * (I + 1)));
      if (I == 4 && Mode >= 8) {
        Literal(4, kInputStart + 8);
        Text.push_back(encode(Opcode::LD_DW_REG, 4, 4));
        std::vector<size_t> Branches, DoneBranches;
        for (unsigned Choice = 0; Choice != 3; ++Choice) {
          Branches.push_back(Text.size());
          Text.push_back({});
        }
        for (unsigned Choice = 0; Choice != 4; ++Choice) {
          const unsigned Value = Choice == 0 ? 3 : Choice - 1;
          if (Choice)
            Text[Branches[Value]] =
                encode(Opcode::JEQ64_IMM, 4, 0,
                       Text.size() - Branches[Value] - 1, Value);
          Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0, Value * 8));
          DoneBranches.push_back(Text.size());
          Text.push_back({});
        }
        for (size_t Jump : DoneBranches)
          Text[Jump] = encode(Opcode::JA, 0, 0, Text.size() - Jump - 1);
      }
      Text.push_back(encode(Opcode::MOV64_REG, 5, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 5, 0, 0, -8));
      Calls.push_back(Text.size());
      Text.push_back({});
    }
    Text.push_back(encode(Opcode::LD_DW_REG, 0, 10, -8));
    const size_t Site = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Append = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 6, 3));
    Text.push_back(encode(Opcode::MOV64_REG, 8, 2));
    Text.push_back(encode(Opcode::LD_DW_REG, 7, 1));
    Text.push_back(encode(Opcode::LD_DW_REG, 9, 7, 16));
    Text.push_back(encode(Opcode::LD_DW_REG, 1, 7));
    Text.push_back(encode(Opcode::SUB64_REG, 1, 9));
    const size_t CapacityBranch = Text.size();
    Text.push_back({});
    // This path must stay unknown when capacity is not established.
    Text.push_back(encode(Opcode::ST_DW_IMM, 5));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Enough = Text.size();
    Text.push_back(encode(Opcode::LD_DW_REG, 1, 7, 8));
    Text.push_back(encode(Opcode::ADD64_REG, 1, 9));
    Text.push_back(encode(Opcode::MOV64_REG, 2, 8));
    Text.push_back(encode(Opcode::MOV64_REG, 3, 6));
    const size_t InnerCall = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::ADD64_REG, 9, 6));
    Text.push_back(encode(Opcode::ST_DW_REG, 7, 9, 16));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Copy = Text.size();
    const size_t EmptyBranch = Text.size();
    Text.push_back({});
    const size_t LargeBranch = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::MOV64_IMM, 4));
    const size_t Loop = Text.size();
    Text.push_back(encode(Opcode::LD_DW_REG, 0, 2));
    Text.push_back(encode(Opcode::ST_DW_REG, 1, 0));
    Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, 8));
    Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, 8));
    Text.push_back(encode(Opcode::ADD64_IMM, 4, 0, 0, 8));
    const size_t Back = Text.size();
    Text.push_back(encode(Opcode::JLT64_REG, 4, 3, Loop - Back - 1));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Empty = Text.size();
    if (Mode == 3 || Mode == 9) {
      Literal(4, kInputStart + (Mode == 9 ? 16 : 8));
      Text.push_back(encode(Opcode::LD_DW_REG, 4, 4));
      Text.push_back(encode(Opcode::JEQ64_IMM, 4, 0, 1));
      Text.push_back(encode(Opcode::ST_DW_IMM, 5));
    }
    if (Mode == 4) {
      const size_t Recurse = Text.size();
      Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, Copy - Recurse - 1));
    }
    if (Mode == 5)
      Text.push_back(encode(Opcode::JA, 0, 0, -1));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Syscall = Text.size();
    Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                          getSyscallInfo(Syscall::Memmove)->Hash));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Target = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    const auto Address = test::loadImm64(6, kBytecodeStart + Target * 8);
    std::copy(Address.begin(), Address.end(), Text.begin() + TargetLiteral);
    for (size_t Slot : Calls)
      Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Append - Slot - 1);
    Text[InnerCall] = encode(Opcode::CALL_IMM, 0, 1, 0, Copy - InnerCall - 1);
    Text[CapacityBranch] =
        encode(Opcode::JGE64_REG, 1, 6, Enough - CapacityBranch - 1);
    Text[EmptyBranch] =
        encode(Opcode::JEQ64_IMM, 3, 0, Empty - EmptyBranch - 1);
    Text[LargeBranch] =
        encode(Opcode::JGT64_IMM, 3, 0, Syscall - LargeBranch - 1, 64);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    if (Mode == 6)
      Program->Config.MaxCallDepth = 2;
    if (Mode == 7)
      for (auto &Instruction : Program->Med.Instructions)
        if (Instruction.Slot == InnerCall) {
          Instruction.Dispatch = CallDispatchPolicy::LegacyRuntimeThenFunction;
          Instruction.Immediate = Program->RegisteredSyscallHashes.front();
        }
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    const auto Root = llvm::find_if(Report->Summaries, [](const auto &Summary) {
      return Summary.EntrySlot == 0 && !Summary.GeneralizedInputs;
    });
    ASSERT_NE(Root, Report->Summaries.end());
    EXPECT_EQ(llvm::any_of(Report->Contexts,
                           [&](const auto &Evidence) {
                             return Evidence.SummaryID == Root->ID &&
                                    Evidence.CallXSlot == Site &&
                                    Evidence.TargetSlot == Target;
                           }),
              Mode < 2 || Mode == 8);
    const auto Edge = llvm::find_if(Report->SummaryEdges, [&](const auto &E) {
      return E.CallerSummaryID == Root->ID && E.CallerSlot == Calls.back();
    });
    ASSERT_NE(Edge, Report->SummaryEdges.end());
    EXPECT_EQ(Edge->InstantiatedMemoryPathStores, Mode < 2
                                                      ? (Mode == 0 ? 1u : 6u)
                                                  : Mode == 8 ? 10u
                                                              : 0u);
    if (Mode < 2 || Mode == 8)
      EXPECT_TRUE(Edge->GeneralizedInputs);
    EXPECT_FALSE(Report->SpecializationBudgetExhausted);
    EXPECT_LE(llvm::count_if(Report->Summaries,
                             [&](const auto &Summary) {
                               return Summary.EntrySlot == Append &&
                                      !Summary.GeneralizedInputs;
                             }),
              4);
    // Mode 7 is an IR dispatch-boundary counterexample; the remaining cases
    // execute the original instruction bytes, including both unknown branches.
    if (Mode == 7)
      continue;
    for (unsigned Selector = 0; Selector != (Mode >= 8   ? 4u
                                             : Mode == 3 ? 2u
                                                         : 1u);
         ++Selector)
      for (unsigned Poison = 0; Poison != (Mode == 9 ? 2u : 1u); ++Poison) {
        const auto Frame =
            initialFramePointer(Program->Low.TheVersion, Program->Config);
        std::vector<uint8_t> Input(24);
        llvm::support::endian::write64le(Input.data(), Frame - 96);
        llvm::support::endian::write64le(Input.data() + 8, Selector);
        llvm::support::endian::write64le(Input.data() + 16, Poison);
        ExecutionEnvironment Environment;
        Environment.Memory.push_back(
            {kInputStart, Input, false, "nested copy controls"});
        InterpreterOptions Options;
        Options.MaxSteps = 4096;
        auto Raw = executeRaw(*Program, std::move(Environment), Options);
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        if (Mode == 5) {
          EXPECT_EQ(Raw->Status, ExecutionStatus::StepLimit);
        } else if (Mode == 4 || Mode == 6 || (Mode == 3 && Selector) ||
                   (Mode == 9 && Selector == 0 && Poison)) {
          EXPECT_EQ(Raw->Status, ExecutionStatus::Faulted) << Raw->Error;
          EXPECT_EQ(Raw->Fault, Mode == 4 || Mode == 6
                                    ? FaultCode::CallDepth
                                    : FaultCode::UnknownIndirectCall);
        } else {
          EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, 42u);
          EXPECT_TRUE(llvm::any_of(Raw->Trace, [&](const auto &Step) {
            return Step.Slot == Target;
          }));
        }
      }
  }
}

TEST(SBFCallXContexts, ProvenCopyReadsKeepDistinctSourcesAcrossCacheReuse) {
  for (bool Nested : {false, true})
    for (bool OpaqueRead : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "nested=" << Nested << ", opaque=" << OpaqueRead);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t At = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return At;
      };
      std::vector<size_t> Warmups, Calls, Sites, Literals;
      for (unsigned I = 0; I < kCallXContextSpecializationsPerEntry; ++I) {
        // Unknown destinations consume the four copy variants before the
        // caller supplies the concrete frame copy that needs path replay.
        Literal(1, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 1, 1));
        Text.push_back(encode(Opcode::MOV64_REG, 9, 1));
        Text.push_back(encode(Opcode::MOV64_REG, 2, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, -128));
        Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0, 8 * (I + 1)));
        Warmups.push_back(Text.size());
        Text.push_back({});
      }
      constexpr unsigned Count = kCallXContextSpecializationsPerEntry + 2;
      for (unsigned I = 0; I < Count; ++I) {
        Literals.push_back(Literal(6, 0));
        Text.push_back(encode(Opcode::ST_DW_REG, 10, 6, -16));
        Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -64, I));
        Literal(9, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 9, 9));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
        Calls.push_back(Text.size());
        Text.push_back({});
        Sites.push_back(Text.size());
        Text.push_back(encode(Opcode::CALL_REG, 0));
      }
      Text.push_back(encode(Opcode::EXIT));
      const size_t Leaf = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 2, 1));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
      Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0, 8));
      const size_t CopyCall = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 10, -8));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Copy = Text.size();
      if (Nested) {
        Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, -8));
        Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, 1));
        Text.push_back(encode(Opcode::EXIT));
      }
      if (OpaqueRead)
        Text.push_back(encode(Opcode::LD_DW_REG, 0, 9));
      const size_t Empty = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 4, 3));
      const size_t Loop = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 2));
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 5));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, 8));
      Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, 8));
      Text.push_back(encode(Opcode::SUB64_IMM, 4, 0, 0, 8));
      Text.push_back(encode(Opcode::JNE64_IMM, 4, 0, Loop - Text.size() - 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t EmptyCopy = Text.size();
      Text.push_back(encode(Opcode::CALL_IMM, 0, 0, 0,
                            getSyscallInfo(Syscall::Memcpy)->Hash));
      Text.push_back(encode(Opcode::EXIT));
      Text[Empty] = encode(Opcode::JEQ64_IMM, 3, 0, EmptyCopy - Empty - 1);
      const size_t A = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t B = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      for (size_t Slot : Warmups)
        Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Copy - Slot - 1);
      Text[CopyCall] = encode(Opcode::CALL_IMM, 0, 1, 0, Copy - CopyCall - 1);
      for (unsigned I = 0; I < Count; ++I) {
        Text[Calls[I]] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Calls[I] - 1);
        const auto Words = test::loadImm64(
            6, kBytecodeStart +
                   (I == kCallXContextSpecializationsPerEntry ? B : A) * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Literals[I]);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
        return Edge.CallerSlot == CopyCall &&
               Edge.InstantiatedMemoryPathStores > 0;
      }));
      const auto Root = llvm::find_if(Report->Summaries, [](const auto &S) {
        return S.EntrySlot == 0 && !S.GeneralizedInputs;
      });
      ASSERT_NE(Root, Report->Summaries.end());
      for (unsigned I = 0; I < Count; ++I) {
        const bool ProvenA = llvm::any_of(Report->Contexts, [&](const auto &C) {
          return C.SummaryID == Root->ID && C.CallXSlot == Sites[I] &&
                 C.TargetSlot == A;
        });
        EXPECT_EQ(ProvenA, I < kCallXContextSpecializationsPerEntry ||
                               (I + 1 == Count && !OpaqueRead))
            << "caller " << I;
      }
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input(8);
      llvm::support::endian::write64le(Input.data(), kHeapStart);
      Environment.Memory.push_back({kInputStart, Input, false, "destination"});
      Environment.Memory.push_back(
          {kHeapStart, std::vector<uint8_t>(32), true, "copy destination"});
      auto Raw = executeRaw(*Program, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 42u);
      std::vector<size_t> Targets;
      for (const auto &Step : Raw->Trace)
        if (Step.Slot == A || Step.Slot == B)
          Targets.push_back(Step.Slot);
      ASSERT_EQ(Targets.size(), Count);
      for (unsigned I = 0; I < Count; ++I)
        EXPECT_EQ(Targets[I],
                  I == kCallXContextSpecializationsPerEntry ? B : A);
    }
}

TEST(SBFCallXContexts, LoaderEntryFactsDoNotInheritOtherRootPaths) {
  for (bool CallsEntry : {false, true})
    for (bool Loop : {false, true})
      for (bool ReverseEdges : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << "calls-entry=" << CallsEntry << ", loop=" << Loop
                     << ", reverse=" << ReverseEdges);
        std::vector<test::EncodedInstruction> Text;
        const auto Literal = [&](unsigned Register, uint64_t Value) {
          const size_t Slot = Text.size();
          const auto Encoded = test::loadImm64(Register, Value);
          Text.insert(Text.end(), Encoded.begin(), Encoded.end());
          return Slot;
        };
        const auto Stride = automaticFrameStride(Version::V3, SBFVMConfig{});
        Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
        Text.push_back(
            encode(Opcode::ADD64_IMM, 1, 0, 0,
                   CallsEntry ? static_cast<int32_t>(Stride) - 8 : -8));
        std::optional<size_t> EntryCall;
        if (CallsEntry) {
          EntryCall = Text.size();
          Text.push_back({});
          Text.push_back(encode(Opcode::EXIT));
        }
        const size_t Entry = Text.size();
        Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
        const size_t FirstLiteral = Literal(2, 0);
        const size_t Initialize = Text.size();
        Text.push_back({});
        const size_t SecondLiteral = Literal(2, 0);
        Text.push_back(encode(Opcode::ST_DW_REG, 6, 2));
        std::optional<size_t> Done;
        if (Loop) {
          Done = Text.size();
          Text.push_back({});
          Text.push_back(encode(Opcode::MOV64_IMM, 7, 0, 0, 1));
          Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
          const size_t Back = Text.size();
          Text.push_back(encode(Opcode::JA, 0, 0, Entry - Back - 1));
        }
        const size_t Finish = Text.size();
        Text.push_back(encode(Opcode::LD_DW_REG, 5, 10, -8));
        const size_t CallX = Text.size();
        Text.push_back(encode(Opcode::CALL_REG, 5));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Store = Text.size();
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 2));
        Text.push_back(encode(Opcode::MOV64_IMM, 0));
        Text.push_back(encode(Opcode::EXIT));
        const size_t FirstTarget = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
        Text.push_back(encode(Opcode::EXIT));
        const size_t SecondTarget = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
        Text.push_back(encode(Opcode::EXIT));
        Text[Initialize] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Store - Initialize - 1);
        if (EntryCall)
          Text[*EntryCall] =
              encode(Opcode::CALL_IMM, 0, 1, 0, Entry - *EntryCall - 1);
        if (Done)
          Text[*Done] = encode(Opcode::JNE64_IMM, 7, 0, Finish - *Done - 1);
        for (const auto &[Slot, Target] :
             {std::pair{FirstLiteral, FirstTarget},
              std::pair{SecondLiteral, SecondTarget}}) {
          const auto Encoded = test::loadImm64(2, kBytecodeStart + Target * 8);
          std::copy(Encoded.begin(), Encoded.end(), Text.begin() + Slot);
        }
        auto Image = test::makeImage(Text, {});
        Image.Entry = kBytecodeStart + Entry * kInstructionSize;
        Image.addSymbol("alternate_entry", kBytecodeStart,
                        Entry * kInstructionSize, true);
        auto Program = analyze(Image);
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        if (ReverseEdges)
          std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        checkSummaryReferences(*Report);
        EXPECT_FALSE(Report->SpecializationBudgetExhausted);
        EXPECT_EQ(llvm::any_of(Report->Contexts,
                               [&](const auto &Site) {
                                 return Site.CalleeEntrySlot == Entry &&
                                        Site.IsRootContext &&
                                        !Site.EntryPrefixOnly &&
                                        Site.CallXSlot == CallX &&
                                        Site.TargetSlot == FirstTarget;
                               }),
                  !Loop);
        if (CallsEntry)
          EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
            return Site.CallerSlot == *EntryCall && !Site.EntryPrefixOnly &&
                   Site.CallXSlot == CallX && Site.TargetSlot == SecondTarget;
          }));
        // Execute both the loader entry and the independently reachable path.
        // The latter aliases the callback field; it must not acquire the
        // loader's serialized-input fact when it calls or falls into Entry.
        for (bool FromPrefix : {false, true}) {
          auto Executable = Image;
          if (FromPrefix)
            Executable.Entry = kBytecodeStart;
          auto Selected = analyze(Executable);
          ASSERT_TRUE(static_cast<bool>(Selected))
              << llvm::toString(Selected.takeError());
          ExecutionEnvironment Environment;
          Environment.Memory.push_back(
              {kInputStart, std::vector<uint8_t>(8), true, "input"});
          auto Raw = executeRaw(*Selected, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, Loop || FromPrefix ? 43u : 42u);
        }
      }
}

TEST(SBFCallXContexts, EqualLoadedFieldsRefineOnlyUnchangedMemory) {
  enum class Change { None, Copy, Store, UnknownStore, Call, Base, Narrow };
  for (Change Mutation :
       {Change::None, Change::Copy, Change::Store, Change::UnknownStore,
        Change::Call, Change::Base, Change::Narrow})
    for (bool Immediate : {false, true})
      for (bool EqualTaken : {false, true})
        for (bool SwapOperands : {false, true})
          for (bool ReverseEdges : {false, true}) {
            if (Immediate && SwapOperands)
              continue;
            SCOPED_TRACE(::testing::Message() << "change=" << unsigned(Mutation)
                                              << ", immediate=" << Immediate
                                              << ", taken=" << EqualTaken
                                              << ", swap=" << SwapOperands
                                              << ", reverse=" << ReverseEdges);
            std::vector<test::EncodedInstruction> Text;
            const auto Literal = [&](unsigned Register, uint64_t Value) {
              const size_t Slot = Text.size();
              const auto Encoded = test::loadImm64(Register, Value);
              Text.insert(Text.end(), Encoded.begin(), Encoded.end());
              return Slot;
            };
            Literal(4, kInputStart);
            Text.push_back(encode(Opcode::LD_DW_REG, 5, 4));
            Text.push_back(encode(Opcode::ST_DW_REG, 10, 5, -16));
            Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -32, 1));
            Text.push_back(encode(Opcode::MOV64_REG, 8, 10));
            Text.push_back(encode(Opcode::ADD64_IMM, 8, 0, 0, -16));
            Text.push_back(encode(Mutation == Change::Narrow
                                      ? Opcode::LD_W_REG
                                      : Opcode::LD_DW_REG,
                                  Mutation == Change::Copy ? 9 : 6, 8));
            if (Mutation == Change::Copy)
              Text.push_back(encode(Opcode::MOV64_REG, 6, 9));
            std::optional<size_t> MutatorCall;
            if (Mutation == Change::Store)
              Text.push_back(encode(Opcode::ST_DW_IMM, 8, 0, 0, 1));
            if (Mutation == Change::UnknownStore) {
              Text.push_back(encode(Opcode::LD_DW_REG, 5, 4, 8));
              Text.push_back(encode(Opcode::ST_DW_IMM, 5, 0, 0, 1));
            }
            if (Mutation == Change::Call) {
              Text.push_back(encode(Opcode::MOV64_REG, 1, 8));
              MutatorCall = Text.size();
              Text.push_back({});
            }
            if (Mutation == Change::Base)
              Text.push_back(encode(Opcode::ADD64_IMM, 8, 0, 0, -16));
            Text.push_back(encode(Opcode::MOV64_IMM, 7));
            const size_t Guard = Text.size();
            Text.push_back({});
            std::optional<size_t> UnequalJump;
            if (EqualTaken) {
              UnequalJump = Text.size();
              Text.push_back({});
            }
            const size_t Equal = Text.size();
            Text.push_back(encode(Opcode::MOV64_REG, 1, 8));
            Text.push_back(encode(Opcode::MOV64_REG, 2, 10));
            Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, -24));
            const size_t Call = Text.size();
            Text.push_back({});
            Text.push_back(encode(Opcode::LD_DW_REG, 5, 10, -24));
            const size_t CallX = Text.size();
            Text.push_back(encode(Opcode::CALL_REG, 5));
            Text.push_back(encode(Opcode::EXIT));
            const size_t Unequal = Text.size();
            Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 99));
            Text.push_back(encode(Opcode::EXIT));
            const size_t Helper = Text.size();
            Text.push_back(encode(Opcode::LD_DW_REG, 3, 1));
            const size_t Select = Text.size();
            Text.push_back({});
            const size_t FirstLiteral = Literal(4, 0);
            Text.push_back(encode(Opcode::ST_DW_REG, 2, 4));
            Text.push_back(encode(Opcode::EXIT));
            const size_t SecondLiteral = Literal(4, 0);
            Text.push_back(encode(Opcode::ST_DW_REG, 2, 4));
            Text.push_back(encode(Opcode::EXIT));
            const size_t Mutator = Text.size();
            Text.push_back(encode(Opcode::ST_DW_IMM, 1, 0, 0, 1));
            Text.push_back(encode(Opcode::EXIT));
            const size_t FirstTarget = Text.size();
            Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
            Text.push_back(encode(Opcode::EXIT));
            const size_t SecondTarget = Text.size();
            Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
            Text.push_back(encode(Opcode::EXIT));
            Text[Guard] = encode(
                Immediate
                    ? (EqualTaken ? Opcode::JEQ64_IMM : Opcode::JNE64_IMM)
                    : (EqualTaken ? Opcode::JEQ64_REG : Opcode::JNE64_REG),
                SwapOperands ? 7 : 6, Immediate ? 0 : (SwapOperands ? 6 : 7),
                (EqualTaken ? Equal : Unequal) - Guard - 1);
            if (UnequalJump)
              Text[*UnequalJump] =
                  encode(Opcode::JA, 0, 0, Unequal - *UnequalJump - 1);
            Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Call - 1);
            Text[Select] =
                encode(Opcode::JNE64_IMM, 3, 0, SecondLiteral - Select - 1, 0);
            if (MutatorCall)
              Text[*MutatorCall] =
                  encode(Opcode::CALL_IMM, 0, 1, 0, Mutator - *MutatorCall - 1);
            for (const auto &[Slot, Target] :
                 {std::pair{FirstLiteral, FirstTarget},
                  std::pair{SecondLiteral, SecondTarget}}) {
              const auto Encoded =
                  test::loadImm64(4, kBytecodeStart + Target * 8);
              std::copy(Encoded.begin(), Encoded.end(), Text.begin() + Slot);
            }
            auto Program = analyze(test::makeImage(Text, {}));
            ASSERT_TRUE(static_cast<bool>(Program))
                << llvm::toString(Program.takeError());
            if (ReverseEdges)
              std::reverse(Program->Low.Edges.begin(),
                           Program->Low.Edges.end());
            auto Report = recoverCallXContexts(*Program);
            ASSERT_TRUE(static_cast<bool>(Report))
                << llvm::toString(Report.takeError());
            checkSummaryReferences(*Report);
            EXPECT_FALSE(Report->SpecializationBudgetExhausted);
            const bool FirstProven =
                llvm::any_of(Report->Contexts, [&](const auto &S) {
                  return !S.EntryPrefixOnly && S.CallXSlot == CallX &&
                         S.TargetSlot == FirstTarget;
                });
            const bool Unchanged =
                Mutation == Change::None || Mutation == Change::Copy;
            EXPECT_EQ(FirstProven, Unchanged);
            for (uint64_t Input : {0u, 1u}) {
              ExecutionEnvironment Environment;
              std::vector<uint8_t> Bytes(16);
              llvm::support::endian::write64le(
                  Bytes.data(),
                  Input | (Mutation == Change::Narrow ? uint64_t{1} << 32 : 0));
              llvm::support::endian::write64le(
                  Bytes.data() + 8,
                  initialFramePointer(Version::V3, SBFVMConfig{}) - 16);
              Environment.Memory.push_back(
                  {kInputStart, std::move(Bytes), true, "input"});
              auto Raw = executeRaw(*Program, std::move(Environment));
              ASSERT_TRUE(static_cast<bool>(Raw))
                  << llvm::toString(Raw.takeError());
              EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
              EXPECT_EQ(Raw->ReturnValue, Input ? 99u : Unchanged ? 42u : 43u);
            }
          }
}

TEST(SBFCallXContexts, DiscardedReturnOnlyCallsDoNotConsumeInputVariants) {
  enum class Use { Dead, Live, Store, Branch, Overwrite };
  for (Use Kind :
       {Use::Dead, Use::Live, Use::Store, Use::Branch, Use::Overwrite})
    for (unsigned Layers : {0u, 2u})
      for (bool ReverseEdges : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(Kind) << ", layers=" << Layers
                     << ", reverse=" << ReverseEdges);
        std::vector<test::EncodedInstruction> Text;
        std::vector<size_t> Calls;
        const size_t Count = kCallXContextSpecializationsPerEntry + 2;
        for (size_t I = 0; I < Count; ++I) {
          Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, I + 1));
          Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -8));
          Calls.push_back(Text.size());
          Text.push_back({});
        }
        if (Kind == Use::Store)
          Text.push_back(encode(Opcode::LD_DW_REG, 0, 10, -8));
        Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
        const size_t Address = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::MOV64_REG, 2, 0));
        Text.push_back(encode(Opcode::MOV64_REG, 0, 6));
        Text.push_back(encode(Opcode::CALL_REG, 2));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Wrapper = Text.size();
        Text.push_back({});
        if (Kind != Use::Live)
          Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Leaf = Text.size();
        if (Kind == Use::Store)
          Text.push_back(encode(Opcode::ST_DW_REG, 1, 0));
        if (Kind == Use::Branch) {
          Text.push_back(encode(Opcode::JEQ64_IMM, 0, 0, 1));
          Text.push_back(encode(Opcode::EXIT));
        }
        if (Kind == Use::Overwrite)
          Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
        Text.push_back(encode(Opcode::EXIT));
        Text[Wrapper] = encode(Opcode::CALL_IMM, 0, 1, 0, Leaf - Wrapper - 1);
        std::vector<size_t> Entries{Wrapper};
        for (unsigned I = 0; I < Layers; ++I) {
          const size_t Entry = Text.size();
          Text.push_back(
              encode(Opcode::CALL_IMM, 0, 1, 0,
                     static_cast<int32_t>(Entries.back()) - Entry - 1));
          Text.push_back(encode(Opcode::EXIT));
          Entries.push_back(Entry);
        }
        for (size_t Slot : Calls)
          Text[Slot] =
              encode(Opcode::CALL_IMM, 0, 1, 0, Entries.back() - Slot - 1);
        const size_t Materializer = Text.size();
        const size_t Target = Materializer + 3;
        const auto Words = test::loadImm64(0, kBytecodeStart + Target * 8);
        Text.insert(Text.end(), Words.begin(), Words.end());
        Text.push_back(encode(Opcode::EXIT));
        Text.push_back(encode(Opcode::EXIT));
        Text[Address] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Materializer - Address - 1);
        auto Program = analyze(test::makeImage(Text, {}));
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        if (ReverseEdges)
          std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        checkSummaryReferences(*Report);
        ASSERT_FALSE(Report->Summaries.empty());
        EXPECT_FALSE(Report->SpecializationBudgetExhausted);
        const bool ReadsInput =
            Kind == Use::Live || Kind == Use::Store || Kind == Use::Branch;
        for (const auto &Summary : Report->Summaries)
          if (llvm::is_contained(Entries, Summary.EntrySlot)) {
            EXPECT_EQ(bool(Summary.InputRegisterMask & 1), ReadsInput);
            EXPECT_EQ(bool(Summary.ReuseInputRegisterMask & 1), ReadsInput);
          }
        if (!ReadsInput) {
          std::set<size_t> Variants;
          size_t Specialized = 0;
          for (const auto &Edge : Report->SummaryEdges)
            if (llvm::is_contained(Calls, Edge.CallerSlot) &&
                !Edge.GeneralizedInputs) {
              ++Specialized;
              ASSERT_TRUE(Edge.CalleeSummaryID);
              Variants.insert(*Edge.CalleeSummaryID);
            }
          EXPECT_EQ(Specialized, Count);
          EXPECT_EQ(Variants.size(), 1u);
        }
        auto Raw = executeRaw(*Program);
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue,
                  Kind == Use::Live || Kind == Use::Store ? Count : 42u);
      }
}

TEST(SBFCallXContexts, TaggedPayloadLoadsKeepTheirEarlierMemoryEpoch) {
  enum class Change { None, Copy, Clobber, Base, Store, Call, Narrow };
  for (Change Mutation :
       {Change::None, Change::Copy, Change::Clobber, Change::Base,
        Change::Store, Change::Call, Change::Narrow})
    for (unsigned Bytes : {1u, 8u})
      for (bool ReverseEdges : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(Mutation) << ", bytes=" << Bytes
                     << ", reverse=" << ReverseEdges);
        std::vector<test::EncodedInstruction> Text;
        const auto Literal = [&](unsigned Register, uint64_t Value) {
          const size_t Slot = Text.size();
          const auto Words = test::loadImm64(Register, Value);
          Text.insert(Text.end(), Words.begin(), Words.end());
          return Slot;
        };
        struct CallbackLiteral {
          size_t Slot;
          unsigned Register;
          bool First;
        };
        std::vector<CallbackLiteral> Callbacks;
        const auto Callback = [&](unsigned Register, bool First) {
          Callbacks.push_back({Literal(Register, 0), Register, First});
        };
        const Opcode LoadTag =
            Bytes == 1 ? Opcode::LD_B_REG : Opcode::LD_DW_REG;
        const Opcode StoreTag =
            Bytes == 1 ? Opcode::ST_B_REG : Opcode::ST_DW_REG;
        Callback(4, false);
        Text.push_back(encode(Opcode::ST_DW_REG, 10, 4, -16));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -32));
        const size_t Call = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::MOV64_REG, 8, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 8, 0, 0, -24));
        Text.push_back(encode(Mutation == Change::Narrow ? Opcode::LD_W_REG
                                                         : Opcode::LD_DW_REG,
                              6, 8));
        unsigned Carrier = 6;
        std::optional<size_t> MutatorCall;
        if (Mutation == Change::Copy) {
          Text.push_back(encode(Opcode::MOV64_REG, 9, 6));
          Carrier = 9;
        } else if (Mutation == Change::Clobber) {
          Literal(4, kInputStart);
          Text.push_back(encode(Opcode::LD_DW_REG, 6, 4, 8));
        } else if (Mutation == Change::Base) {
          Text.push_back(encode(Opcode::ADD64_IMM, 8, 0, 0, 8));
        } else if (Mutation == Change::Store) {
          Callback(4, false);
          Text.push_back(encode(Opcode::ST_DW_REG, 8, 4));
        } else if (Mutation == Change::Call) {
          Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -32));
          MutatorCall = Text.size();
          Text.push_back({});
        }
        Text.push_back(encode(LoadTag, 7, 10, -32));
        const size_t Branch = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::MOV64_REG, 2, Carrier));
        if (Mutation == Change::Narrow) {
          Literal(3, kBytecodeStart);
          Text.push_back(encode(Opcode::ADD64_REG, 2, 3));
        }
        const size_t CallX = Text.size();
        Text.push_back(encode(Opcode::CALL_REG, 2));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Success = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 5));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Producer = Text.size();
        Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
        Literal(3, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 3));
        const size_t Select = Text.size();
        Text.push_back({});
        Callback(2, false);
        Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
        Text.push_back(encode(Opcode::MOV64_IMM, 1));
        const size_t Join = Text.size();
        Text.push_back({});
        const size_t Error = Text.size();
        Callback(2, true);
        Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
        Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, 1));
        const size_t Tail = Text.size();
        Text.push_back(encode(StoreTag, 6, 1));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Mutator = Text.size();
        Text.push_back(encode(Bytes == 1 ? Opcode::ST_B_IMM : Opcode::ST_DW_IMM,
                              1, 0, 0, 1));
        Text.push_back(encode(Opcode::EXIT));
        const size_t A = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
        Text.push_back(encode(Opcode::EXIT));
        const size_t B = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
        Text.push_back(encode(Opcode::EXIT));
        Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Producer - Call - 1);
        Text[Branch] = encode(Opcode::JEQ64_IMM, 7, 0, Success - Branch - 1);
        Text[Select] = encode(Opcode::JNE64_IMM, 2, 0, Error - Select - 1);
        Text[Join] = encode(Opcode::JA, 0, 0, Tail - Join - 1);
        if (MutatorCall)
          Text[*MutatorCall] =
              encode(Opcode::CALL_IMM, 0, 1, 0, Mutator - *MutatorCall - 1);
        for (const auto &[Slot, Register, First] : Callbacks) {
          const auto Words =
              test::loadImm64(Register, kBytecodeStart + (First ? A : B) * 8);
          std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
        }
        auto Program = analyze(test::makeImage(Text, {}));
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        if (ReverseEdges)
          std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        checkSummaryReferences(*Report);
        EXPECT_FALSE(Report->SpecializationBudgetExhausted);
        const auto Proven = [&](size_t Target) {
          return llvm::any_of(Report->Contexts, [&](const auto &Site) {
            return !Site.EntryPrefixOnly && Site.CallXSlot == CallX &&
                   Site.TargetSlot == Target;
          });
        };
        if (Mutation == Change::None || Mutation == Change::Copy)
          EXPECT_TRUE(Proven(A));
        if (Mutation == Change::Clobber || Mutation == Change::Call)
          EXPECT_FALSE(Proven(A));
        else
          EXPECT_FALSE(Proven(B));
        EXPECT_FALSE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
          return Site.CallXSlot == CallX &&
                 Site.Result == CallXContextResult::InvalidTarget;
        }));
        for (uint64_t Flag : {0u, 1u}) {
          ExecutionEnvironment Environment;
          std::vector<uint8_t> Input(16);
          llvm::support::endian::write64le(Input.data(), Flag);
          llvm::support::endian::write64le(Input.data() + 8,
                                           kBytecodeStart + B * 8);
          Environment.Memory.push_back(
              {kInputStart, Input, false, "tag input"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          const bool TakesCall = Flag || Mutation == Change::Call;
          const bool Other = Mutation == Change::Clobber ||
                             (Mutation == Change::Call && !Flag);
          EXPECT_EQ(Raw->ReturnValue, !TakesCall ? 5u : Other ? 43u : 42u);
        }
      }
}

TEST(SBFStoredRegisterMemory, OverflowingCapturesKeepOnlyProvenNumericBounds) {
  for (unsigned Mode = 0; Mode != 6; ++Mode)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Mode << ", reverse=" << Reverse);
      auto Addresses = std::make_shared<SymbolicAddressSpace>(
          3, Mode == 3 ? UnsignedValueRange{0, UINT64_MAX}
                       : UnsignedValueRange{kHeapStart + 8, UINT64_MAX});
      const int64_t Offset =
          Mode == 1   ? -static_cast<int64_t>(kHeapStart - kStackStart)
          : Mode == 2 ? 8
                      : 0;
      const auto Capture = [&](uint64_t I) {
        return RegisterValue{Mode == 5 ? RegisterValue::Kind::StackAddress
                                       : RegisterValue::Kind::SymbolicAddress,
                             I, Offset};
      };
      ProvenValueSet Left(RegisterValue{RegisterValue::Kind::Constant, 0, 0});
      Left.join(RegisterValue{RegisterValue::Kind::Constant, 8, 0});
      Left.join(Capture(0));
      Left.join(Capture(1));
      ASSERT_EQ(Left.values().size(), kCallXContextValueAlternatives);
      ProvenValueSet Right(Capture(1));
      Right.join(Capture(2));
      StoredRegisterMemory First, Second;
      for (auto *Memory : {&First, &Second}) {
        if (Mode != 4)
          Memory->setSymbolicAddresses(Addresses);
        Memory->setFrameTranslationLimit(4096);
      }
      First.writeValues(kStackStart + 32, Reverse ? Right : Left);
      Second.writeValues(kStackStart + 32, Reverse ? Left : Right);
      First.joinValues(Second);
      const auto Joined = First.readValues(kStackStart + 32);
      if (Mode < 2) {
        EXPECT_EQ(Joined.values().size(), 3u);
        EXPECT_TRUE(llvm::is_contained(
            Joined.values(),
            unsignedRangeValue({kHeapStart + 8 + static_cast<uint64_t>(Offset),
                                UINT64_MAX + static_cast<uint64_t>(Offset)})));
      } else {
        EXPECT_TRUE(Joined.unknown());
      }
      if (Mode < 2)
        for (uint64_t I = 0; I != 3; ++I) {
          Second.writeValues(kStackStart + 32, Capture(I));
          First.joinValues(Second);
          EXPECT_EQ(First.readValues(kStackStart + 32), Joined);
        }
      const RegisterValue Number{RegisterValue::Kind::Constant, 42, 0};
      First.write(kStackStart + 64, Number, MemoryCoordinate::FrameRelative);
      if (Joined.unknown())
        First.invalidate(RegisterValue{}, 8);
      else
        for (const auto &Value : Joined.values())
          First.invalidate(Value, 8);
      EXPECT_EQ(First.read(kStackStart + 64, MemoryCoordinate::FrameRelative),
                Mode == 0 ? Number : RegisterValue{});
    }
}

TEST(SBFCallXContexts, OverflowingCapturedWritesPreserveDisjointStackFields) {
  for (int16_t Offset : {0, 42})
    for (bool ThroughCallee : {false, true})
      for (unsigned Mode = 0; Mode != 4; ++Mode)
        for (bool Reverse : {false, true}) {
          SCOPED_TRACE(::testing::Message() << Mode << ", reverse=" << Reverse
                                            << ", offset=" << Offset
                                            << ", callee=" << ThroughCallee);
          std::vector<test::EncodedInstruction> Text;
          const auto Literal = [&](unsigned R, uint64_t Value) {
            const auto Slots = test::loadImm64(R, Value);
            const size_t At = Text.size();
            Text.insert(Text.end(), Slots.begin(), Slots.end());
            return At;
          };
          Literal(7, kInputStart);
          Text.push_back(encode(Opcode::LD_DW_REG, 8, 7));
          const size_t FirstLiteral = Literal(2, 0);
          Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -8));
          std::array<size_t, 4> Branches;
          for (auto &Slot : Branches) {
            Slot = Text.size();
            Text.push_back({});
          }
          std::array<size_t, 5> Paths, Exits;
          std::array<size_t, 3> Calls, Nulls;
          for (unsigned I : {4u, 0u, 1u, 2u, 3u}) {
            Paths[I] = Text.size();
            if (I < 3) {
              Text.push_back(encode(Opcode::MOV64_REG, 1, 7));
              Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, (I + 1) * 8));
              Calls[I] = Text.size();
              Text.push_back({});
              Nulls[I] = Text.size();
              Text.push_back({});
              if (Mode == 2) {
                Literal(2, kStackStart - kHeapStart);
                Text.push_back(encode(Opcode::ADD64_REG, 0, 2));
              }
            } else {
              Text.push_back(
                  encode(Opcode::MOV64_IMM, 0, 0, 0, I == 3 ? 0 : 8));
            }
            Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
            Text.push_back(encode(Opcode::ST_DW_REG, 10, 0, -16));
            Exits[I] = Text.size();
            Text.push_back({});
          }
          const size_t Join = Text.size();
          // The selector skips invalid sentinel stores at runtime without
          // refining the joined pointer. Both a register and a spilled field
          // must retain their numeric alternatives at this five-way forward
          // join.
          const size_t Skip = Text.size();
          Text.push_back({});
          const size_t SecondLiteral = Literal(2, 0);
          std::optional<size_t> StoreCall;
          if (ThroughCallee) {
            Text.push_back(encode(Opcode::MOV64_REG, 3, 2));
            Text.push_back(encode(Opcode::MOV64_REG, 1, 6));
            Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -16));
            StoreCall = Text.size();
            Text.push_back({});
          } else {
            Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, Offset));
            Text.push_back(encode(Opcode::LD_DW_REG, 9, 10, -16));
            Text.push_back(encode(Opcode::ST_DW_REG, 9, 2, Offset));
          }
          const size_t Finish = Text.size();
          Text.push_back(encode(Opcode::LD_DW_REG, 5, 10, -8));
          const size_t CallX = Text.size();
          Text.push_back(encode(Opcode::CALL_REG, 5));
          Text.push_back(encode(Opcode::EXIT));
          const size_t Error = Text.size();
          Text.push_back(encode(Opcode::MOV64_IMM, 0));
          Text.push_back(encode(Opcode::EXIT));
          if (StoreCall) {
            const size_t Helper = Text.size();
            Text.push_back(encode(Opcode::ST_DW_REG, 1, 3, Offset));
            Text.push_back(encode(Opcode::ST_DW_REG, 2, 3, Offset));
            Text.push_back(encode(Opcode::EXIT));
            Text[*StoreCall] =
                encode(Opcode::CALL_IMM, 0, 1, 0, Helper - *StoreCall - 1);
          }
          const size_t Allocate = Text.size();
          Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
          if (Mode != 3) {
            Literal(2, (Mode == 1 ? kStackStart : kHeapStart) + 8);
            Text.push_back(encode(Opcode::JGE64_REG, 0, 2, 1));
            Text.push_back(encode(Opcode::MOV64_IMM, 0));
          }
          Text.push_back(encode(Opcode::EXIT));
          const size_t FirstTarget = Text.size();
          Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
          Text.push_back(encode(Opcode::EXIT));
          const size_t SecondTarget = Text.size();
          Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
          Text.push_back(encode(Opcode::EXIT));
          for (unsigned I = 0; I != Branches.size(); ++I)
            Text[Branches[I]] =
                encode(Opcode::JEQ64_IMM, 8, 0, Paths[I] - Branches[I] - 1, I);
          for (size_t Slot : Calls)
            Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Allocate - Slot - 1);
          for (size_t Slot : Nulls)
            Text[Slot] = encode(Opcode::JEQ64_IMM, 0, 0, Error - Slot - 1);
          for (size_t Slot : Exits)
            Text[Slot] = encode(Opcode::JA, 0, 0, Join - Slot - 1);
          Text[Skip] = encode(Opcode::JGE64_IMM, 8, 0, Finish - Skip - 1, 3);
          for (const auto &[Slot, Target] :
               {std::pair{FirstLiteral, FirstTarget},
                std::pair{SecondLiteral, SecondTarget}}) {
            const auto Slots = test::loadImm64(2, kBytecodeStart + Target * 8);
            std::copy(Slots.begin(), Slots.end(), Text.begin() + Slot);
          }
          auto Program = analyze(test::makeImage(Text, {}));
          ASSERT_TRUE(static_cast<bool>(Program))
              << llvm::toString(Program.takeError());
          if (Reverse)
            std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
          auto Report = recoverCallXContexts(*Program);
          ASSERT_TRUE(static_cast<bool>(Report))
              << llvm::toString(Report.takeError());
          checkSummaryReferences(*Report);
          EXPECT_EQ(llvm::any_of(Report->Contexts,
                                 [&](const auto &Site) {
                                   return Site.CallXSlot == CallX &&
                                          Site.TargetSlot == FirstTarget;
                                 }),
                    Mode == 0);
          const uint64_t Saved =
              initialFramePointer(Program->Low.TheVersion, Program->Config) - 8;
          for (unsigned Select = 0; Select != 5; ++Select)
            for (bool Alias : {false, true}) {
              SCOPED_TRACE(::testing::Message()
                           << "selector=" << Select << ", alias=" << Alias);
              ExecutionEnvironment Environment;
              std::vector<uint8_t> Input(32);
              llvm::support::endian::write64le(Input.data(), Select);
              for (unsigned I = 0; I != 3; ++I) {
                uint64_t Pointer = Mode != 0 && Alias
                                       ? Saved - Offset
                                       : kHeapStart + 64 + (Alias ? 0 : I * 16);
                if (Mode == 2)
                  Pointer += kHeapStart - kStackStart;
                llvm::support::endian::write64le(Input.data() + (I + 1) * 8,
                                                 Pointer);
              }
              Environment.Memory.push_back(
                  {kInputStart, Input, false, "input"});
              Environment.Memory.push_back(
                  {kHeapStart, std::vector<uint8_t>(256), true, "objects"});
              auto Raw = executeRaw(*Program, std::move(Environment));
              ASSERT_TRUE(static_cast<bool>(Raw))
                  << llvm::toString(Raw.takeError());
              EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
              EXPECT_EQ(Raw->ReturnValue,
                        Mode != 0 && Alias && Select < 3 ? 43u : 42u);
            }
        }
}

TEST(SBFStoredRegisterMemory, OffsetStoreRangesKeepModularFootprints) {
  using namespace callx_detail;
  auto Program = analyze(test::makeImage({encode(Opcode::EXIT)}, {}));
  ASSERT_TRUE(static_cast<bool>(Program))
      << llvm::toString(Program.takeError());
  for (unsigned Mode = 0; Mode != 4; ++Mode) {
    SCOPED_TRACE(Mode);
    const auto Base =
        Mode == 0   ? unsignedRangeValue({kHeapStart + 8, UINT64_MAX})
        : Mode == 1 ? unsignedRangeValue({0, kStackStart - 64})
        : Mode == 2 ? unsignedRangeValue({kStackStart + 8, UINT64_MAX})
                    : RegisterValue{};
    const int16_t Offset = Mode == 1 ? -42 : 42;
    const RegisterValue Number{RegisterValue::Kind::Constant, 42, 0};
    MachineState State;
    State.Registers[1] = Base;
    State.Registers[2] = Number;
    State.Scratch.Values.setFrameTranslationLimit(4096);
    State.Scratch.Values.write(kStackStart + 64, Number,
                               MemoryCoordinate::FrameRelative);
    auto ThroughSummary = State.Scratch;
    auto ThroughIncluded = State.Scratch;
    MedInstruction Store;
    Store.Op = Operation::Store;
    Store.Form = OperandForm::StoreReg;
    Store.Width = kBitsPerByte;
    Store.Dst = 1;
    Store.Src = 2;
    Store.Offset = Offset;
    applyTransfer(Store, State, Program->ExecutableImage,
                  ScratchLoadPolicy::ProvenValues);
    EXPECT_EQ(State.Scratch.Values.read(kStackStart + 64,
                                        MemoryCoordinate::FrameRelative),
              Mode < 2 ? Number : RegisterValue{});
    MemoryEffects Effects;
    Effects.addWrite({RegisterValue::Kind::StackAddress, 2, Offset}, 1,
                     *Program);
    std::array<ProvenValueSet, kRegisterCount> Entry{};
    Entry[1] = Base;
    Effects.invalidate(ThroughSummary, Entry, *Program);
    EXPECT_EQ(ThroughSummary.Values.read(kStackStart + 64,
                                         MemoryCoordinate::FrameRelative),
              Mode < 2 ? Number : RegisterValue{});
    RegisterState Actual{};
    Actual[1] = Base;
    MemoryEffects Included;
    Included.include(Effects, Actual, *Program);
    EXPECT_EQ(Included.Unknown, Mode == 3);
    Included.invalidate(ThroughIncluded, Entry, *Program);
    EXPECT_EQ(ThroughIncluded.Values.read(kStackStart + 64,
                                          MemoryCoordinate::FrameRelative),
              Mode < 2 ? Number : RegisterValue{});
    // Scalar facts still use the nonwrapping domain. An access footprint is
    // allowed to wrap, but it must not become an ordinary scalar range fact.
    MedInstruction Add;
    Add.Op = Operation::Add;
    Add.Form = OperandForm::DstImm;
    Add.Width = kDoubleWordBitWidth;
    Add.Dst = 1;
    Add.Immediate = static_cast<uint64_t>(Offset);
    Add.Semantics.Immediate = ImmediateExtension::Full64;
    Add.Semantics.WritesDestination = true;
    applyRegisterTransfer(Add, Actual, Program->ExecutableImage);
    EXPECT_EQ(Actual[1], RegisterValue{});
  }
}

TEST(SBFCallXContexts, TaggedReturnPayloadsCrossAcyclicCleanup) {
  size_t RawExecutions = 0;
  for (unsigned Bytes : {1u, 8u})
    for (unsigned Mode = 0; Mode < 5; ++Mode)
      for (bool EarlyTail : {false, true})
        for (bool ReverseEdges : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << "bytes=" << Bytes << ", mode=" << Mode
                       << ", early tail=" << EarlyTail
                       << ", reverse=" << ReverseEdges);
          const Opcode LoadTag =
              Bytes == 1 ? Opcode::LD_B_REG : Opcode::LD_DW_REG;
          const Opcode StoreTag =
              Bytes == 1 ? Opcode::ST_B_REG : Opcode::ST_DW_REG;
          std::vector<test::EncodedInstruction> Text;
          const auto Literal = [&](unsigned R, uint64_t Value) {
            const size_t Slot = Text.size();
            const auto Words = test::loadImm64(R, Value);
            Text.insert(Text.end(), Words.begin(), Words.end());
            return Slot;
          };
          Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -32));
          const size_t Call = Text.size();
          Text.push_back({});
          Text.push_back(encode(LoadTag, 7, 10, -32));
          const size_t IsError = Text.size();
          Text.push_back({});
          Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -24));
          const size_t CallX = Text.size();
          Text.push_back(encode(Opcode::CALL_REG, 2));
          Text.push_back(encode(Opcode::EXIT));
          const size_t Done = Text.size();
          Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 5));
          Text.push_back(encode(Opcode::EXIT));
          const size_t Producer = Text.size();
          Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
          Literal(3, kInputStart);
          Text.push_back(encode(Opcode::LD_B_REG, 9, 3));
          if (Mode == 2)
            Text.push_back(encode(Opcode::LD_DW_REG, 8, 3, 8));
          Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -8, 19));
          std::optional<size_t> ClobberTarget;
          const auto EmitCleanup = [&] {
            const size_t Start = Text.size();
            Text.push_back(encode(Opcode::LD_DW_REG, 5, 10, -8));
            Text.push_back(encode(Opcode::ADD64_IMM, 5, 0, 0, -1));
            Text.push_back(encode(Opcode::ST_DW_REG, 10, 5, -8));
            if (Mode == 2 || Mode == 3) {
              ClobberTarget = Literal(2, 0);
              Text.push_back(Mode == 2 ? encode(Opcode::ST_DW_REG, 8, 2)
                                       : encode(Opcode::ST_DW_REG, 6, 2, 8));
            }
            if (Mode == 4)
              Text.push_back(
                  encode(Bytes == 1 ? Opcode::ST_B_IMM : Opcode::ST_DW_IMM, 6,
                         0, 0, 1));
            Text.push_back(encode(Opcode::EXIT));
            return Start;
          };
          std::optional<size_t> Cleanup;
          if (EarlyTail) {
            const size_t Skip = Text.size();
            Text.push_back({});
            Cleanup = EmitCleanup();
            Text[Skip] = encode(Opcode::JA, 0, 0, Text.size() - Skip - 1);
          }
          const size_t Select = Text.size();
          Text.push_back({});
          const size_t SuccessTarget = Literal(2, 0);
          Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
          Text.push_back(Mode == 1 ? encode(Opcode::MOV64_REG, 1, 9)
                                   : encode(Opcode::MOV64_IMM, 1, 0, 0, 1));
          Text.push_back(encode(StoreTag, 6, 1));
          const size_t Join = Text.size();
          Text.push_back({});
          const size_t Error = Text.size();
          const size_t ErrorTarget = Literal(2, 0);
          Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
          Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, 2));
          Text.push_back(encode(StoreTag, 6, 1));
          if (EarlyTail) {
            const auto Back = static_cast<int64_t>(*Cleanup) -
                              static_cast<int64_t>(Text.size()) - 1;
            Text.push_back(encode(Opcode::JA, 0, 0, Back));
          } else {
            Cleanup = EmitCleanup();
          }
          const size_t A = Text.size();
          Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
          Text.push_back(encode(Opcode::EXIT));
          const size_t B = Text.size();
          Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
          Text.push_back(encode(Opcode::EXIT));
          Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Producer - Call - 1);
          Text[IsError] =
              encode(Opcode::JEQ64_IMM, 7, 0, Done - IsError - 1, 2);
          Text[Select] = encode(Opcode::JEQ64_IMM, 9, 0, Error - Select - 1, 2);
          Text[Join] = encode(Opcode::JA, 0, 0,
                              static_cast<int64_t>(*Cleanup) -
                                  static_cast<int64_t>(Join) - 1);
          for (const auto &[Slot, Target] :
               {std::pair{SuccessTarget, A}, std::pair{ErrorTarget, B},
                std::pair{ClobberTarget.value_or(ErrorTarget), B}}) {
            const auto Words = test::loadImm64(2, kBytecodeStart + Target * 8);
            std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
          }
          auto Program = analyze(test::makeImage(Text, {}));
          ASSERT_TRUE(static_cast<bool>(Program))
              << llvm::toString(Program.takeError());
          if (ReverseEdges)
            std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
          auto Report = recoverCallXContexts(*Program);
          ASSERT_TRUE(static_cast<bool>(Report))
              << llvm::toString(Report.takeError());
          checkSummaryReferences(*Report);
          EXPECT_FALSE(Report->SpecializationBudgetExhausted);
          bool HasExpected = false;
          for (const auto &Site : Report->Contexts)
            if (Site.CallXSlot == CallX) {
              if (Mode == 0 || Mode == 1 || Mode == 3) {
                HasExpected |= Site.TargetSlot == (Mode == 3 ? B : A);
                if (Site.TargetSlot)
                  EXPECT_EQ(*Site.TargetSlot, Mode == 3 ? B : A);
              } else
                EXPECT_FALSE(Site.TargetAddress);
            }
          if (Mode == 0 || Mode == 1 || Mode == 3)
            EXPECT_TRUE(HasExpected);
          for (unsigned Tag : {0u, 1u, 2u, 3u, 255u})
            for (unsigned Trial = 0; Trial < (Mode == 2 ? 2u : 1u); ++Trial) {
              SCOPED_TRACE(::testing::Message()
                           << "tag=" << Tag << ", trial=" << Trial);
              std::vector<uint8_t> Input(16);
              Input[0] = Tag;
              llvm::support::endian::write64le(
                  Input.data() + 8,
                  Trial ? initialFramePointer(Program->Low.TheVersion,
                                              Program->Config) -
                              24
                        : kHeapStart + 64);
              ExecutionEnvironment Environment;
              Environment.Memory.push_back(
                  {kInputStart, Input, false, "input"});
              Environment.Memory.push_back(
                  {kHeapStart, std::vector<uint8_t>(128), true, "heap"});
              auto Raw = executeRaw(*Program, std::move(Environment));
              ASSERT_TRUE(static_cast<bool>(Raw))
                  << llvm::toString(Raw.takeError());
              ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
              ++RawExecutions;
              const bool Calls = Mode == 4 || Tag != 2;
              const bool CallsB =
                  Mode == 3 || (Mode == 2 && Trial) || (Mode == 4 && Tag == 2);
              EXPECT_EQ(Raw->ReturnValue, !Calls ? 5u : CallsB ? 43u : 42u);
              EXPECT_EQ(llvm::count_if(Raw->Trace,
                                       [&](const auto &Step) {
                                         return Step.Slot == CallX;
                                       }),
                        Calls ? 1 : 0);
            }
        }
  RecordProperty("raw_executions", std::to_string(RawExecutions));
}

TEST(SBFStoredRegisterMemory, NarrowStoresRetainOnlyFittingNumericBounds) {
  for (unsigned Bytes : {1u, 2u, 4u}) {
    SCOPED_TRACE(Bytes);
    const uint64_t Mask = (uint64_t{1} << (Bytes * 8)) - 1;
    const RegisterValue Address{RegisterValue::Kind::Constant, kStackStart + 32,
                                0};
    for (const auto &Value :
         {unsignedRangeValue({0, Mask}), unsignedRangeValue({1, Mask - 1}),
          unsignedRangeValue({0, Mask + 1}),
          unsignedRangeValue({Mask, UINT64_MAX}),
          unsignedRangeValue({UINT64_MAX, 1}),
          RegisterValue{RegisterValue::Kind::StackAddress, 1, 0},
          RegisterValue{RegisterValue::Kind::SymbolicAddress, 0, 0},
          RegisterValue{}}) {
      StoredRegisterMemory Memory;
      Memory.writeValues(Address, Value, Bytes);
      const auto Bounds = unsignedValueRange(Value);
      const bool Fits =
          Bounds && Bounds->Lower <= Bounds->Upper && Bounds->Upper <= Mask;
      EXPECT_EQ(Memory.readValues(Address, Bytes),
                Fits ? ProvenValueSet(Value) : ProvenValueSet{});
    }
    const auto Low = unsignedRangeValue({0, 3});
    const auto High = unsignedRangeValue({8, Mask});
    ProvenValueSet Alternatives(Low);
    Alternatives.join(High);
    StoredRegisterMemory Memory;
    Memory.writeValues(Address, Alternatives, Bytes);
    EXPECT_EQ(Memory.readValues(Address, Bytes), Alternatives);
    // A write to any part of the tag destroys the old full-width relation.
    Memory.writeValues(Address,
                       RegisterValue{RegisterValue::Kind::Constant, 9, 0}, 1);
    EXPECT_NE(Memory.readValues(Address, Bytes), Alternatives);
    Alternatives.join(RegisterValue{RegisterValue::Kind::StackAddress, 1, 0});
    EXPECT_TRUE(truncateStoredValue(Alternatives, Bytes).unknown());
  }
}

TEST(SBFCallXContexts, TaggedRangesExcludeOnlyImpossiblePayloads) {
  size_t RawExecutions = 0;
  for (unsigned Mode = 0; Mode < 10; ++Mode) {
    SCOPED_TRACE(Mode);
    const bool Ambiguous = Mode == 1 || Mode == 2 || Mode == 3;
    const uint64_t Sentinel = Mode == 1   ? 128
                              : Mode == 2 ? 0x100000080ULL
                              : Mode == 3 ? 0
                              : Mode == 7 ? 255
                                          : 256;
    const uint64_t Shift = Mode == 3   ? 0xffffffffULL
                           : Mode == 6 ? 512
                           : Mode == 7 ? 256
                                       : 0;
    const uint64_t Compare = Mode == 2                ? 128
                             : Mode == 4 || Mode == 7 ? 255
                             : Mode == 6              ? 512
                                                      : Sentinel;
    const Opcode BranchOpcode = Mode == 2 || Mode == 3 ? Opcode::JEQ32_IMM
                                : Mode == 4            ? Opcode::JGT64_IMM
                                : Mode == 5            ? Opcode::JGE64_IMM
                                : Mode == 6            ? Opcode::JLT64_IMM
                                : Mode == 7            ? Opcode::JLE64_IMM
                                : Mode == 8            ? Opcode::JGT64_REG
                                : Mode == 9            ? Opcode::JNE64_IMM
                                                       : Opcode::JEQ64_IMM;
    std::vector<test::EncodedInstruction> Text;
    const auto Literal = [&](unsigned R, uint64_t Value) {
      const size_t Slot = Text.size();
      const auto Words = test::loadImm64(R, Value);
      Text.insert(Text.end(), Words.begin(), Words.end());
      return Slot;
    };
    Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
    Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
    const size_t Call = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::LD_DW_REG, 7, 10, -16));
    if (Mode == 8)
      Literal(8, Compare);
    const size_t Branch = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 5));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Selected = Text.size();
    Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -8));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 2));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Producer = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
    Literal(3, kInputStart);
    Text.push_back(encode(Opcode::LD_B_REG, 9, 3));
    const size_t Choose = Text.size();
    Text.push_back({});
    const size_t FirstTarget = Literal(2, 0);
    Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
    Text.push_back(
        encode(Mode == 3 ? Opcode::LD_W_REG : Opcode::LD_B_REG, 1, 3, 8));
    if (Shift) {
      Literal(4, Shift);
      Text.push_back(encode(Opcode::ADD64_REG, 1, 4));
    }
    const size_t Join = Text.size();
    Text.push_back({});
    const size_t Second = Text.size();
    const size_t SecondTarget = Literal(2, 0);
    Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
    Literal(1, Sentinel);
    const size_t Tail = Text.size();
    Text.push_back(encode(Opcode::ST_DW_REG, 6, 1));
    Text.push_back(encode(Opcode::EXIT));
    const size_t A = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    const size_t B = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
    Text.push_back(encode(Opcode::EXIT));
    Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Producer - Call - 1);
    Text[Branch] = encode(BranchOpcode, Mode == 8 ? 8 : 7, Mode == 8 ? 7 : 0,
                          Selected - Branch - 1, Compare);
    Text[Choose] = encode(Opcode::JNE64_IMM, 9, 0, Second - Choose - 1);
    Text[Join] = encode(Opcode::JA, 0, 0, Tail - Join - 1);
    for (const auto &[Slot, Target] :
         {std::pair{FirstTarget, A}, std::pair{SecondTarget, B}}) {
      const auto Words = test::loadImm64(2, kBytecodeStart + Target * 8);
      std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
    }
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_FALSE(Report->SpecializationBudgetExhausted);
    bool HasExpected = false;
    const size_t ExpectedTarget = Mode == 8 || Mode == 9 ? A : B;
    for (const auto &Site : Report->Contexts)
      if (Site.CallXSlot == CallX) {
        if (Ambiguous)
          EXPECT_FALSE(Site.TargetSlot);
        else if (Site.TargetSlot) {
          EXPECT_EQ(*Site.TargetSlot, ExpectedTarget);
          HasExpected = true;
        }
      }
    if (!Ambiguous)
      EXPECT_TRUE(HasExpected);
    std::set<size_t> ObservedTargets;
    for (unsigned Selector : {0u, 1u})
      for (uint64_t Tag : {uint64_t{0}, uint64_t{1}, uint64_t{128},
                           uint64_t{255}, uint64_t{UINT32_MAX}}) {
        std::vector<uint8_t> Input(16);
        Input[0] = Selector;
        llvm::support::endian::write64le(Input.data() + 8, Tag);
        ExecutionEnvironment Environment;
        Environment.Memory.push_back({kInputStart, Input, false, "tags"});
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        ++RawExecutions;
        for (size_t I = 0; I + 1 < Raw->Trace.size(); ++I)
          if (Raw->Trace[I].Slot == CallX)
            ObservedTargets.insert(Raw->Trace[I + 1].Slot);
      }
    EXPECT_EQ(ObservedTargets, Ambiguous ? (std::set<size_t>{A, B})
                                         : (std::set<size_t>{ExpectedTarget}));
  }
  RecordProperty("raw_executions", std::to_string(RawExecutions));
}

TEST(SBFCallXContexts, FiniteMemoryInputsKeepReturnBoundsAndFieldIdentity) {
  constexpr uint64_t CallbackAddress = kHeapStart + 904;
  size_t RawExecutions = 0;
  for (unsigned Mode = 0; Mode < 5; ++Mode) {
    SCOPED_TRACE(Mode);
    const bool MayAliasCallback = Mode == 2 || Mode == 3;
    std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
    const auto Literal = [&](unsigned R, uint64_t Value) {
      const size_t Slot = Text.size();
      const auto Words = test::loadImm64(R, Value);
      Text.insert(Text.end(), Words.begin(), Words.end());
      return Slot;
    };
    std::vector<size_t> Entries, Calls, TargetALiterals, TargetBLiterals;
    size_t FirstSite = 0, FieldSite = 0;
    for (unsigned I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
      const bool Last = I == kCallXContextSpecializationsPerEntry;
      Entries.push_back(Text.size());
      if (!Last) {
        Literal(2, kHeapStart + 512 + I * 64);
      } else {
        Literal(8, kInputStart);
        if (Mode == 2) {
          Text.push_back(encode(Opcode::LD_DW_REG, 2, 8, 16));
        } else {
          Text.push_back(encode(Opcode::LD_B_REG, 9, 8));
          Literal(2, kHeapStart + (Mode == 1 ? 8 : 128));
          const size_t Select = Text.size();
          Text.push_back({});
          Literal(2, Mode == 3 ? CallbackAddress + 40 : kHeapStart + 192);
          Text[Select] =
              encode(Opcode::JEQ64_IMM, 9, 0, Text.size() - Select - 1);
        }
        TargetALiterals.push_back(Literal(5, 0));
        // Use a tracked scratch cell; external input stores are not modeled.
        Literal(7, CallbackAddress);
        Text.push_back(encode(Opcode::ST_DW_REG, 7, 5));
      }
      Literal(3, kHeapStart);
      Text.push_back(encode(Opcode::ST_DW_REG, 3, 2));
      Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, 8 * (I + 1)));
      if (Last && Mode == 4) {
        Text.push_back(encode(Opcode::LD_B_REG, 9, 8, 1));
        Text.push_back(encode(Opcode::JEQ64_IMM, 9, 0, 1));
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, 8));
      }
      Calls.push_back(Text.size());
      Text.push_back({});
      const size_t Failed = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
      TargetBLiterals.push_back(Literal(5, 0));
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 5));
      if (Last) {
        Literal(8, CallbackAddress);
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 8));
        FirstSite = Text.size();
        Text.push_back(encode(Opcode::CALL_REG, 2));
      }
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 6));
      if (Last)
        FieldSite = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 2));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Done = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 5));
      Text.push_back(encode(Opcode::EXIT));
      Text[Failed] = encode(Opcode::JEQ64_IMM, 0, 0, Done - Failed - 1);
    }
    const size_t Helper = Text.size();
    Literal(3, kHeapStart);
    Text.push_back(encode(Opcode::LD_DW_REG, 0, 3));
    Text.push_back(encode(Opcode::MOV64_REG, 4, 0));
    Text.push_back(encode(Opcode::SUB64_REG, 0, 1));
    const size_t Underflow = Text.size();
    Text.push_back({});
    Literal(2, kHeapStart + 8);
    const size_t Exhausted = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::ST_DW_REG, 3, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Failed = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    Text[Underflow] = encode(Opcode::JGT64_REG, 0, 4, Failed - Underflow - 1);
    Text[Exhausted] = encode(Opcode::JLT64_REG, 0, 2, Failed - Exhausted - 1);
    const size_t A = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    const size_t B = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
    Text.push_back(encode(Opcode::EXIT));
    for (size_t Call : Calls)
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Call - 1);
    for (const auto &[Slots, Target] :
         {std::pair{TargetALiterals, A}, std::pair{TargetBLiterals, B}})
      for (size_t Slot : Slots) {
        const auto Words = test::loadImm64(5, kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      }
    auto Image = test::makeImage(Text, {});
    for (size_t Entry : Entries) {
      Symbol Function;
      Function.Name = "finite_memory_caller_" + std::to_string(Entry);
      Function.Addr = kBytecodeStart + Entry * kInstructionSize;
      Function.IsFunc = true;
      Image.Symbols.push_back(std::move(Function));
    }
    auto Program = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_FALSE(Report->SpecializationBudgetExhausted);
    EXPECT_EQ(llvm::count_if(Report->Summaries,
                             [&](const auto &S) {
                               return S.EntrySlot == Helper &&
                                      !S.GeneralizedInputs;
                             }),
              kCallXContextSpecializationsPerEntry);
    EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
      return Edge.CallerSlot == Calls.back() &&
             Edge.InputGeneralization ==
                 CallXInputGeneralization::SpecializationLimit;
    }));
    bool FirstProven = false, FieldProven = false;
    for (const auto &Site : Report->Contexts) {
      if (Site.CallXSlot == FirstSite && Site.TargetSlot) {
        EXPECT_FALSE(MayAliasCallback);
        EXPECT_EQ(*Site.TargetSlot, A);
        FirstProven = true;
      }
      if (!MayAliasCallback && Site.CallXSlot == FieldSite && Site.TargetSlot) {
        EXPECT_EQ(*Site.TargetSlot, B);
        FieldProven = true;
      }
    }
    if (!MayAliasCallback) {
      EXPECT_TRUE(FirstProven);
      EXPECT_TRUE(FieldProven);
    }
    Image.Entry = kBytecodeStart + Entries.back() * kInstructionSize;
    auto Selected = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Selected))
        << llvm::toString(Selected.takeError());
    for (unsigned Trial = 0; Trial < (Mode == 4 ? 4u : 2u); ++Trial) {
      std::vector<uint8_t> Input(64);
      Input[0] = Trial & 1;
      Input[1] = Trial >> 1;
      llvm::support::endian::write64le(
          Input.data() + 16, Trial ? CallbackAddress + 40 : kHeapStart + 192);
      ExecutionEnvironment Environment;
      Environment.Memory.push_back({kInputStart, Input, false, "input"});
      Environment.Memory.push_back(
          {kHeapStart, std::vector<uint8_t>(1024), true, "heap"});
      auto Raw = executeRaw(*Selected, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      ++RawExecutions;
      const bool Null = Mode == 1 && Trial == 0;
      EXPECT_EQ(Raw->ReturnValue, Null ? 5u : 43u);
      std::map<size_t, size_t> Actual;
      for (size_t I = 0; I + 1 < Raw->Trace.size(); ++I)
        if (Raw->Trace[I].Slot == FirstSite || Raw->Trace[I].Slot == FieldSite)
          Actual.emplace(Raw->Trace[I].Slot, Raw->Trace[I + 1].Slot);
      if (Null)
        EXPECT_TRUE(Actual.empty());
      else {
        EXPECT_EQ(Actual[FirstSite], MayAliasCallback && Trial ? B : A);
        EXPECT_EQ(Actual[FieldSite], B);
      }
    }
  }
  RecordProperty("raw_executions", std::to_string(RawExecutions));
}

TEST(SBFCallXContexts, ArithmeticCaptureProductsKeepNumericAddressBounds) {
  size_t RawExecutions = 0;
  for (unsigned Mode = 0; Mode != 3; ++Mode)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Mode << ", reverse=" << Reverse);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      Literal(7, kInputStart);
      const size_t InitialTarget = Literal(2, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -32));
      Text.push_back(encode(Opcode::LD_DW_REG, 9, 7));
      const size_t ChooseCall = Text.size();
      Text.push_back({});
      std::array<size_t, 2> Calls;
      Text.push_back(encode(Opcode::MOV64_REG, 1, 7));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, 8));
      Calls[0] = Text.size();
      Text.push_back({});
      const size_t SkipOtherCall = Text.size();
      Text.push_back({});
      const size_t OtherCall = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 1, 7));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, 8));
      Calls[1] = Text.size();
      Text.push_back({});
      const size_t Join = Text.size();
      const size_t Null = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
      Text.push_back(encode(Opcode::LD_DW_REG, 9, 7, 16));
      Text.push_back(encode(Opcode::MOV64_IMM, 8));
      const size_t ZeroOffset = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_IMM, 8, 0, 0, 8));
      const size_t EightOffset = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_IMM, 8, 0, 0, 16));
      const size_t Add = Text.size();
      // Two independently captured bases times three offsets exceed the
      // four-alternative domain. Numeric bounds still prove heap/stack
      // separation in mode 0; capture names themselves prove no separation.
      Text.push_back(encode(Opcode::ADD64_REG, 6, 8));
      const size_t ChangedTarget = Literal(2, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 2));
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 10, -32));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 5));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Error = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 5));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Helper = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
      std::vector<size_t> Reject;
      if (Mode != 2) {
        Literal(2, (Mode == 0 ? kHeapStart : kStackStart) + 8);
        Reject.push_back(Text.size());
        Text.push_back({});
        Literal(2, Mode == 0 ? kHeapStart + 512 : kHeapStart - 1);
        Reject.push_back(Text.size());
        Text.push_back({});
      }
      Text.push_back(encode(Opcode::EXIT));
      const size_t Failed = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t A = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t B = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      Text[ChooseCall] =
          encode(Opcode::JEQ64_IMM, 9, 0, OtherCall - ChooseCall - 1);
      Text[SkipOtherCall] = encode(Opcode::JA, 0, 0, Join - SkipOtherCall - 1);
      Text[Null] = encode(Opcode::JEQ64_IMM, 0, 0, Error - Null - 1);
      Text[ZeroOffset] = encode(Opcode::JEQ64_IMM, 9, 0, Add - ZeroOffset - 1);
      Text[EightOffset] =
          encode(Opcode::JEQ64_IMM, 9, 0, Add - EightOffset - 1, 1);
      for (size_t Slot : Calls)
        Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Slot - 1);
      for (size_t I = 0; I < Reject.size(); ++I)
        Text[Reject[I]] = encode(I == 0 ? Opcode::JLT64_REG : Opcode::JGT64_REG,
                                 0, 2, Failed - Reject[I] - 1);
      for (const auto &[Slot, Target] :
           {std::pair{InitialTarget, A}, std::pair{ChangedTarget, B}}) {
        const auto Words = test::loadImm64(2, kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (Reverse)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_EQ(llvm::any_of(Report->Contexts,
                             [&](const auto &Site) {
                               return Site.CallXSlot == CallX &&
                                      Site.TargetSlot == A;
                             }),
                Mode == 0);
      const uint64_t Saved =
          initialFramePointer(Program->Low.TheVersion, Program->Config) - 32;
      for (unsigned Path = 0; Path != 2; ++Path)
        for (unsigned Offset = 0; Offset != 3; ++Offset)
          for (bool Alias : {false, true}) {
            const uint64_t Pointer = Mode != 0 && Alias ? Saved - Offset * 8
                                     : Mode == 1
                                         ? Saved - 128
                                         : kHeapStart + 64 + (Alias ? 16 : 0);
            std::vector<uint8_t> Input(24);
            llvm::support::endian::write64le(Input.data(), Path);
            llvm::support::endian::write64le(Input.data() + 8, Pointer);
            llvm::support::endian::write64le(Input.data() + 16, Offset);
            ExecutionEnvironment Environment;
            Environment.Memory.push_back({kInputStart, Input, false, "input"});
            Environment.Memory.push_back(
                {kHeapStart, std::vector<uint8_t>(1024), true, "objects"});
            auto Raw = executeRaw(*Program, std::move(Environment));
            ASSERT_TRUE(static_cast<bool>(Raw))
                << llvm::toString(Raw.takeError());
            ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
            EXPECT_EQ(Raw->ReturnValue, Mode != 0 && Alias ? 43u : 42u);
            ++RawExecutions;
          }
    }
  EXPECT_EQ(RawExecutions, 72u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, BoundedAllocatorCursorKeepsDisjointHeapFields) {
  size_t RawExecutions = 0;
  for (unsigned Mode = 0; Mode != 3; ++Mode)
    for (bool Warm : {false, true})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Mode << ", warm=" << Warm << ", reverse=" << Reverse);
        std::vector<test::EncodedInstruction> Text;
        const auto Literal = [&](unsigned R, uint64_t Value) {
          const size_t Slot = Text.size();
          const auto Words = test::loadImm64(R, Value);
          Text.insert(Text.end(), Words.begin(), Words.end());
          return Slot;
        };
        Literal(7, kHeapStart);
        Literal(8, kInputStart);
        const size_t InitialTarget = Literal(2, 0);
        Text.push_back(encode(Opcode::ST_DW_REG, 7, 2, 256));
        Text.push_back(encode(Opcode::LD_DW_REG, 9, 8));
        std::vector<size_t> AllocateCalls;
        for (unsigned I = 0; Warm && I < kCallXContextSpecializationsPerEntry;
             ++I) {
          Literal(6, kHeapStart + 2048 + I * 64);
          Text.push_back(encode(Opcode::ST_DW_REG, 7, 6));
          Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, (I + 1) * 8));
          AllocateCalls.push_back(Text.size());
          Text.push_back({});
        }
        std::array<size_t, 4> Branches;
        for (auto &Slot : Branches) {
          Slot = Text.size();
          Text.push_back({});
        }
        std::array<size_t, 5> Choices, Joins;
        for (unsigned N = 0; N != 5; ++N) {
          const unsigned I = (N + 4) % 5;
          Choices[I] = Text.size();
          if (Mode == 2 && I == 4)
            Text.push_back(encode(Opcode::LD_DW_REG, 6, 8, 8));
          else
            Literal(6,
                    kHeapStart + (Mode == 1 && I == 4 ? 264 : 1024 + I * 32));
          Text.push_back(encode(Opcode::ST_DW_REG, 7, 6));
          Joins[I] = Text.size();
          Text.push_back({});
        }
        const size_t Join = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, 24));
        const size_t Allocate = Text.size();
        AllocateCalls.push_back(Allocate);
        Text.push_back({});
        const size_t Null = Text.size();
        Text.push_back({});
        const size_t ChangedTarget = Literal(2, 0);
        Text.push_back(encode(Opcode::ST_DW_REG, 0, 2, 16));
        Text.push_back(encode(Opcode::LD_DW_REG, 5, 7, 256));
        const size_t CallX = Text.size();
        Text.push_back(encode(Opcode::CALL_REG, 5));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Error = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 5));
        Text.push_back(encode(Opcode::EXIT));
        // A descending allocator with an explicit subtraction-underflow guard,
        // eight-byte alignment, and a lower-bound check. Its cursor can alias
        // an existing object: the allocator name supplies no freshness proof.
        const size_t Helper = Text.size();
        Literal(3, kHeapStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 3, 3));
        Literal(4, kHeapStart + 32768);
        Text.push_back(encode(Opcode::JEQ64_IMM, 3, 0, 1));
        Text.push_back(encode(Opcode::MOV64_REG, 4, 3));
        Text.push_back(encode(Opcode::MOV64_REG, 3, 4));
        Text.push_back(encode(Opcode::SUB64_REG, 3, 1));
        Text.push_back(encode(Opcode::MOV64_IMM, 0));
        Text.push_back(encode(Opcode::MOV64_IMM, 5, 0, 0, 1));
        Text.push_back(encode(Opcode::JGT64_REG, 3, 4, 1));
        Text.push_back(encode(Opcode::MOV64_IMM, 5));
        Text.push_back(encode(Opcode::MOV64_IMM, 1));
        Text.push_back(encode(Opcode::JNE64_IMM, 5, 0, 1));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 3));
        Text.push_back(encode(Opcode::AND64_IMM, 1, 0, 0, -8));
        Literal(2, kHeapStart + 8);
        const size_t Reject = Text.size();
        Text.push_back({});
        Literal(2, kHeapStart);
        Text.push_back(encode(Opcode::ST_DW_REG, 2, 1));
        Text.push_back(encode(Opcode::MOV64_REG, 0, 1));
        const size_t Exit = Text.size();
        Text.push_back(encode(Opcode::EXIT));
        const size_t A = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
        Text.push_back(encode(Opcode::EXIT));
        const size_t B = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
        Text.push_back(encode(Opcode::EXIT));
        for (unsigned I = 0; I != Branches.size(); ++I)
          Text[Branches[I]] =
              encode(Opcode::JEQ64_IMM, 9, 0, Choices[I] - Branches[I] - 1, I);
        for (size_t Slot : Joins)
          Text[Slot] = encode(Opcode::JA, 0, 0, Join - Slot - 1);
        for (size_t Slot : AllocateCalls)
          Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Slot - 1);
        Text[Null] = encode(Opcode::JEQ64_IMM, 0, 0, Error - Null - 1);
        Text[Reject] = encode(Opcode::JGT64_REG, 2, 1, Exit - Reject - 1);
        for (const auto &[Slot, Target] :
             {std::pair{InitialTarget, A}, std::pair{ChangedTarget, B}}) {
          const auto Words = test::loadImm64(2, kBytecodeStart + Target * 8);
          std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
        }
        auto Program = analyze(test::makeImage(Text, {}));
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        if (Reverse)
          std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        checkSummaryReferences(*Report);
        EXPECT_EQ(llvm::any_of(Report->Contexts,
                               [&](const auto &Site) {
                                 return Site.CallXSlot == CallX &&
                                        Site.TargetSlot == A;
                               }),
                  Mode == 0);
        for (unsigned Selector = 0; Selector != 5; ++Selector)
          for (bool Alias : {false, true}) {
            std::vector<uint8_t> Input(16);
            llvm::support::endian::write64le(Input.data(), Selector);
            llvm::support::endian::write64le(Input.data() + 8,
                                             kHeapStart + (Alias ? 264 : 1200));
            ExecutionEnvironment Environment;
            Environment.Memory.push_back({kInputStart, Input, false, "input"});
            Environment.Memory.push_back(
                {kHeapStart, std::vector<uint8_t>(32768), true, "objects"});
            auto Raw = executeRaw(*Program, std::move(Environment));
            ASSERT_TRUE(static_cast<bool>(Raw))
                << llvm::toString(Raw.takeError());
            ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
            const bool Overwritten =
                Selector == 4 && (Mode == 1 || (Mode == 2 && Alias));
            EXPECT_EQ(Raw->ReturnValue, Overwritten ? 43u : 42u);
            ++RawExecutions;
          }
      }
  EXPECT_EQ(RawExecutions, 120u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, ReplayedRangeReturnKeepsObjectIdentity) {
  size_t RawExecutions = 0;
  for (bool Warm : {false, true})
    for (bool Alias : {false, true})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "warm=" << Warm << ", alias="
                                          << Alias << ", reverse=" << Reverse);
        std::vector<test::EncodedInstruction> Text;
        const auto Literal = [&](unsigned R, uint64_t Value) {
          const size_t Slot = Text.size();
          const auto Words = test::loadImm64(R, Value);
          Text.insert(Text.end(), Words.begin(), Words.end());
          return Slot;
        };
        Literal(7, kHeapStart);
        Literal(8, kInputStart);
        std::vector<size_t> Calls;
        for (unsigned I = 0; Warm && I < kCallXContextSpecializationsPerEntry;
             ++I) {
          Literal(6, kHeapStart + 2048 + I * 64);
          Text.push_back(encode(Opcode::ST_DW_REG, 7, 6));
          Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, (I + 1) * 8));
          Calls.push_back(Text.size());
          Text.push_back({});
        }
        // The zero-extended word plus this cursor has an unsigned range
        // whose upper endpoint is beyond the heap region. Naming the returned
        // value must not assume all possible addresses are mapped or disjoint.
        Literal(6, kHeapStart + 1024);
        Text.push_back(encode(Opcode::ST_DW_REG, 7, 6));
        Text.push_back(encode(Opcode::LD_W_REG, 1, 8));
        const size_t Call = Text.size();
        Calls.push_back(Call);
        Text.push_back({});
        const size_t Target = Literal(2, 0);
        Text.push_back(encode(Opcode::ST_DW_REG, 0, 2, 16));
        std::optional<size_t> OtherTarget;
        if (Alias) {
          Text.push_back(encode(Opcode::LD_DW_REG, 3, 8, 8));
          OtherTarget = Literal(2, 0);
          Text.push_back(encode(Opcode::ST_DW_REG, 3, 2));
        }
        Text.push_back(encode(Opcode::LD_DW_REG, 5, 0, 16));
        const size_t CallX = Text.size();
        Text.push_back(encode(Opcode::CALL_REG, 5));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Helper = Text.size();
        Literal(2, kHeapStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 3, 2));
        Text.push_back(encode(Opcode::MOV64_REG, 0, 1));
        Text.push_back(encode(Opcode::ADD64_REG, 0, 3));
        Text.push_back(encode(Opcode::AND64_IMM, 0, 0, 0, -8));
        Text.push_back(encode(Opcode::ST_DW_REG, 2, 0));
        Text.push_back(encode(Opcode::EXIT));
        const size_t A = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
        Text.push_back(encode(Opcode::EXIT));
        const size_t B = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
        Text.push_back(encode(Opcode::EXIT));
        for (size_t Slot : Calls)
          Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Slot - 1);
        const auto Words = test::loadImm64(2, kBytecodeStart + A * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Target);
        if (OtherTarget) {
          const auto Other = test::loadImm64(2, kBytecodeStart + B * 8);
          std::copy(Other.begin(), Other.end(), Text.begin() + *OtherTarget);
        }
        auto Program = analyze(test::makeImage(Text, {}));
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        if (Reverse)
          std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        checkSummaryReferences(*Report);
        if (Warm)
          EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
            return Edge.CallerSlot == Call &&
                   Edge.InstantiatedMemoryPathInstructions;
          }));
        EXPECT_EQ(llvm::any_of(Report->Contexts,
                               [&](const auto &Site) {
                                 return Site.CallXSlot == CallX &&
                                        Site.TargetSlot == A;
                               }),
                  !Alias);
        for (uint32_t Offset : {0u, 7u, 16u, 1024u}) {
          std::vector<uint8_t> Input(16);
          llvm::support::endian::write32le(Input.data(), Offset);
          const uint64_t Pointer = (kHeapStart + 1024 + Offset) & ~uint64_t{7};
          llvm::support::endian::write64le(Input.data() + 8, Pointer + 16);
          ExecutionEnvironment Environment;
          Environment.Memory.push_back({kInputStart, Input, false, "input"});
          Environment.Memory.push_back(
              {kHeapStart, std::vector<uint8_t>(32768), true, "objects"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, Alias ? 43u : 42u);
          ++RawExecutions;
        }
      }
  EXPECT_EQ(RawExecutions, 32u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFStoredRegisterMemory, AlignmentBoundsPreserveRawResultsAndRejectHoles) {
  auto Image = analyze(test::makeImage({encode(Opcode::EXIT)}, {}));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  struct Case {
    uint64_t Lower;
    uint64_t Upper;
    int32_t Mask;
    bool Representable;
  };
  size_t RawExecutions = 0;
  for (const auto &C : {Case{kHeapStart + 15, kHeapStart + 32, -16, true},
                        Case{kHeapStart + 15, kHeapStart + 32, -17, false},
                        Case{UINT64_MAX - 31, UINT64_MAX, -16, true},
                        Case{kHeapStart + 31, kHeapStart + 15, -16, false},
                        Case{kHeapStart + 15, kHeapStart + 32, 0, true},
                        Case{kHeapStart + 15, kHeapStart + 32, -1, true}}) {
    SCOPED_TRACE(::testing::Message()
                 << C.Lower << ", " << C.Upper << ", mask=" << C.Mask);
    const auto &Info = *getOpcodeInfo(Opcode::AND64_IMM);
    MedInstruction Instruction;
    Instruction.Op = Info.Op;
    Instruction.Form = Info.Form;
    Instruction.Width = Info.Width;
    Instruction.Immediate = static_cast<uint64_t>(C.Mask);
    Instruction.Semantics = semanticTraits(Info, Version::V0);
    RegisterState State{};
    State[0] = unsignedRangeValue({C.Lower, C.Upper});
    applyRegisterTransfer(Instruction, State, Image->ExecutableImage);
    const auto Bounds = unsignedValueRange(State[0]);
    EXPECT_EQ(Bounds.has_value(), C.Representable);
    std::vector<uint64_t> Inputs;
    if (C.Lower <= C.Upper) {
      for (uint64_t Value = C.Lower;; ++Value) {
        Inputs.push_back(Value);
        if (Value == C.Upper)
          break;
      }
    } else {
      Inputs = {C.Lower, UINT64_MAX, 0, C.Upper};
    }
    bool BelowEndpointHull = false;
    for (uint64_t Input : Inputs) {
      const auto Words = test::loadImm64(0, Input);
      std::vector<test::EncodedInstruction> Text(Words.begin(), Words.end());
      Text.push_back(encode(Opcode::AND64_IMM, 0, 0, 0, C.Mask));
      Text.push_back(encode(Opcode::EXIT));
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      auto Raw = executeRaw(*Program);
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      if (Bounds) {
        EXPECT_LE(Bounds->Lower, Raw->ReturnValue);
        EXPECT_GE(Bounds->Upper, Raw->ReturnValue);
      }
      const uint64_t Mask = static_cast<uint64_t>(C.Mask);
      BelowEndpointHull |=
          Raw->ReturnValue < std::min(C.Lower & Mask, C.Upper & Mask);
      ++RawExecutions;
    }
    if (C.Mask == -17)
      EXPECT_TRUE(BelowEndpointHull);
  }
  EXPECT_EQ(RawExecutions, 108u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFStoredRegisterMemory, CommutedBoundedAdditionKeepsRawContainment) {
  auto Image = analyze(test::makeImage({encode(Opcode::EXIT)}, {}));
  ASSERT_TRUE(static_cast<bool>(Image)) << llvm::toString(Image.takeError());
  struct Case {
    uint64_t Lower, Upper, Base;
    bool Representable;
  };
  size_t RawExecutions = 0;
  for (const auto &C :
       {Case{64, 80, kHeapStart, true}, Case{64, 80, kInputStart, true},
        Case{0, 16, 8, true}, Case{64, 80, UINT64_MAX - 31, true},
        Case{0, 16, UINT64_MAX - 7, false},
        Case{UINT64_MAX - 16, UINT64_MAX, 8, false}})
    for (bool BaseFirst : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << C.Lower << ", " << C.Upper << ", base=" << C.Base
                   << ", base_first=" << BaseFirst);
      const auto &Info = *getOpcodeInfo(Opcode::ADD64_REG);
      MedInstruction Instruction;
      Instruction.Op = Info.Op;
      Instruction.Form = Info.Form;
      Instruction.Width = Info.Width;
      Instruction.Dst = 0;
      Instruction.Src = 1;
      Instruction.Semantics = semanticTraits(Info, Version::V0);
      RegisterState State{};
      State[BaseFirst ? 0 : 1] = {RegisterValue::Kind::Constant, C.Base, 0};
      State[BaseFirst ? 1 : 0] = unsignedRangeValue({C.Lower, C.Upper});
      applyRegisterTransfer(Instruction, State, Image->ExecutableImage);
      const auto Bounds = unsignedValueRange(State[0]);
      EXPECT_EQ(Bounds.has_value(), C.Representable);
      for (uint64_t Value :
           {C.Lower, C.Lower + (C.Upper - C.Lower) / 2, C.Upper}) {
        auto Text = test::loadImm64(0, BaseFirst ? C.Base : Value);
        const auto Right = test::loadImm64(1, BaseFirst ? Value : C.Base);
        Text.insert(Text.end(), Right.begin(), Right.end());
        Text.push_back(encode(Opcode::ADD64_REG, 0, 1));
        Text.push_back(encode(Opcode::EXIT));
        auto Program = analyze(test::makeImage(Text, {}));
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        auto Raw = executeRaw(*Program);
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        if (Bounds) {
          EXPECT_LE(Bounds->Lower, Raw->ReturnValue);
          EXPECT_GE(Bounds->Upper, Raw->ReturnValue);
        }
        ++RawExecutions;
      }
    }
  EXPECT_EQ(RawExecutions, 36u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, OffsetSumProductsKeepDisjointFields) {
  size_t RawExecutions = 0;
  for (unsigned Mode = 0; Mode != 3; ++Mode)
    for (bool BaseFirst : {false, true})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Mode << ", base_first=" << BaseFirst
                     << ", reverse=" << Reverse);
        std::vector<test::EncodedInstruction> Text;
        const auto Literal = [&](unsigned R, uint64_t Value) {
          const size_t Slot = Text.size();
          const auto Words = test::loadImm64(R, Value);
          Text.insert(Text.end(), Words.begin(), Words.end());
          return Slot;
        };
        Literal(7, kHeapStart + 32);
        const size_t SavedTarget = Literal(2, 0);
        Text.push_back(encode(Opcode::ST_DW_REG, 7, 2));
        Literal(1, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 5, 1));
        std::array<size_t, 3> Branches;
        for (auto &Slot : Branches) {
          Slot = Text.size();
          Text.push_back({});
        }
        std::array<size_t, 4> Choices, Joins;
        for (unsigned N = 0; N != 4; ++N) {
          const unsigned I = (N + 3) % 4;
          Choices[I] = Text.size();
          if (Mode == 2 && I == 3)
            Text.push_back(encode(Opcode::LD_DW_REG, 8, 1, 16));
          else
            Literal(8, Mode == 1 && I == 0 ? 16 : 64 + I * 64);
          Joins[I] = Text.size();
          Text.push_back({});
        }
        const size_t Join = Text.size();
        Text.push_back(encode(Opcode::LD_DW_REG, 9, 1, 8));
        Text.push_back(encode(Opcode::JEQ64_IMM, 9, 0, 2));
        Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, 16));
        Text.push_back(encode(Opcode::JA, 0, 0, 1));
        Text.push_back(encode(Opcode::MOV64_IMM, 9));
        Text.push_back(encode(Opcode::ADD64_REG, 8, 9));
        Literal(6, kHeapStart);
        if (BaseFirst)
          Text.push_back(encode(Opcode::ADD64_REG, 6, 8));
        else {
          Text.push_back(encode(Opcode::ADD64_REG, 8, 6));
          Text.push_back(encode(Opcode::MOV64_REG, 6, 8));
        }
        const size_t ChangedTarget = Literal(2, 0);
        Text.push_back(encode(Opcode::ST_DW_REG, 6, 2));
        Text.push_back(encode(Opcode::LD_DW_REG, 5, 7));
        const size_t CallX = Text.size();
        Text.push_back(encode(Opcode::CALL_REG, 5));
        Text.push_back(encode(Opcode::EXIT));
        const size_t A = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
        Text.push_back(encode(Opcode::EXIT));
        const size_t B = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
        Text.push_back(encode(Opcode::EXIT));
        for (unsigned I = 0; I != Branches.size(); ++I)
          Text[Branches[I]] =
              encode(Opcode::JEQ64_IMM, 5, 0, Choices[I] - Branches[I] - 1, I);
        for (size_t Slot : Joins)
          Text[Slot] = encode(Opcode::JA, 0, 0, Join - Slot - 1);
        for (const auto &[Slot, Target] :
             {std::pair{SavedTarget, A}, std::pair{ChangedTarget, B}}) {
          const auto Words = test::loadImm64(2, kBytecodeStart + Target * 8);
          std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
        }
        auto Program = analyze(test::makeImage(Text, {}));
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        if (Reverse)
          std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        checkSummaryReferences(*Report);
        EXPECT_EQ(llvm::any_of(Report->Contexts,
                               [&](const auto &Site) {
                                 return Site.CallXSlot == CallX &&
                                        Site.TargetSlot == A;
                               }),
                  Mode == 0);
        for (unsigned Selector = 0; Selector != 4; ++Selector)
          for (unsigned Length = 0; Length != 2; ++Length) {
            std::vector<uint8_t> Input(24);
            llvm::support::endian::write64le(Input.data(), Selector);
            llvm::support::endian::write64le(Input.data() + 8, Length);
            llvm::support::endian::write64le(Input.data() + 16, 32);
            ExecutionEnvironment Environment;
            Environment.Memory.push_back({kInputStart, Input, false, "input"});
            Environment.Memory.push_back(
                {kHeapStart, std::vector<uint8_t>(1024), true, "heap"});
            auto Raw = executeRaw(*Program, std::move(Environment));
            ASSERT_TRUE(static_cast<bool>(Raw))
                << llvm::toString(Raw.takeError());
            ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
            const bool Alias = (Mode == 1 && Selector == 0 && Length == 1) ||
                               (Mode == 2 && Selector == 3 && Length == 0);
            EXPECT_EQ(Raw->ReturnValue, Alias ? 43u : 42u);
            ++RawExecutions;
          }
      }
  EXPECT_EQ(RawExecutions, 96u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, InteriorTagExclusionsKeepTheirOwnPayloads) {
  size_t RawExecutions = 0;
  for (unsigned Mode = 0; Mode != 4; ++Mode)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Mode << ", reverse=" << Reverse);
      const bool MutateTag = Mode == 3;
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
      const size_t Call = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_B_REG, 7, 10, -16));
      const size_t Select = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_DW_REG, 8, 10, -8));
      const size_t OtherCallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 8));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Equal = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 8, 10, -8));
      const size_t EqualCallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 8));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Producer = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
      Literal(3, kInputStart);
      Text.push_back(encode(Opcode::LD_B_REG, 9, 3));
      if (Mode == 2)
        Text.push_back(encode(Opcode::MOV64_IMM, 4, 0, 0, 2));
      const size_t Choose = Text.size();
      Text.push_back({});
      const size_t FirstTarget = Literal(2, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
      if (MutateTag)
        Text.push_back(encode(Opcode::MOV64_IMM, 9, 0, 0, 2));
      const size_t Jump = Text.size();
      Text.push_back({});
      const size_t Second = Text.size();
      const size_t SecondTarget = Literal(2, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
      const size_t Tail = Text.size();
      Text.push_back(encode(Opcode::ST_B_REG, 6, 9));
      Text.push_back(encode(Opcode::EXIT));
      const size_t A = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t B = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Producer - Call - 1);
      Text[Select] = encode(Opcode::JEQ64_IMM, 7, 0, Equal - Select - 1, 2);
      Text[Choose] =
          encode(Mode == 1   ? Opcode::JNE64_IMM
                 : Mode == 2 ? Opcode::JEQ64_REG
                             : Opcode::JEQ64_IMM,
                 Mode == 2 ? 4 : 9, Mode == 2 ? 9 : 0, Second - Choose - 1, 2);
      Text[Jump] = encode(Opcode::JA, 0, 0, Tail - Jump - 1);
      for (const auto &[Slot, Target] :
           {std::pair{FirstTarget, Mode == 1 ? B : A},
            std::pair{SecondTarget, Mode == 1 ? A : B}}) {
        const auto Words = test::loadImm64(2, kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (Reverse)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      for (const auto &[Slot, Target] :
           {std::pair{EqualCallX, B}, std::pair{OtherCallX, A}}) {
        bool Proven = false;
        for (const auto &Site : Report->Contexts)
          if (Site.CallXSlot == Slot && Site.TargetSlot) {
            EXPECT_EQ(*Site.TargetSlot, Target);
            Proven = true;
          }
        if (!MutateTag)
          EXPECT_TRUE(Proven) << Slot;
        else if (Slot == EqualCallX)
          EXPECT_FALSE(Proven);
      }
      std::set<size_t> EqualTargets, OtherTargets;
      for (unsigned Tag = 0; Tag != 256; ++Tag) {
        ExecutionEnvironment Environment;
        Environment.Memory.push_back(
            {kInputStart, {static_cast<uint8_t>(Tag)}, false, "tag"});
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, Tag == 2 ? 43u : 42u);
        for (size_t I = 0; I + 1 < Raw->Trace.size(); ++I) {
          if (Raw->Trace[I].Slot == EqualCallX)
            EqualTargets.insert(Raw->Trace[I + 1].Slot);
          if (Raw->Trace[I].Slot == OtherCallX)
            OtherTargets.insert(Raw->Trace[I + 1].Slot);
        }
        ++RawExecutions;
      }
      EXPECT_EQ(EqualTargets,
                MutateTag ? (std::set<size_t>{A, B}) : (std::set<size_t>{B}));
      EXPECT_EQ(OtherTargets,
                MutateTag ? (std::set<size_t>{}) : (std::set<size_t>{A}));
    }
  EXPECT_EQ(RawExecutions, 2048u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, InteriorExclusionsKeepBoundsAtTheAlternativeLimit) {
  size_t RawExecutions = 0;
  for (bool Unknown : {false, true})
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "unknown=" << Unknown << ", reverse=" << Reverse);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      const size_t SavedTarget = Literal(2, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -8));
      Literal(1, kInputStart);
      Text.push_back(
          encode(Unknown ? Opcode::LD_DW_REG : Opcode::LD_B_REG, 8, 1));
      std::array<size_t, 4> Exclusions;
      for (auto &Slot : Exclusions) {
        Slot = Text.size();
        Text.push_back({});
      }
      Text.push_back(encode(Opcode::AND64_IMM, 8, 0, 0, -8));
      Literal(1, kHeapStart);
      Text.push_back(encode(Opcode::ADD64_REG, 8, 1));
      const size_t ChangedTarget = Literal(2, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 8, 2));
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 10, -8));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 5));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Excluded = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t A = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t B = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      for (unsigned I = 0; I != Exclusions.size(); ++I)
        Text[Exclusions[I]] = encode(Opcode::JEQ64_IMM, 8, 0,
                                     Excluded - Exclusions[I] - 1, 2 + 2 * I);
      for (const auto &[Slot, Target] :
           {std::pair{SavedTarget, A}, std::pair{ChangedTarget, B}}) {
        const auto Words = test::loadImm64(2, kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (Reverse)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      EXPECT_EQ(llvm::any_of(Report->Contexts,
                             [&](const auto &Site) {
                               return Site.CallXSlot == CallX &&
                                      Site.TargetSlot == A;
                             }),
                !Unknown);
      for (unsigned I = 0; I != 256 + unsigned(Unknown); ++I) {
        const bool Alias = I == 256;
        const uint64_t Value =
            Alias
                ? kStackStart + Program->Config.StackFrameSize - 8 - kHeapStart
                : I;
        std::vector<uint8_t> Input(8);
        llvm::support::endian::write64le(Input.data(), Value);
        ExecutionEnvironment Environment;
        Environment.Memory.push_back({kInputStart, Input, false, "offset"});
        Environment.Memory.push_back(
            {kHeapStart, std::vector<uint8_t>(256), true, "heap"});
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, Alias ? 43u
                                    : (I == 2 || I == 4 || I == 6 || I == 8)
                                        ? 0u
                                        : 42u);
        ++RawExecutions;
      }
    }
  EXPECT_EQ(RawExecutions, 1026u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, TaggedPayloadExpressionsCrossGenericWrappers) {
  size_t RawExecutions = 0;
  for (unsigned Mode = 0; Mode != 5; ++Mode)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Mode << ", reverse=" << Reverse);
      const bool Preload = Mode >= 3;
      const bool MutateSource = Mode == 1 || Mode == 4;
      std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      std::vector<size_t> Entries, Calls, Targets, TargetLiterals,
          OtherLiterals;
      for (unsigned I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
        Entries.push_back(Text.size());
        Literal(1, kHeapStart + 64 * (I + 1));
        TargetLiterals.push_back(Literal(2, 0));
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 2));
        OtherLiterals.push_back(Literal(2, 0));
        Text.push_back(encode(Opcode::ST_DW_REG, 1, 2, 8));
        Calls.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Wrapper = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
      if (Mode == 2) {
        Literal(4, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 5, 4, 8));
        OtherLiterals.push_back(Literal(2, 0));
        Text.push_back(encode(Opcode::ST_DW_REG, 5, 2));
      }
      Text.push_back(encode(Opcode::MOV64_REG, 2, 6));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -32));
      const size_t ChildCall = Text.size();
      Text.push_back({});
      if (Preload) {
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -24));
        Text.push_back(encode(Opcode::MOV64_REG, 8, 2));
      }
      Text.push_back(encode(Opcode::LD_B_REG, 7, 10, -32));
      const size_t Select = Text.size();
      Text.push_back({});
      if (!Preload)
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -24));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, Preload ? 8 : 2));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Done = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 5));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Producer = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
      Text.push_back(encode(Opcode::MOV64_REG, 7, 2));
      Literal(4, kInputStart);
      Text.push_back(encode(Opcode::LD_B_REG, 9, 4));
      const size_t Choose = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 7));
      if (MutateSource) {
        const size_t Mutation = Literal(3, 0);
        Text.push_back(encode(Opcode::ST_DW_REG, 7, 3));
        // This callback literal uses r3, unlike the root's r2 literals.
        OtherLiterals.push_back(Mutation);
      }
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
      Text.push_back(encode(Opcode::MOV64_IMM, 1));
      const size_t Jump = Text.size();
      Text.push_back({});
      const size_t Other = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 7, 8));
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 2, 8));
      Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, 1));
      const size_t Tail = Text.size();
      Text.push_back(encode(Opcode::ST_B_REG, 6, 1));
      Text.push_back(encode(Opcode::EXIT));
      for (unsigned I = 0; I != Entries.size(); ++I) {
        Targets.push_back(Text.size());
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 40 + I));
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Changed = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 99));
      Text.push_back(encode(Opcode::EXIT));
      for (unsigned I = 0; I != Calls.size(); ++I) {
        Text[Calls[I]] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
        const auto Words = test::loadImm64(2, kBytecodeStart + Targets[I] * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + TargetLiterals[I]);
      }
      for (size_t Slot : OtherLiterals) {
        const auto Words =
            test::loadImm64(MutateSource && Slot > Producer ? 3 : 2,
                            kBytecodeStart + Changed * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      }
      Text[ChildCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Producer - ChildCall - 1);
      Text[Select] = encode(Opcode::JNE64_IMM, 7, 0, Done - Select - 1);
      Text[Choose] = encode(Opcode::JNE64_IMM, 9, 0, Other - Choose - 1);
      Text[Jump] = encode(Opcode::JA, 0, 0, Tail - Jump - 1);
      auto Image = test::makeImage(Text, {});
      for (size_t Entry : Entries) {
        Symbol Function;
        Function.Name = "tag_expression_caller_" + std::to_string(Entry);
        Function.Addr = kBytecodeStart + Entry * kInstructionSize;
        Function.IsFunc = true;
        Image.Symbols.push_back(std::move(Function));
      }
      auto Program = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (Reverse)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
        return Edge.CallerSlot == Calls.back() &&
               Edge.InputGeneralization ==
                   CallXInputGeneralization::SpecializationLimit;
      }));
      bool Proven = false;
      for (const auto &Site : Report->Contexts)
        if (!Site.EntryPrefixOnly && Site.CallerSlot == Calls.back() &&
            Site.CallXSlot == CallX && Site.TargetSlot) {
          EXPECT_NE(Mode, 2u);
          EXPECT_EQ(*Site.TargetSlot, Targets.back());
          Proven = true;
        }
      EXPECT_EQ(Proven, Mode != 2);
      for (unsigned I = 0; I != Entries.size(); ++I) {
        Image.Entry = kBytecodeStart + Entries[I] * kInstructionSize;
        auto Selected = analyze(Image);
        ASSERT_TRUE(static_cast<bool>(Selected))
            << llvm::toString(Selected.takeError());
        for (unsigned Trial = 0; Trial != (Mode == 2 ? 4u : 2u); ++Trial) {
          std::vector<uint8_t> Input(16);
          Input[0] = Trial & 1;
          llvm::support::endian::write64le(
              Input.data() + 8,
              Trial >> 1 ? kHeapStart + 64 * (I + 1) : kHeapStart + 512);
          ExecutionEnvironment Environment;
          Environment.Memory.push_back({kInputStart, Input, false, "input"});
          Environment.Memory.push_back(
              {kHeapStart, std::vector<uint8_t>(1024), true, "heap"});
          auto Raw = executeRaw(*Selected, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, Trial & 1    ? 5u
                                      : Trial >> 1 ? 99u
                                                   : 40 + I);
          ++RawExecutions;
        }
      }
    }
  EXPECT_EQ(RawExecutions, 120u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, RestoredEntryWordsPreserveCallerMemory) {
  size_t RawExecutions = 0;
  for (unsigned Mode = 0; Mode != 6; ++Mode)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Mode << ", reverse=" << Reverse);
      std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      std::vector<size_t> Entries, Calls, CallXs, Targets, TargetLiterals;
      std::vector<std::pair<size_t, unsigned>> OtherLiterals;
      for (unsigned I = 0; I <= kCallXContextSpecializationsPerEntry; ++I) {
        Entries.push_back(Text.size());
        Literal(6, kHeapStart + 64 * (I + 1));
        TargetLiterals.push_back(Literal(2, 0));
        Text.push_back(encode(Opcode::ST_DW_REG, 6, 2));
        Literal(1, kInputStart + 16 * I);
        Text.push_back(encode(Opcode::LD_DW_REG, 4, 1, 8));
        Calls.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::LD_DW_REG, 8, 6));
        CallXs.push_back(Text.size());
        Text.push_back(encode(Opcode::CALL_REG, 8));
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Borrow = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 1, 1));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 1));
      Literal(3, INT64_MAX);
      const size_t Branch = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::JA, 0, 0, -1));
      const size_t Normal = Text.size();
      OtherLiterals.emplace_back(Literal(3, 0), 3);
      Text.push_back(encode(Opcode::ST_DW_REG, 1, 3));
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
      if (Mode == 1)
        OtherLiterals.emplace_back(Literal(2, 0), 2);
      if (Mode == 2)
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, 8));
      if (Mode == 4)
        Text.push_back(encode(Opcode::ST_DW_REG, 4, 3));
      const size_t InterveningCall = Text.size();
      if (Mode == 5)
        Text.push_back({});
      Text.push_back(
          encode(Mode == 3 ? Opcode::ST_W_REG : Opcode::ST_DW_REG, 1, 2));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Mutator = Text.size();
      OtherLiterals.emplace_back(Literal(3, 0), 3);
      Text.push_back(encode(Opcode::ST_DW_REG, 4, 3));
      Text.push_back(encode(Opcode::EXIT));
      for (unsigned I = 0; I != Entries.size(); ++I) {
        Targets.push_back(Text.size());
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 40 + I));
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Changed = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 99));
      Text.push_back(encode(Opcode::EXIT));
      for (unsigned I = 0; I != Calls.size(); ++I) {
        Text[Calls[I]] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Borrow - Calls[I] - 1);
        const auto Words = test::loadImm64(2, kBytecodeStart + Targets[I] * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + TargetLiterals[I]);
      }
      for (const auto &[Slot, Register] : OtherLiterals) {
        const auto Words =
            test::loadImm64(Register, kBytecodeStart + Changed * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      }
      Text[Branch] = encode(Opcode::JGT64_REG, 3, 2, Normal - Branch - 1);
      if (Mode == 5)
        Text[InterveningCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Mutator - InterveningCall - 1);
      auto Image = test::makeImage(Text, {});
      for (size_t Entry : Entries) {
        Symbol Function;
        Function.Name = "restored_word_caller_" + std::to_string(Entry);
        Function.Addr = kBytecodeStart + Entry * kInstructionSize;
        Function.IsFunc = true;
        Image.Symbols.push_back(std::move(Function));
      }
      auto Program = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (Reverse)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      bool Proven = false;
      for (const auto &Site : Report->Contexts)
        if (!Site.EntryPrefixOnly && Site.CallXSlot == CallXs.back() &&
            Site.TargetSlot) {
          EXPECT_EQ(Mode, 0u);
          EXPECT_EQ(*Site.TargetSlot, Targets.back());
          Proven = true;
        }
      EXPECT_EQ(Proven, Mode == 0);
      for (unsigned I = 0; I != Entries.size(); ++I) {
        Image.Entry = kBytecodeStart + Entries[I] * kInstructionSize;
        auto Selected = analyze(Image);
        ASSERT_TRUE(static_cast<bool>(Selected))
            << llvm::toString(Selected.takeError());
        for (unsigned Trial = 0; Trial != (Mode >= 4 ? 4u : 2u); ++Trial) {
          const bool BorrowAliases = Trial & 1;
          const bool ExtraAliases = Trial & 2;
          std::vector<uint8_t> Input(16 * Entries.size());
          const uint64_t Cell = kHeapStart + 64 * (I + 1);
          llvm::support::endian::write64le(
              Input.data() + 16 * I, BorrowAliases ? Cell : kHeapStart + 512);
          llvm::support::endian::write64le(Input.data() + 16 * I + 8,
                                           ExtraAliases ? Cell
                                                        : kHeapStart + 544);
          ExecutionEnvironment Environment;
          Environment.Memory.push_back({kInputStart, Input, false, "input"});
          Environment.Memory.push_back(
              {kHeapStart, std::vector<uint8_t>(1024), true, "heap"});
          auto Raw = executeRaw(*Selected, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          const bool ChangedTarget =
              ((Mode == 1 || Mode == 2) && BorrowAliases) ||
              (Mode >= 4 && ExtraAliases && !BorrowAliases);
          EXPECT_EQ(Raw->ReturnValue, ChangedTarget ? 99u : 40 + I);
          ++RawExecutions;
        }
      }
    }
  EXPECT_EQ(RawExecutions, 160u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, IncomingCapturedFieldsSurviveCalleeBackedges) {
  unsigned RawExecutions = 0;
  for (bool Clobber : {false, true})
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Clobber);
      SCOPED_TRACE(Reverse);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned Register, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(Register, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      Literal(1, kInputStart);
      const size_t AcquireCall = Text.size();
      Text.push_back({});
      const size_t Null = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
      const size_t InitialTarget = Literal(3, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 3, 8));
      Literal(2, kInputStart + 8);
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 2));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 6));
      const size_t LoopCall = Text.size();
      Text.push_back({});
      const size_t Exit = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t Loop = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
      Text.push_back(encode(Opcode::MOV64_REG, 7, 2));
      const size_t Header = Text.size();
      Text.push_back({});
      std::optional<size_t> ReplacementTarget;
      if (Clobber) {
        Literal(3, kInputStart + 16);
        Text.push_back(encode(Opcode::LD_DW_REG, 3, 3));
        ReplacementTarget = Literal(4, 0);
        // This may alias the incoming object's field. Keeping its identity
        // must not retain a field invalidated by a possibly overlapping
        // write.
        Text.push_back(encode(Opcode::ST_DW_REG, 3, 4, 8));
      }
      Text.push_back(encode(Opcode::ADD64_IMM, 7, 0, 0, -1));
      const size_t Back = Text.size();
      Text.push_back(encode(Opcode::JA, 0, 0, Header - Back - 1));
      const size_t Read = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 5, 6, 8));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 5));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Acquire = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
      Literal(2, kHeapStart + 8);
      Text.push_back(encode(Opcode::JGE64_REG, 0, 2, 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t FirstTarget = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t SecondTarget = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      Text[AcquireCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Acquire - AcquireCall - 1);
      Text[LoopCall] = encode(Opcode::CALL_IMM, 0, 1, 0, Loop - LoopCall - 1);
      Text[Null] = encode(Opcode::JEQ64_IMM, 0, 0, Exit - Null - 1);
      Text[Header] = encode(Opcode::JEQ64_IMM, 7, 0, Read - Header - 1);
      for (const auto &[Slot, Target] :
           {std::pair{InitialTarget, FirstTarget},
            std::pair{ReplacementTarget.value_or(InitialTarget),
                      ReplacementTarget ? SecondTarget : FirstTarget}}) {
        const auto Words = test::loadImm64(Slot == InitialTarget ? 3 : 4,
                                           kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (Reverse)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      bool Proven = false;
      for (const auto &Site : Report->Contexts)
        if (!Site.EntryPrefixOnly && Site.CallXSlot == CallX &&
            Site.TargetSlot) {
          Proven = true;
          EXPECT_FALSE(Clobber);
          EXPECT_EQ(*Site.TargetSlot, FirstTarget);
        }
      EXPECT_EQ(Proven, !Clobber);
      for (uint64_t Count : {1u, 2u, 4u})
        for (bool Alias : {false, true}) {
          ExecutionEnvironment Environment;
          std::vector<uint8_t> Input(24);
          llvm::support::endian::write64le(Input.data(), kHeapStart + 16);
          llvm::support::endian::write64le(Input.data() + 8, Count);
          llvm::support::endian::write64le(Input.data() + 16,
                                           kHeapStart + (Alias ? 16 : 64));
          Environment.Memory.push_back({kInputStart, Input, false, "input"});
          Environment.Memory.push_back(
              {kHeapStart, std::vector<uint8_t>(128), true, "objects"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, Clobber && Alias ? 43u : 42u);
          ++RawExecutions;
        }
    }
  EXPECT_EQ(RawExecutions, 24u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, GuardRefinesTheValueJustStoredByTheSameBlock) {
  unsigned RawExecutions = 0;
  for (unsigned Mode = 0; Mode != 8; ++Mode)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Mode);
      SCOPED_TRACE(Reverse);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned Register, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(Register, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      Text.push_back(encode(Opcode::MOV64_REG, 6, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 6, 0, 0, -32));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 6));
      Literal(5, uint64_t{1} << 32);
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 5));
      Literal(5, kInputStart);
      Text.push_back(
          encode(Mode == 2 ? Opcode::LD_DW_REG : Opcode::LD_B_REG, 3, 5));
      const size_t Call = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_DW_REG, 1, 6));
      const size_t Select = Text.size();
      Text.push_back({});
      const size_t Bad = Literal(8, 0);
      const size_t Join = Text.size();
      Text.push_back({});
      const size_t Good = Literal(8, 0);
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 8));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Helper = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 4, 0, 0, 2));
      std::optional<size_t> AlternateEntry;
      if (Mode == 7) {
        AlternateEntry = Text.size();
        Text.push_back({});
      }
      Text.push_back(
          encode(Mode == 5 ? Opcode::ST_B_REG : Opcode::ST_DW_REG, 1, 3));
      if (Mode == 3)
        Text.push_back(encode(Opcode::ST_DW_IMM, 1, 0, 0, 3));
      if (Mode == 4)
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, 8));
      if (Mode == 6)
        Text.push_back(encode(Opcode::MOV64_IMM, 3));
      const size_t Guard = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
      const size_t Reject = Text.size();
      Text.push_back(
          encode(Mode == 5 ? Opcode::ST_B_IMM : Opcode::ST_DW_IMM, 1));
      Text.push_back(encode(Opcode::EXIT));
      const size_t FirstTarget = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t SecondTarget = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Call - 1);
      Text[Select] = encode(Opcode::JLT64_IMM, 1, 0, Good - Select - 1, 2);
      Text[Join] = encode(Opcode::JA, 0, 0, CallX - Join - 1);
      Text[Guard] =
          Mode == 1 ? encode(Opcode::JLE64_REG, 4, 3, Reject - Guard - 1)
                    : encode(Mode == 2 ? Opcode::JGE32_IMM : Opcode::JGE64_IMM,
                             3, 0, Reject - Guard - 1, 2);
      if (AlternateEntry)
        Text[*AlternateEntry] =
            encode(Opcode::JEQ64_IMM, 3, 0, Guard - *AlternateEntry - 1);
      for (const auto &[Slot, Target] :
           {std::pair{Good, FirstTarget}, std::pair{Bad, SecondTarget}}) {
        const auto Words = test::loadImm64(8, kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (Reverse)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      bool OnlyFirst = false;
      for (const auto &Site : Report->Contexts)
        if (!Site.EntryPrefixOnly && Site.CallXSlot == CallX &&
            Site.TargetSlot) {
          EXPECT_EQ(*Site.TargetSlot, FirstTarget);
          OnlyFirst = true;
        }
      EXPECT_EQ(OnlyFirst, Mode < 2);
      for (uint64_t Value : {uint64_t{0}, uint64_t{1}, uint64_t{2},
                             uint64_t{255}, (uint64_t{1} << 32) | 1}) {
        ExecutionEnvironment Environment;
        std::vector<uint8_t> Input(8);
        llvm::support::endian::write64le(Input.data(), Value);
        Environment.Memory.push_back({kInputStart, Input, false, "input"});
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        const auto Byte = static_cast<uint8_t>(Value);
        const bool BadValue =
            (Mode == 2 && Value >> 32 && static_cast<uint32_t>(Value) < 2) ||
            (Mode == 3 && Byte < 2) || (Mode == 4 && Byte >= 2) || Mode == 5 ||
            (Mode == 6 && Byte >= 2) || (Mode == 7 && Byte == 0);
        EXPECT_EQ(Raw->ReturnValue, BadValue ? 43u : 42u) << Value;
        ++RawExecutions;
      }
    }
  EXPECT_EQ(RawExecutions, 80u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, ReusesTargetEvidenceWhenTheReturnedScalarIsUnknown) {
  unsigned RawExecutions = 0;
  for (bool KnownReturn : {false, true})
    for (bool ChangedTarget : {false, true})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(KnownReturn);
        SCOPED_TRACE(ChangedTarget);
        SCOPED_TRACE(Reverse);
        std::vector<test::EncodedInstruction> Text;
        const auto Literal = [&](unsigned Register, uint64_t Value) {
          const size_t Slot = Text.size();
          const auto Words = test::loadImm64(Register, Value);
          Text.insert(Text.end(), Words.begin(), Words.end());
          return Slot;
        };
        std::vector<size_t> Calls, Targets;
        for (unsigned I = 0; I != kCallXContextSpecializationsPerEntry + 2;
             ++I) {
          Targets.push_back(Literal(2, 0));
          Text.push_back(encode(Opcode::ST_DW_REG, 10, 2, -16));
          Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -32, I));
          Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
          Text.push_back(encode(Opcode::MOV64_IMM, 2));
          Calls.push_back(Text.size());
          Text.push_back({});
        }
        Text.push_back(encode(Opcode::EXIT));
        const size_t Helper = Text.size();
        Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
        const size_t PriorCall = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::LD_DW_REG, 5, 6));
        // The non-affine decode cannot be recovered merely by applying the
        // generic entry-load expression. Reuse must retain the actual proof.
        Text.push_back(encode(Opcode::XOR64_IMM, 5, 0, 0, 1));
        const size_t CallX = Text.size();
        Text.push_back(encode(Opcode::CALL_REG, 5));
        Text.push_back(encode(Opcode::EXIT));
        const size_t FirstTarget = Text.size();
        Literal(3, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 3));
        Text.push_back(encode(Opcode::ST_DW_IMM, 2));
        if (KnownReturn) {
          Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
        } else {
          Text.push_back(encode(Opcode::LD_DW_REG, 0, 3, 8));
        }
        Text.push_back(encode(Opcode::EXIT));
        const size_t SecondTarget = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 73));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Noop = Text.size();
        Text.push_back(encode(Opcode::EXIT));
        Text[PriorCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Noop - PriorCall - 1);
        for (unsigned I = 0; I != Calls.size(); ++I) {
          const size_t Target =
              ChangedTarget && I >= kCallXContextSpecializationsPerEntry
                  ? SecondTarget
                  : FirstTarget;
          const auto Words = test::loadImm64(
              2, (kBytecodeStart + Target * kInstructionSize) ^ 1);
          std::copy(Words.begin(), Words.end(), Text.begin() + Targets[I]);
          Text[Calls[I]] =
              encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Calls[I] - 1);
        }
        auto Program = analyze(test::makeImage(Text, {}));
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        if (Reverse)
          std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        checkSummaryReferences(*Report);
        EXPECT_FALSE(Report->SpecializationBudgetExhausted);
        for (unsigned I = 0; I != Calls.size(); ++I) {
          bool Proven = false;
          for (const auto &Site : Report->Contexts)
            if (!Site.EntryPrefixOnly && Site.CallerSlot == Calls[I] &&
                Site.CallXSlot == CallX && Site.TargetSlot) {
              Proven = true;
              EXPECT_EQ(*Site.TargetSlot,
                        ChangedTarget &&
                                I >= kCallXContextSpecializationsPerEntry
                            ? SecondTarget
                            : FirstTarget);
            }
          if (!ChangedTarget || I < kCallXContextSpecializationsPerEntry)
            EXPECT_TRUE(Proven) << "call " << I;
        }
        for (uint64_t Value :
             {uint64_t{0}, uint64_t{1}, uint64_t{42}, uint64_t{1} << 32}) {
          ExecutionEnvironment Environment;
          std::vector<uint8_t> Input(16);
          llvm::support::endian::write64le(
              Input.data(),
              initialFramePointer(Program->Low.TheVersion, Program->Config) -
                  80);
          llvm::support::endian::write64le(Input.data() + 8, Value);
          Environment.Memory.push_back({kInputStart, Input, false, "input"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, ChangedTarget ? 73
                                      : KnownReturn ? 42
                                                    : Value);
          ++RawExecutions;
        }
      }
  EXPECT_EQ(RawExecutions, 32u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, OrdersOffsetsOfTheSameCapturedValueWithoutWrap) {
  struct Case {
    Opcode Predicate;
    int32_t Left, Right;
    bool Independent;
    std::optional<bool> Taken;
    unsigned Mask = 0;
    bool Truncate = false;
  };
  const std::array Cases{
      Case{Opcode::JGT64_REG, -24, 0, false, false},
      Case{Opcode::JGE64_REG, 0, -24, false, true},
      Case{Opcode::JLT64_REG, -24, 0, false, true},
      Case{Opcode::JLE64_REG, 0, -24, false, false},
      Case{Opcode::JEQ64_REG, -24, -24, false, true},
      Case{Opcode::JNE64_REG, -24, -16, false, true},
      Case{Opcode::JEQ64_REG, -24, -16, false, false},
      Case{Opcode::JNE64_REG, -24, -24, false, false},
      // Equality is modular; ordered comparisons additionally need both
      // displacements to stay within the captured value's numeric bounds.
      Case{Opcode::JEQ64_REG, 24, 24, false, true},
      Case{Opcode::JGT64_REG, 24, 0, false, std::nullopt},
      Case{Opcode::JLT64_REG, 0, 24, false, std::nullopt},
      Case{Opcode::JSGT64_REG, -24, 0, false, std::nullopt},
      Case{Opcode::JEQ64_REG, 0, 0, true, std::nullopt},
      Case{Opcode::JEQ64_REG, 0, 0, false, true, 1},
      Case{Opcode::JEQ64_REG, 0, 0, false, true, 2},
      Case{Opcode::JEQ64_REG, 0, 0, false, std::nullopt, 3},
      Case{Opcode::JGT64_REG, -24, 0, false, std::nullopt, 0, true}};
  const uint64_t Lower = kHeapStart + 8;
  size_t RawExecutions = 0;
  for (const auto &C : Cases)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << static_cast<unsigned>(C.Predicate) << ", left=" << C.Left
                   << ", right=" << C.Right << ", independent=" << C.Independent
                   << ", mask=" << C.Mask << ", truncate=" << C.Truncate
                   << ", reverse=" << Reverse);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      Literal(1, kInputStart);
      const size_t AcquireCall = Text.size();
      Text.push_back({});
      const size_t Null = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
      std::optional<size_t> OtherCall, OtherNull;
      if (C.Independent) {
        Literal(1, kInputStart + 8);
        OtherCall = Text.size();
        Text.push_back({});
        OtherNull = Text.size();
        Text.push_back({});
      }
      Text.push_back(encode(Opcode::MOV64_REG, 1, 6));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, C.Left));
      Text.push_back(encode(Opcode::MOV64_REG, 2, C.Independent ? 0 : 6));
      Text.push_back(encode(Opcode::ADD64_IMM, 2, 0, 0, C.Right));
      if (C.Mask == 2) {
        Text.push_back(encode(Opcode::MOV64_IMM, 3, 0, 0, -1));
        Text.push_back(encode(Opcode::AND64_REG, 1, 3));
      } else if (C.Mask) {
        Text.push_back(
            encode(Opcode::AND64_IMM, 1, 0, 0, C.Mask == 1 ? -1 : -8));
      }
      if (C.Truncate) {
        Text.push_back(encode(Opcode::MOV32_REG, 1, 1));
        Text.push_back(encode(Opcode::MOV32_REG, 2, 2));
      }
      const size_t Branch = Text.size();
      Text.push_back({});
      const size_t FalseLiteral = Literal(5, 0);
      const size_t JoinBranch = Text.size();
      Text.push_back({});
      const size_t TrueLiteral = Literal(5, 0);
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 5));
      const size_t Exit = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      // The input address is bounded, but neither allocator freshness nor a
      // heap initialization is assumed. Two calls can return the same value.
      const size_t Acquire = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
      Literal(2, Lower);
      Text.push_back(encode(Opcode::JGE64_REG, 0, 2, 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t FalseTarget = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t TrueTarget = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 73));
      Text.push_back(encode(Opcode::EXIT));
      for (size_t Slot : {AcquireCall, OtherCall.value_or(AcquireCall)})
        Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Acquire - Slot - 1);
      for (size_t Slot : {Null, OtherNull.value_or(Null)})
        Text[Slot] = encode(Opcode::JEQ64_IMM, 0, 0, Exit - Slot - 1);
      Text[Branch] = encode(C.Predicate, 1, 2, TrueLiteral - Branch - 1);
      Text[JoinBranch] = encode(Opcode::JA, 0, 0, CallX - JoinBranch - 1);
      for (const auto &[Slot, Target] : {std::pair{FalseLiteral, FalseTarget},
                                         std::pair{TrueLiteral, TrueTarget}}) {
        const auto Words = test::loadImm64(5, kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (Reverse)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      bool Proven = false;
      for (const auto &Site : Report->Contexts)
        if (!Site.EntryPrefixOnly && Site.CallXSlot == CallX &&
            Site.TargetSlot) {
          Proven = true;
          ASSERT_TRUE(C.Taken.has_value());
          EXPECT_EQ(*Site.TargetSlot, *C.Taken ? TrueTarget : FalseTarget);
        }
      EXPECT_EQ(Proven, C.Taken.has_value());
      bool SawTrue = false, SawFalse = false;
      for (uint64_t Value :
           {Lower + 64, UINT64_MAX - 8, (uint64_t{1} << 63) + 8,
            (uint64_t{4} << 32) + 8, uint64_t{0}})
        for (bool Same : {false, true}) {
          const uint64_t Other = Same ? Value : Lower + 128;
          uint64_t Left = Value + static_cast<uint64_t>(C.Left);
          uint64_t Right =
              (C.Independent ? Other : Value) + static_cast<uint64_t>(C.Right);
          if (C.Mask)
            Left &= C.Mask == 3 ? ~uint64_t{7} : UINT64_MAX;
          if (C.Truncate) {
            Left = static_cast<uint32_t>(Left);
            Right = static_cast<uint32_t>(Right);
          }
          bool Taken = false;
          switch (C.Predicate) {
          case Opcode::JGT64_REG:
            Taken = Left > Right;
            break;
          case Opcode::JGE64_REG:
            Taken = Left >= Right;
            break;
          case Opcode::JLT64_REG:
            Taken = Left < Right;
            break;
          case Opcode::JLE64_REG:
            Taken = Left <= Right;
            break;
          case Opcode::JEQ64_REG:
            Taken = Left == Right;
            break;
          case Opcode::JNE64_REG:
            Taken = Left != Right;
            break;
          case Opcode::JSGT64_REG:
            Taken = static_cast<int64_t>(Left) > static_cast<int64_t>(Right);
            break;
          default:
            FAIL() << "unexpected comparison";
          }
          std::vector<uint8_t> Input(16);
          llvm::support::endian::write64le(Input.data(), Value);
          llvm::support::endian::write64le(Input.data() + 8, Other);
          ExecutionEnvironment Environment;
          Environment.Memory.push_back({kInputStart, Input, false, "input"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          const bool NullReturn =
              Value < Lower || (C.Independent && Other < Lower);
          EXPECT_EQ(Raw->ReturnValue, NullReturn ? 0u : Taken ? 73u : 42u);
          if (!NullReturn) {
            SawTrue |= Taken;
            SawFalse |= !Taken;
          }
          ++RawExecutions;
        }
      if (!C.Taken) {
        EXPECT_TRUE(SawTrue);
        EXPECT_TRUE(SawFalse);
      }
    }
  EXPECT_EQ(RawExecutions, 340u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, ReplayedStoredReturnsKeepTheirCallSiteIdentity) {
  unsigned RawExecutions = 0;
  for (unsigned Mode = 0; Mode != 4; ++Mode)
    for (bool Wrapped : {false, true})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(::testing::Message() << Mode << ", wrapped=" << Wrapped
                                          << ", reverse=" << Reverse);
        std::vector<test::EncodedInstruction> Text;
        const auto Literal = [&](unsigned R, uint64_t Value) {
          const size_t Slot = Text.size();
          const auto Words = test::loadImm64(R, Value);
          Text.insert(Text.end(), Words.begin(), Words.end());
          return Slot;
        };
        Literal(1, kInputStart);
        const size_t AcquireCall = Text.size();
        Text.push_back({});
        std::vector<size_t> Nulls{Text.size()};
        Text.push_back({});
        Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
        Literal(7, kHeapStart);
        std::vector<size_t> Calls;
        for (unsigned I = 0; I != kCallXContextSpecializationsPerEntry; ++I) {
          Literal(3, kHeapStart + 4096 + 128 * I);
          Text.push_back(encode(Opcode::ST_DW_REG, 7, 3));
          Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, (I + 1) * 8));
          Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, 1));
          Calls.push_back(Text.size());
          Text.push_back({});
        }
        Text.push_back(encode(Opcode::ST_DW_REG, 7, 6));
        for (unsigned I = 0; I != 2; ++I) {
          Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, 24));
          Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, 8));
          Calls.push_back(Text.size());
          Text.push_back({});
          Nulls.push_back(Text.size());
          Text.push_back({});
          if (I == 0) {
            Text.push_back(encode(Opcode::MOV64_REG, 8, 0));
            if (Mode == 3)
              Text.push_back(encode(Opcode::ST_DW_REG, 7, 6));
          }
        }
        if (Mode == 1) {
          Text.push_back(encode(Opcode::MOV64_REG, 3, 0));
          Text.push_back(encode(Opcode::ADD64_IMM, 3, 0, 0, 8));
          Text.push_back(encode(Opcode::ST_DW_REG, 7, 3));
        }
        Text.push_back(encode(Opcode::LD_DW_REG, 3, 7));
        const size_t EqualTarget = Literal(5, 0);
        Text.push_back(encode(Opcode::JEQ64_REG, Mode >= 2 ? 8 : 0, 3, 2));
        const size_t DifferentTarget = Literal(5, 0);
        const size_t CallX = Text.size();
        Text.push_back(encode(Opcode::CALL_REG, 5));
        const size_t Exit = Text.size();
        Text.push_back(encode(Opcode::EXIT));
        const size_t Acquire = Text.size();
        Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
        Literal(2, kHeapStart + 8);
        Text.push_back(encode(Opcode::JGE64_REG, 0, 2, 2));
        Text.push_back(encode(Opcode::MOV64_IMM, 0));
        Text.push_back(encode(Opcode::EXIT));
        Literal(2, kHeapStart + 4096);
        Text.push_back(encode(Opcode::JLE64_REG, 0, 2, 1));
        Text.push_back(encode(Opcode::MOV64_IMM, 0));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Wrapper = Text.size();
        if (Wrapped) {
          Text.push_back(encode(Opcode::CALL_IMM, 0, 1, 0, 1));
          Text.push_back(encode(Opcode::EXIT));
        }
        Literal(3, kHeapStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 3, 3));
        Text.push_back(encode(Opcode::MOV64_REG, 4, 3));
        Text.push_back(encode(Opcode::SUB64_REG, 3, 1));
        Text.push_back(encode(Opcode::MOV64_IMM, 0));
        const size_t Underflow = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::NEG64, 2));
        Text.push_back(encode(Opcode::AND64_REG, 3, 2));
        Literal(2, kHeapStart + 8);
        const size_t Reject = Text.size();
        Text.push_back({});
        Literal(2, kHeapStart);
        Text.push_back(encode(Opcode::ST_DW_REG, 2, 3));
        Text.push_back(encode(Opcode::MOV64_REG, 0, 3));
        const size_t AllocateExit = Text.size();
        Text.push_back(encode(Opcode::EXIT));
        const size_t Equal = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Different = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 73));
        Text.push_back(encode(Opcode::EXIT));
        Text[AcquireCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Acquire - AcquireCall - 1);
        for (size_t S : Calls)
          Text[S] = encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - S - 1);
        for (size_t S : Nulls)
          Text[S] = encode(Opcode::JEQ64_IMM, 0, 0, Exit - S - 1);
        Text[Underflow] =
            encode(Opcode::JGT64_REG, 3, 4, AllocateExit - Underflow - 1);
        Text[Reject] =
            encode(Opcode::JLT64_REG, 3, 2, AllocateExit - Reject - 1);
        for (const auto [S, Target] : {std::pair{EqualTarget, Equal},
                                       std::pair{DifferentTarget, Different}}) {
          const auto Words = test::loadImm64(5, kBytecodeStart + Target * 8);
          std::copy(Words.begin(), Words.end(), Text.begin() + S);
        }
        auto Program = analyze(test::makeImage(Text, {}));
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        if (Reverse)
          std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        checkSummaryReferences(*Report);
        EXPECT_FALSE(Report->SpecializationBudgetExhausted);
        EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
          return Edge.CallerSlot == Calls.back() && Edge.GeneralizedInputs &&
                 Edge.InputGeneralization ==
                     CallXInputGeneralization::SpecializationLimit;
        }));
        EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
          return Edge.CallerSlot == Calls.back() &&
                 Edge.InstantiatedMemoryPathInstructions > 0;
        }));
        bool Proven = false;
        for (const auto &Site : Report->Contexts)
          if (!Site.EntryPrefixOnly && !Site.GeneralizedInputs &&
              Site.CallXSlot == CallX && Site.TargetSlot) {
            Proven = true;
            EXPECT_EQ(*Site.TargetSlot,
                      Mode == 1 || Mode == 2 ? Different : Equal);
          }
        if (Mode < 2)
          EXPECT_TRUE(Proven);
        for (uint64_t Offset : {16u, 32u, 64u, 73u, 128u}) {
          std::vector<uint8_t> Input(8);
          llvm::support::endian::write64le(Input.data(), kHeapStart + Offset);
          ExecutionEnvironment Environment;
          Environment.Memory.push_back({kInputStart, Input, false, "input"});
          Environment.Memory.push_back(
              {kHeapStart, std::vector<uint8_t>(8192), true, "heap"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          const uint64_t First = (Offset - 24) & ~uint64_t{7};
          const bool Null = Offset < 32 || (Mode != 3 && First < 32);
          EXPECT_EQ(Raw->ReturnValue, Null                     ? 0u
                                      : Mode == 1 || Mode == 2 ? 73u
                                                               : 42u);
          ++RawExecutions;
        }
      }
  EXPECT_EQ(RawExecutions, 80u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFStoredRegisterMemory, CapturedDifferencesRequirePathAndIdentityProofs) {
  auto Space = std::make_shared<SymbolicAddressSpace>(
      5, UnsignedValueRange{kHeapStart + 8, kHeapStart + 4096});
  const RegisterValue Old{RegisterValue::Kind::SymbolicAddress, 0, 0};
  const RegisterValue Aligned{RegisterValue::Kind::SymbolicAddress, 1, 0};
  const RegisterValue Next{RegisterValue::Kind::SymbolicAddress, 2, 0};
  const RegisterValue Target{RegisterValue::Kind::Constant, 42, 0};
  const RegisterValue Replacement{RegisterValue::Kind::Constant, 73, 0};
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    StoredRegisterMemory Before;
    Before.setSymbolicAddresses(Space);
    if (Reverse)
      Before.refineCapturedDifference(Old, Aligned, {24, 31});
    else
      Before.refineCapturedDifference(Aligned, Old,
                                      {uint64_t{0} - 31, uint64_t{0} - 24});
    Before.writeValues(Old, Target);
    auto Disjoint = Before;
    Disjoint.writeValues(Aligned, Replacement);
    EXPECT_EQ(Disjoint.readValues(Old).singleton(), Target);
    EXPECT_EQ(Disjoint.readValues(Aligned).singleton(), Replacement);
    for (int64_t Offset : {17, 24, 31, 38}) {
      auto Overlapping = Before;
      auto Address = Aligned;
      Address.Offset = Offset;
      Overlapping.writeValues(Address, Replacement);
      EXPECT_TRUE(Overlapping.readValues(Old).unknown()) << Offset;
    }
    auto Chained = Disjoint;
    Chained.refineCapturedDifference(Next, Aligned,
                                     {uint64_t{0} - 23, uint64_t{0} - 16});
    Chained.writeValues(Next, Target);
    EXPECT_EQ(Chained.readValues(Old).singleton(), Target);
    EXPECT_EQ(Chained.readValues(Aligned).singleton(), Replacement);

    auto Joined = Before;
    auto Unrelated = Before;
    Unrelated.discardCapturedRanges();
    Joined.joinValues(Unrelated);
    Joined.writeValues(Aligned, Replacement);
    EXPECT_TRUE(Joined.readValues(Old).unknown());

    auto AliasingBranch = Unrelated;
    AliasingBranch.refineCapturedDifference(Aligned, Old, {0, 7});
    auto BothBranches = Before;
    BothBranches.joinValues(AliasingBranch);
    BothBranches.writeValues(Aligned, Replacement);
    EXPECT_TRUE(BothBranches.readValues(Old).unknown());

    // Equal proven addresses share fields in either direction. Merely
    // overlapping intervals, or a relation absent on one path, cannot do so.
    auto Aliases = Before;
    Aliases.refineCapturedDifference(Next, Old, {16, 16});
    const RegisterValue OldField{Old.ValueKind, Old.Value, 16};
    Aliases.writeValues(Next, Target);
    EXPECT_EQ(Aliases.readValues(OldField).singleton(), Target);
    Aliases.writeValues(OldField, Replacement);
    EXPECT_EQ(Aliases.readValues(Next).singleton(), Replacement);
    Aliases.discardCapturedRanges();
    EXPECT_TRUE(Aliases.readValues(Next).unknown());

    auto Renamed = Before;
    Renamed.renameCapture(0, 4);
    Renamed.writeValues(Aligned, Replacement);
    const RegisterValue RenamedOld{Old.ValueKind, 4, 0};
    EXPECT_EQ(Renamed.readValues(RenamedOld).singleton(), Target);
    EXPECT_TRUE(Renamed.readValues(Old).unknown());

    Before.refineCapturedRange(Old, {kHeapStart + 64, kHeapStart + 512});
    auto Rebound = Before;
    const auto Bind = [](RegisterValue Value) {
      if (Value.Value == 0) {
        Value.Value = 3;
        Value.Offset += 8;
      }
      return Value;
    };
    Rebound.transformSymbols(Bind);
    Rebound.refineCapturedRanges(Before, Bind);
    EXPECT_EQ(Rebound.valueRange(Bind(Old)), Before.valueRange(Old));

    Rebound.writeValues(Aligned, Replacement);
    EXPECT_EQ(Rebound.readValues(Bind(Old)).singleton(), Target);
    EXPECT_NE(Before, Unrelated);
    EXPECT_GT(Before.retainedByteEstimate(), Unrelated.retainedByteEstimate());
    llvm::FoldingSetNodeID A, B;
    Before.profile(A);
    Unrelated.profile(B);
    EXPECT_NE(A, B);
  }
}

TEST(SBFCallXContexts, UpdatedSinglePredecessorReplacesTransientCallResults) {
  unsigned RawExecutions = 0;
  for (bool Reverse : {false, true}) {
    SCOPED_TRACE(Reverse);
    std::vector<test::EncodedInstruction> Text;
    const auto Literal = [&](unsigned R, uint64_t Value) {
      const size_t Slot = Text.size();
      const auto Words = test::loadImm64(R, Value);
      Text.insert(Text.end(), Words.begin(), Words.end());
      return Slot;
    };
    Literal(1, kInputStart + 24);
    Text.push_back(encode(Opcode::LD_B_REG, 9, 1));
    const size_t ChooseA = Text.size();
    Text.push_back({});
    const size_t ChooseB = Text.size();
    Text.push_back({});
    std::array<size_t, 3> Arms, AcquireCalls, Joins;
    for (unsigned I : {2u, 0u, 1u}) {
      Arms[I] = Text.size();
      // The long arm reaches the shared constructor after its first solve.
      for (unsigned J = 0; J < (I == 2 ? 4u : 0u); ++J)
        Text.push_back(encode(Opcode::JA));
      Literal(1, kInputStart + I * 8);
      AcquireCalls[I] = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 2, 0));
      Joins[I] = Text.size();
      Text.push_back({});
    }
    const size_t Join = Text.size();
    const size_t ConstructCall = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::ST_DW_REG, 10, 0, -8));
    Text.push_back(encode(Opcode::LD_DW_REG, 8, 10, -8));
    const size_t Null = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::LD_DW_REG, 5, 8, 7));
    const size_t CallX = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 5));
    const size_t Exit = Text.size();
    Text.push_back(encode(Opcode::EXIT));
    const size_t Construct = Text.size();
    Text.push_back(encode(Opcode::MOV64_REG, 6, 2));
    Literal(1, kInputStart + 32);
    const size_t ObjectCall = Text.size();
    Text.push_back({});
    const size_t ObjectNull = Text.size();
    Text.push_back({});
    const size_t TargetLiteral = Literal(5, 0);
    Text.push_back(encode(Opcode::ST_DW_REG, 0, 5, 8));
    Text.push_back(encode(Opcode::ST_DW_REG, 0, 6));
    Text.push_back(encode(Opcode::ADD64_IMM, 0, 0, 0, 1));
    const size_t ObjectExit = Text.size();
    Text.push_back(encode(Opcode::EXIT));
    const size_t Acquire = Text.size();
    Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
    Literal(2, kHeapStart + 8);
    Text.push_back(encode(Opcode::JGE64_REG, 0, 2, 2));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    Literal(2, kHeapStart + 4096);
    Text.push_back(encode(Opcode::JLE64_REG, 0, 2, 1));
    Text.push_back(encode(Opcode::MOV64_IMM, 0));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Target = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    Text[ChooseA] = encode(Opcode::JEQ64_IMM, 9, 0, Arms[0] - ChooseA - 1, 0);
    Text[ChooseB] = encode(Opcode::JEQ64_IMM, 9, 0, Arms[1] - ChooseB - 1, 1);
    for (unsigned I = 0; I != 3; ++I) {
      Text[AcquireCalls[I]] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Acquire - AcquireCalls[I] - 1);
      Text[Joins[I]] = encode(Opcode::JA, 0, 0, Join - Joins[I] - 1);
    }
    Text[ConstructCall] =
        encode(Opcode::CALL_IMM, 0, 1, 0, Construct - ConstructCall - 1);
    Text[ObjectCall] =
        encode(Opcode::CALL_IMM, 0, 1, 0, Acquire - ObjectCall - 1);
    Text[Null] = encode(Opcode::JEQ64_IMM, 8, 0, Exit - Null - 1);
    Text[ObjectNull] =
        encode(Opcode::JEQ64_IMM, 0, 0, ObjectExit - ObjectNull - 1);
    const auto TargetWords = test::loadImm64(5, kBytecodeStart + Target * 8);
    std::copy(TargetWords.begin(), TargetWords.end(),
              Text.begin() + TargetLiteral);
    auto Program = analyze(test::makeImage(Text, {}));
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    if (Reverse)
      std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    EXPECT_FALSE(Report->SpecializationBudgetExhausted);
    bool Proven = false;
    for (const auto &Site : Report->Contexts)
      if (!Site.EntryPrefixOnly && Site.CallXSlot == CallX && Site.TargetSlot) {
        EXPECT_EQ(*Site.TargetSlot, Target);
        Proven = true;
      }
    EXPECT_TRUE(Proven);
    for (unsigned Selector = 0; Selector != 3; ++Selector) {
      ExecutionEnvironment Environment;
      std::vector<uint8_t> Input(40);
      for (unsigned I = 0; I != 3; ++I)
        llvm::support::endian::write64le(Input.data() + I * 8,
                                         kHeapStart + 128 + 8 * I);
      Input[24] = Selector;
      llvm::support::endian::write64le(Input.data() + 32, kHeapStart + 64);
      Environment.Memory.push_back({kInputStart, Input, false, "input"});
      Environment.Memory.push_back(
          {kHeapStart, std::vector<uint8_t>(256), true, "objects"});
      auto Raw = executeRaw(*Program, std::move(Environment));
      ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
      EXPECT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
      EXPECT_EQ(Raw->ReturnValue, 42u);
      ++RawExecutions;
    }
  }
  EXPECT_EQ(RawExecutions, 6u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, CapturedCursorOffsetsKeepConditionalWritesAndAliases) {
  unsigned RawExecutions = 0;
  for (unsigned Mode = 0; Mode != 8; ++Mode)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "mode=" << Mode << ", reverse=" << Reverse);
      const unsigned Alignment = Mode == 3 || Mode >= 6 ? 8 : 1;
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      Literal(1, kInputStart);
      std::vector<size_t> AcquireCalls{Text.size()};
      Text.push_back({});
      std::vector<size_t> Nulls{Text.size()};
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 6, 0));
      const size_t InitialTarget = Literal(3, 0);
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 3));
      Literal(7, kHeapStart);
      std::vector<size_t> AllocateCalls;
      for (unsigned I = 0; I != kCallXContextSpecializationsPerEntry; ++I) {
        Literal(3, kHeapStart + 4096 + 128 * I);
        Text.push_back(encode(Opcode::ST_DW_REG, 7, 3));
        Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, (I + 1) * 8));
        Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, 1));
        AllocateCalls.push_back(Text.size());
        Text.push_back({});
      }
      if (Mode == 2) {
        Literal(1, kInputStart + 8);
        AcquireCalls.push_back(Text.size());
        Text.push_back({});
        Nulls.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::MOV64_REG, 3, 0));
      } else {
        Text.push_back(encode(Opcode::MOV64_REG, 3, 6));
        if (Mode == 1)
          Text.push_back(encode(Opcode::ADD64_IMM, 3, 0, 0, 24));
      }
      // Resetting the cursor can deliberately alias an older object. The
      // fixed-cell path must follow this exact value, not assume freshness.
      Text.push_back(encode(Opcode::ST_DW_REG, 7, 3));
      std::vector<size_t> Replacements;
      for (unsigned Allocation = 0; Allocation != 2; ++Allocation) {
        const unsigned Size = Allocation == 0 || Mode >= 4 ? 24 : 16;
        Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, Size));
        Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, Alignment));
        AllocateCalls.push_back(Text.size());
        Text.push_back({});
        Nulls.push_back(Text.size());
        Text.push_back({});
        if (Mode >= 4 && Allocation == 0)
          Text.push_back(encode(Opcode::MOV64_REG, 8, 0));
        Replacements.push_back(Literal(3, 0));
        Text.push_back(encode(Opcode::ST_DW_REG, 0, 3));
        if ((Mode == 5 || Mode == 7) && Allocation == 0)
          Text.push_back(encode(Opcode::ST_DW_REG, 7, 6));
      }
      Text.push_back(encode(Opcode::LD_DW_REG, 5, Mode >= 4 ? 8 : 6));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 5));
      const size_t Exit = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t Acquire = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
      Literal(2, kHeapStart + 8);
      Text.push_back(encode(Opcode::JGE64_REG, 0, 2, 2));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      Literal(2, kHeapStart + 4096);
      Text.push_back(encode(Opcode::JLE64_REG, 0, 2, 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Allocate = Text.size();
      Literal(3, kHeapStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 3, 3));
      Literal(4, kHeapStart + 32768);
      Text.push_back(encode(Opcode::JEQ64_IMM, 3, 0, 1));
      Text.push_back(encode(Opcode::MOV64_REG, 4, 3));
      Text.push_back(encode(Opcode::MOV64_REG, 3, 4));
      Text.push_back(encode(Opcode::SUB64_REG, 3, 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::MOV64_IMM, 5, 0, 0, 1));
      Text.push_back(encode(Opcode::JGT64_REG, 3, 4, 1));
      Text.push_back(encode(Opcode::MOV64_IMM, 5));
      Text.push_back(encode(Opcode::MOV64_IMM, 1));
      Text.push_back(encode(Opcode::JNE64_IMM, 5, 0, 1));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 3));
      Text.push_back(encode(Opcode::NEG64, 2));
      Text.push_back(encode(Opcode::AND64_REG, 1, 2));
      Literal(2, kHeapStart + 8);
      const size_t Reject = Text.size();
      Text.push_back({});
      Literal(2, kHeapStart);
      Text.push_back(encode(Opcode::ST_DW_REG, 2, 1));
      Text.push_back(encode(Opcode::MOV64_REG, 0, 1));
      const size_t AllocateExit = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t First = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Second = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 73));
      Text.push_back(encode(Opcode::EXIT));
      for (size_t S : AcquireCalls)
        Text[S] = encode(Opcode::CALL_IMM, 0, 1, 0, Acquire - S - 1);
      for (size_t S : AllocateCalls)
        Text[S] = encode(Opcode::CALL_IMM, 0, 1, 0, Allocate - S - 1);
      for (size_t S : Nulls)
        Text[S] = encode(Opcode::JEQ64_IMM, 0, 0, Exit - S - 1);
      Text[Reject] = encode(Opcode::JGT64_REG, 2, 1, AllocateExit - Reject - 1);
      for (unsigned I = 0; I != Replacements.size(); ++I) {
        const size_t S = Replacements[I];
        const size_t Target = Mode >= 4 && I == 0 ? First : Second;
        const auto Words = test::loadImm64(3, kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + S);
      }
      const auto Words = test::loadImm64(3, kBytecodeStart + First * 8);
      std::copy(Words.begin(), Words.end(), Text.begin() + InitialTarget);
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (Reverse)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      bool Proven = false;
      for (const auto &Site : Report->Contexts)
        if (!Site.EntryPrefixOnly && Site.CallXSlot == CallX &&
            Site.TargetSlot) {
          Proven = true;
          EXPECT_TRUE(Mode != 2 && Mode != 7);
          EXPECT_EQ(*Site.TargetSlot,
                    Mode == 1 || Mode == 5 || Mode == 7 ? Second : First);
        }
      // Repeating a masked allocation can alias an earlier result. Without
      // equality of the masked expressions, retain the dynamic fallback.
      EXPECT_EQ(Proven, Mode != 2 && Mode != 7);
      for (uint64_t Offset : {16u, 32u, 64u, 73u})
        for (bool Alias : {false, true}) {
          const uint64_t Pointer = kHeapStart + Offset;
          const uint64_t Other = Pointer + (Alias ? 24 : 128);
          uint64_t Cursor = Mode == 2 ? Other : Pointer + (Mode == 1 ? 24 : 0);
          bool Null = false, Overlap = false;
          uint64_t ReadPointer = Pointer;
          for (unsigned Allocation = 0; Allocation != 2; ++Allocation) {
            const uint64_t Size = Allocation == 0 || Mode >= 4 ? 24 : 16;
            if ((Mode == 5 || Mode == 7) && Allocation == 1)
              Cursor = Pointer;
            Cursor = (Cursor - Size) & (uint64_t{0} - Alignment);
            if (Cursor < kHeapStart + 8) {
              Null = true;
              break;
            }
            if (Mode >= 4 && Allocation == 0)
              ReadPointer = Cursor;
            else
              Overlap |= Cursor == ReadPointer;
          }
          std::vector<uint8_t> Input(16);
          llvm::support::endian::write64le(Input.data(), Pointer);
          llvm::support::endian::write64le(Input.data() + 8, Other);
          ExecutionEnvironment Environment;
          Environment.Memory.push_back({kInputStart, Input, false, "input"});
          Environment.Memory.push_back(
              {kHeapStart, std::vector<uint8_t>(8192), true, "heap"});
          auto Raw = executeRaw(*Program, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, Null ? 0u : Overlap ? 73u : 42u);
          ++RawExecutions;
        }
    }
  EXPECT_EQ(RawExecutions, 128u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, TaggedReturnsRestoreGuardedAbsoluteFacts) {
  unsigned RawExecutions = 0;
  for (unsigned Mode = 0; Mode != 6; ++Mode)
    for (bool ByteTag : {false, true})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << "mode=" << Mode << ", byte=" << ByteTag
                     << ", reverse=" << Reverse);
        const unsigned Count =
            Mode == 3 ? kCallXContextSpecializationsPerEntry + 2 : 1;
        std::vector<test::EncodedInstruction> Text;
        const auto Literal = [&](unsigned Register, uint64_t Value) {
          const size_t Slot = Text.size();
          const auto Words = test::loadImm64(Register, Value);
          Text.insert(Text.end(), Words.begin(), Words.end());
          return Slot;
        };
        Literal(7, kHeapStart);
        Literal(8, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 9, 8));
        std::vector<size_t> Calls;
        for (unsigned I = 0; I != Count; ++I) {
          // This cell is never read or written by the helper. A projected
          // summary must not restore a different invocation's old snapshot.
          Text.push_back(encode(Opcode::ST_DW_IMM, 7, 0, 16, I));
          Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
          Text.push_back(encode(Opcode::MOV64_REG, 2, 9));
          Calls.push_back(Text.size());
          Text.push_back({});
        }
        if (Mode == 1 || Mode == 4)
          Text.push_back(encode(
              Mode == 4 ? Opcode::ST_B_IMM : Opcode::ST_DW_IMM, 7, 0, 0, 73));
        if (Mode == 2) {
          Text.push_back(encode(Opcode::LD_DW_REG, 3, 8, 8));
          Text.push_back(encode(Opcode::ST_DW_IMM, 3, 0, 0, 73));
        }
        if (Mode == 5)
          Text.push_back(encode(ByteTag ? Opcode::ST_B_IMM : Opcode::ST_DW_IMM,
                                10, 0, -16));
        Text.push_back(
            encode(ByteTag ? Opcode::LD_B_REG : Opcode::LD_DW_REG, 1, 10, -16));
        const size_t Skip = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::LD_DW_REG, 2, 7, Mode == 3 ? 16 : 0));
        const size_t AValue = Literal(5, 0);
        Text.push_back(
            encode(Opcode::JEQ64_IMM, 2, 0, 2, Mode == 3 ? Count - 1 : 42));
        const size_t BValue = Literal(5, 0);
        const size_t CallX = Text.size();
        Text.push_back(encode(Opcode::CALL_REG, 5));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Empty = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0));
        Text.push_back(encode(Opcode::EXIT));
        Text[Skip] = encode(Opcode::JNE64_IMM, 1, 0, Empty - Skip - 1, 1);
        const size_t Helper = Text.size();
        Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
        // Give the specialized summary a useful scalar result as well, so
        // the changing unread cell actually exercises conditional reuse.
        Text.push_back(encode(Opcode::MOV64_REG, 0, 1));
        Literal(3, kHeapStart);
        const size_t Choice = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::ST_DW_IMM, 3, 0, 0, 42));
        Text.push_back(encode(Opcode::ST_DW_IMM, 6, 0, 8, 42));
        Text.push_back(encode(Opcode::MOV64_IMM, 1, 0, 0, 1));
        const size_t Join = Text.size();
        Text.push_back({});
        const size_t Other = Text.size();
        Text.push_back(encode(Opcode::ST_DW_IMM, 3, 0, 0, 73));
        Text.push_back(encode(Opcode::ST_DW_IMM, 6, 0, 8, 73));
        Text.push_back(encode(Opcode::MOV64_IMM, 1));
        const size_t Tail = Text.size();
        Text.push_back(
            encode(ByteTag ? Opcode::ST_B_REG : Opcode::ST_DW_REG, 6, 1));
        Text.push_back(encode(Opcode::EXIT));
        Text[Choice] = encode(Opcode::JEQ64_IMM, 2, 0, Other - Choice - 1);
        Text[Join] = encode(Opcode::JA, 0, 0, Tail - Join - 1);
        const size_t A = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
        Text.push_back(encode(Opcode::EXIT));
        const size_t B = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
        Text.push_back(encode(Opcode::EXIT));
        for (const auto &[Slot, Target] :
             {std::pair{AValue, A}, std::pair{BValue, B}}) {
          const auto Words =
              test::loadImm64(5, kBytecodeStart + Target * kInstructionSize);
          std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
        }
        for (size_t Slot : Calls)
          Text[Slot] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Slot - 1);
        auto Program = analyze(test::makeImage(Text, {}));
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        if (Reverse)
          std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        checkSummaryReferences(*Report);
        EXPECT_FALSE(Report->SpecializationBudgetExhausted);
        bool Proven = false;
        for (const auto &Site : Report->Contexts)
          if (!Site.EntryPrefixOnly && !Site.GeneralizedInputs &&
              Site.CallXSlot == CallX && Site.TargetSlot) {
            Proven = true;
            EXPECT_EQ(*Site.TargetSlot, Mode == 1 ? B : A);
          }
        EXPECT_EQ(Proven, Mode == 0 || Mode == 1 || Mode == 3);
        if (Mode == 3)
          EXPECT_TRUE(llvm::any_of(Report->SummaryEdges, [&](const auto &Edge) {
            return Edge.CallerSlot == Calls.back() &&
                   Edge.InputGeneralization ==
                       CallXInputGeneralization::EquivalentSpecializedInputs;
          }));
        for (uint64_t Selector : {0u, 1u})
          for (unsigned Destination = 0; Destination != 3; ++Destination) {
            std::vector<uint8_t> Input(16);
            llvm::support::endian::write64le(Input.data(), Selector);
            const uint64_t Address =
                Destination == 2 ? initialFramePointer(Program->Low.TheVersion,
                                                       Program->Config) -
                                       16
                                 : kHeapStart + Destination * 8;
            llvm::support::endian::write64le(Input.data() + 8, Address);
            ExecutionEnvironment Environment;
            Environment.Memory.push_back({kInputStart, Input, false, "input"});
            Environment.Memory.push_back(
                {kHeapStart, std::vector<uint8_t>(64), true, "globals"});
            auto Raw = executeRaw(*Program, std::move(Environment));
            ASSERT_TRUE(static_cast<bool>(Raw))
                << llvm::toString(Raw.takeError());
            ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
            const uint64_t Expected =
                !Selector || Mode == 5 || (Mode == 2 && Destination == 2) ? 0
                : Mode == 1 || Mode == 4 || (Mode == 2 && Destination == 0)
                    ? 43
                    : 42;
            EXPECT_EQ(Raw->ReturnValue, Expected);
            ++RawExecutions;
          }
      }
  EXPECT_EQ(RawExecutions, 144u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, SameTagPathsKeepTheirOwnCapturedPayloadFields) {
  unsigned RawExecutions = 0;
  for (bool UnknownField : {false, true})
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message() << UnknownField << ", " << Reverse);
      std::vector<test::EncodedInstruction> Text;
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
      Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, -16));
      const size_t Call = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_DW_REG, 1, 10, -16));
      const size_t EmptyBranch = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 10, -8));
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 2));
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 2));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Empty = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Helper = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
      Literal(3, kInputStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 2, 3));
      std::vector<size_t> Selects, Arms, Calls, Joins, Targets;
      for (unsigned I = 0; I != 3; ++I) {
        Selects.push_back(Text.size());
        Text.push_back({});
      }
      Text.push_back(encode(Opcode::ST_DW_IMM, 6, 0, 0, 0));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      const size_t SuccessJoin = Text.size();
      Text.push_back({});
      size_t FirstJoin = 0;
      for (unsigned I = 0; I != 3; ++I) {
        Arms.push_back(Text.size());
        Literal(1, kInputStart + 8);
        Calls.push_back(Text.size());
        Text.push_back({});
        if (UnknownField && I == 2) {
          Literal(2, kInputStart + 16);
          Text.push_back(encode(Opcode::LD_DW_REG, 5, 2));
        } else {
          Targets.push_back(Literal(5, 0));
        }
        Text.push_back(encode(Opcode::ST_DW_REG, 0, 5));
        if (I == 0) {
          Text.push_back(encode(Opcode::ST_DW_REG, 6, 0, 8));
          Text.push_back(encode(Opcode::ST_DW_IMM, 6, 0, 0, 1));
          FirstJoin = Text.size();
          Text.push_back({});
        } else {
          Joins.push_back(Text.size());
          Text.push_back({});
        }
      }
      const size_t SharedError = Text.size();
      Text.push_back(encode(Opcode::ST_DW_REG, 6, 0, 8));
      Text.push_back(encode(Opcode::ST_DW_IMM, 6, 0, 0, 1));
      const size_t ErrorJoin = Text.size();
      Text.push_back({});
      const size_t Tail = Text.size();
      Text.push_back(encode(Opcode::EXIT));
      const size_t Acquire = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 0, 1));
      Literal(2, kHeapStart + 8);
      const size_t Below = Text.size();
      Text.push_back({});
      Literal(2, kHeapStart + 128);
      const size_t Above = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::EXIT));
      const size_t Fallback = Literal(0, kHeapStart + 32);
      Text.push_back(encode(Opcode::EXIT));
      const size_t A = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t B = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      Text[Call] = encode(Opcode::CALL_IMM, 0, 1, 0, Helper - Call - 1);
      Text[EmptyBranch] =
          encode(Opcode::JEQ64_IMM, 1, 0, Empty - EmptyBranch - 1);
      Text[Below] = encode(Opcode::JLT64_REG, 0, 2, Fallback - Below - 1);
      Text[Above] = encode(Opcode::JGT64_REG, 0, 2, Fallback - Above - 1);
      for (unsigned I = 0; I != 3; ++I)
        Text[Selects[I]] =
            encode(Opcode::JEQ64_IMM, 2, 0, Arms[I] - Selects[I] - 1, I + 1);
      for (size_t At : Calls)
        Text[At] = encode(Opcode::CALL_IMM, 0, 1, 0, Acquire - At - 1);
      for (size_t At : Joins)
        Text[At] = encode(Opcode::JA, 0, 0, SharedError - At - 1);
      for (size_t At : {SuccessJoin, FirstJoin, ErrorJoin})
        Text[At] = encode(Opcode::JA, 0, 0, Tail - At - 1);
      for (size_t At : Targets) {
        const auto Words = test::loadImm64(5, kBytecodeStart + A * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + At);
      }
      auto Program = analyze(test::makeImage(Text, {}));
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (Reverse)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      EXPECT_EQ(llvm::any_of(Report->Contexts,
                             [&](const auto &Site) {
                               return Site.CallXSlot == CallX &&
                                      !Site.GeneralizedInputs &&
                                      Site.TargetSlot == A;
                             }),
                !UnknownField);
      for (unsigned Selector = 0; Selector != 4; ++Selector) {
        ExecutionEnvironment Environment;
        std::vector<uint8_t> Input(24);
        llvm::support::endian::write64le(Input.data(), Selector);
        llvm::support::endian::write64le(Input.data() + 8, kHeapStart + 32);
        llvm::support::endian::write64le(Input.data() + 16,
                                         kBytecodeStart + B * 8);
        Environment.Memory.push_back({kInputStart, Input, false, "input"});
        Environment.Memory.push_back(
            {kHeapStart, std::vector<uint8_t>(256), true, "heap"});
        auto Raw = executeRaw(*Program, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, Selector == 0                   ? 0u
                                    : UnknownField && Selector == 3 ? 43u
                                                                    : 42u);
        ++RawExecutions;
      }
    }
  EXPECT_EQ(RawExecutions, 16u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, RedundantUnknownRootsReleaseOnlyCoveredProofs) {
  unsigned RawExecutions = 0;
  for (unsigned Mode = 0; Mode != 3; ++Mode)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "mode=" << Mode << ", reverse=" << Reverse);
      std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t Slot = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return Slot;
      };
      const size_t Wrapper = Text.size();
      Text.push_back(encode(Opcode::MOV64_REG, 6, 1));
      // Snapshot the field before the opaque callee can overwrite its address.
      // The ABI preserves this register; it does not promise frame no-alias.
      Text.push_back(encode(Opcode::LD_DW_REG, 7, 6));
      std::optional<size_t> Choose, Skip, Alternate;
      if (Mode == 1) {
        Choose = Text.size();
        Text.push_back({});
        Alternate = Literal(4, 0);
        Skip = Text.size();
        Text.push_back({});
      }
      const size_t CommonLiteral = Literal(4, 0);
      const size_t CommonCall = Text.size();
      Text.push_back({});
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 7));
      if (Mode == 2)
        Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -8, 91));
      Text.push_back(encode(Opcode::EXIT));
      std::vector<size_t> Entries, Calls, Literals;
      for (unsigned I = 0; I < kCallXContextSpecializationsPerEntry; ++I) {
        Entries.push_back(Text.size());
        Literals.push_back(Literal(3, 0));
        const int16_t Offset = -16 * (I + 1);
        Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, Offset));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, Offset));
        Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, 1));
        Calls.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Common = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 4));
      Literal(3, kInputStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 3, 3));
      Text.push_back(encode(Opcode::ST_DW_IMM, 3));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t First = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Second = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      const auto SetLiteral = [&](size_t Slot, unsigned R, size_t Target) {
        const auto Words = test::loadImm64(R, kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
      };
      SetLiteral(CommonLiteral, 4, First);
      if (Alternate) {
        SetLiteral(*Alternate, 4, Second);
        Text[*Choose] =
            encode(Opcode::JEQ64_IMM, 2, 0, CommonLiteral - *Choose - 1);
        Text[*Skip] = encode(Opcode::JA, 0, 0, CommonCall - *Skip - 1);
      }
      Text[CommonCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Common - CommonCall - 1);
      for (size_t I = 0; I < Calls.size(); ++I) {
        Text[Calls[I]] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
        SetLiteral(Literals[I], 3, I + 1 == Calls.size() ? Second : First);
      }
      auto Image = test::makeImage(Text, {});
      auto Named = Entries;
      Named.push_back(Wrapper);
      for (size_t Entry : Named) {
        Symbol Function;
        Function.Name = "caller_" + std::to_string(Entry);
        Function.Addr = kBytecodeStart + Entry * kInstructionSize;
        Function.IsFunc = true;
        Image.Symbols.push_back(std::move(Function));
      }
      auto Program = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (Reverse)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      EXPECT_LE(llvm::count_if(Report->Summaries,
                               [&](const auto &S) {
                                 return S.EntrySlot == Wrapper &&
                                        !S.GeneralizedInputs;
                               }),
                kCallXContextSpecializationsPerEntry);
      EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
        return Site.CallerSlot == Calls.back() && Site.CallXSlot == CallX &&
               Site.TargetSlot == Second;
      }));
      const auto Last = llvm::find_if(Report->Summaries, [&](const auto &S) {
        return S.EntrySlot == Entries.back() && !S.GeneralizedInputs;
      });
      ASSERT_NE(Last, Report->Summaries.end());
      if (Mode == 0) {
        EXPECT_EQ(Last->ReturnValue.ValueKind, RegisterValue::Kind::Constant);
        EXPECT_EQ(Last->ReturnValue.Value, 43u);
      } else {
        EXPECT_EQ(Last->ReturnValue.ValueKind, RegisterValue::Kind::Unknown);
      }
      EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
        return Site.CallXSlot == Common &&
               (Site.TargetSlot == First ||
                llvm::any_of(Site.TargetValueAlternatives.values(),
                             [&](const auto &Value) {
                               return Value.ValueKind ==
                                          RegisterValue::Kind::Constant &&
                                      Value.Value == kBytecodeStart + First * 8;
                             }));
      }));
      EXPECT_EQ(llvm::any_of(Report->Summaries,
                             [&](const auto &S) {
                               return S.EntrySlot == Wrapper && S.IsRoot &&
                                      !S.GeneralizedInputs;
                             }),
                Mode != 0);
      for (size_t I = 0; I < Entries.size(); ++I) {
        Image.Entry = kBytecodeStart + Entries[I] * kInstructionSize;
        auto Selected = analyze(Image);
        ASSERT_TRUE(static_cast<bool>(Selected))
            << llvm::toString(Selected.takeError());
        ExecutionEnvironment Environment;
        std::vector<uint8_t> Input(8);
        llvm::support::endian::write64le(Input.data(), kHeapStart);
        Environment.Memory.push_back({kInputStart, Input, false, "pointer"});
        Environment.Memory.push_back(
            {kHeapStart, std::vector<uint8_t>(8), true, "output"});
        auto Raw = executeRaw(*Selected, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, I + 1 == Calls.size() ? 43u : 42u);
        EXPECT_TRUE(llvm::any_of(Raw->Trace, [&](const auto &Step) {
          return Step.Slot == (I + 1 == Calls.size() ? Second : First);
        }));
        ++RawExecutions;
      }
    }
  EXPECT_EQ(RawExecutions, 24u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, RootReclamationUsesOnlyIndependentPublishedProofs) {
  unsigned RawExecutions = 0;
  for (unsigned Mode = 0; Mode != 3; ++Mode)
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << "mode=" << Mode << ", reverse=" << Reverse);
      std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
      const auto Literal = [&](unsigned R, uint64_t Value) {
        const size_t At = Text.size();
        const auto Words = test::loadImm64(R, Value);
        Text.insert(Text.end(), Words.begin(), Words.end());
        return At;
      };
      std::optional<size_t> Witness, WitnessTarget, WitnessCall, WitnessOwn;
      if (Mode) {
        Witness = Text.size();
        Literal(1, kInputStart + 8);
        Text.push_back(encode(Opcode::LD_DW_REG, 1, 1));
        WitnessTarget = Literal(2, 0);
        WitnessCall = Text.size();
        Text.push_back({});
        WitnessOwn = Literal(4, 0);
        Text.push_back(encode(Opcode::CALL_REG, 4));
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Wrapper = Text.size();
      Text.push_back(encode(Opcode::LD_DW_REG, 7, 1));
      Text.push_back(encode(Opcode::MOV64_REG, 1, 2));
      const size_t ParentTarget = Literal(2, 0);
      const size_t ParentCall = Text.size();
      Text.push_back({});
      const size_t CallX = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 7));
      Text.push_back(encode(Opcode::EXIT));
      std::vector<size_t> Entries, Calls, Literals;
      for (unsigned I = 0; I != kCallXContextSpecializationsPerEntry; ++I) {
        Entries.push_back(Text.size());
        Literals.push_back(Literal(3, 0));
        const int16_t Offset = -16 * (I + 1);
        Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, Offset));
        Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
        Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, Offset));
        Text.push_back(encode(Opcode::MOV64_IMM, 2));
        Calls.push_back(Text.size());
        Text.push_back({});
        Text.push_back(encode(Opcode::EXIT));
      }
      const size_t Parent = Text.size();
      const size_t Skip = Text.size();
      Text.push_back({});
      Text.push_back(encode(Opcode::MOV64_REG, 4, 2));
      const size_t CommonCall = Text.size();
      Text.push_back({});
      const size_t ParentExit = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Common = Text.size();
      Text.push_back(encode(Opcode::CALL_REG, 4));
      Literal(3, kInputStart);
      Text.push_back(encode(Opcode::LD_DW_REG, 3, 3));
      Text.push_back(encode(Opcode::ST_DW_IMM, 3));
      Text.push_back(encode(Opcode::MOV64_IMM, 0));
      Text.push_back(encode(Opcode::EXIT));
      const size_t First = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
      Text.push_back(encode(Opcode::EXIT));
      const size_t Second = Text.size();
      Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
      Text.push_back(encode(Opcode::EXIT));
      const auto SetLiteral = [&](size_t At, unsigned R, size_t Target) {
        const auto Words = test::loadImm64(R, kBytecodeStart + Target * 8);
        std::copy(Words.begin(), Words.end(), Text.begin() + At);
      };
      SetLiteral(ParentTarget, 2, First);
      Text[ParentCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Parent - ParentCall - 1);
      Text[CommonCall] =
          encode(Opcode::CALL_IMM, 0, 1, 0, Common - CommonCall - 1);
      Text[Skip] = encode(Opcode::JEQ64_IMM, 1, 0, ParentExit - Skip - 1);
      if (Witness) {
        SetLiteral(*WitnessTarget, 2, Mode == 1 ? First : Second);
        SetLiteral(*WitnessOwn, 4, First);
        Text[*WitnessCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Parent - *WitnessCall - 1);
      }
      for (size_t I = 0; I != Calls.size(); ++I) {
        Text[Calls[I]] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
        SetLiteral(Literals[I], 3, I + 1 == Calls.size() ? Second : First);
      }
      auto Image = test::makeImage(Text, {});
      auto Named = Entries;
      Named.push_back(Wrapper);
      if (Witness)
        Named.push_back(*Witness);
      for (size_t Entry : Named) {
        Symbol Function;
        Function.Name = "caller_" + std::to_string(Entry);
        Function.Addr = kBytecodeStart + Entry * kInstructionSize;
        Function.IsFunc = true;
        Image.Symbols.push_back(std::move(Function));
      }
      auto Program = analyze(Image);
      ASSERT_TRUE(static_cast<bool>(Program))
          << llvm::toString(Program.takeError());
      if (Reverse)
        std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
      auto Report = recoverCallXContexts(*Program);
      ASSERT_TRUE(static_cast<bool>(Report))
          << llvm::toString(Report.takeError());
      checkSummaryReferences(*Report);
      EXPECT_FALSE(Report->SpecializationBudgetExhausted);
      EXPECT_LE(llvm::count_if(Report->Summaries,
                               [&](const auto &S) {
                                 return S.EntrySlot == Wrapper &&
                                        !S.GeneralizedInputs;
                               }),
                kCallXContextSpecializationsPerEntry);
      EXPECT_EQ(llvm::any_of(Report->Summaries,
                             [&](const auto &S) {
                               return S.EntrySlot == Wrapper && S.IsRoot &&
                                      !S.GeneralizedInputs;
                             }),
                Mode != 1);
      EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
        return Site.CallXSlot == Common && Site.TargetSlot == First;
      }));
      if (Mode == 1) {
        const auto Last = llvm::find_if(Report->Summaries, [&](const auto &S) {
          return S.EntrySlot == Entries.back() && !S.GeneralizedInputs;
        });
        ASSERT_NE(Last, Report->Summaries.end());
        EXPECT_EQ(Last->ReturnValue.ValueKind, RegisterValue::Kind::Constant);
        EXPECT_EQ(Last->ReturnValue.Value, 43u);
        EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &Site) {
          return Site.CallerSlot == Calls.back() && Site.CallXSlot == CallX &&
                 Site.TargetSlot == Second;
        }));
      }
      auto Executed = Entries;
      if (Witness)
        Executed.push_back(*Witness);
      for (size_t I = 0; I != Executed.size(); ++I) {
        Image.Entry = kBytecodeStart + Executed[I] * kInstructionSize;
        auto Selected = analyze(Image);
        ASSERT_TRUE(static_cast<bool>(Selected))
            << llvm::toString(Selected.takeError());
        ExecutionEnvironment Environment;
        std::vector<uint8_t> Input(16);
        llvm::support::endian::write64le(Input.data(), kHeapStart);
        llvm::support::endian::write64le(Input.data() + 8, 1);
        Environment.Memory.push_back({kInputStart, Input, false, "pointer"});
        Environment.Memory.push_back(
            {kHeapStart, std::vector<uint8_t>(8), true, "output"});
        auto Raw = executeRaw(*Selected, std::move(Environment));
        ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
        ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
        EXPECT_EQ(Raw->ReturnValue, I + 1 == Entries.size() ? 43u : 42u);
        if (I == Entries.size()) {
          const auto At = llvm::find_if(Raw->Trace, [&](const auto &Step) {
            return Step.Slot == Common;
          });
          ASSERT_NE(At, Raw->Trace.end());
          ASSERT_NE(std::next(At), Raw->Trace.end());
          EXPECT_EQ(std::next(At)->Slot, Mode == 1 ? First : Second);
        }
        ++RawExecutions;
      }
    }
  EXPECT_EQ(RawExecutions, 28u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, UnknownInputRootsCanRefineGenericScalarReturns) {
  unsigned RawExecutions = 0;
  for (bool BoundArgument : {false, true}) {
    SCOPED_TRACE(::testing::Message() << "bound=" << BoundArgument);
    std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
    const auto Literal = [&](unsigned Register, uint64_t Value) {
      const size_t Slot = Text.size();
      const auto Words = test::loadImm64(Register, Value);
      Text.insert(Text.end(), Words.begin(), Words.end());
      return Slot;
    };
    const size_t Wrapper = Text.size();
    std::optional<size_t> FixedTarget;
    if (BoundArgument)
      Text.push_back(encode(Opcode::MOV64_REG, 1, 2));
    else
      FixedTarget = Literal(1, 0);
    const size_t DispatchCall = Text.size();
    Text.push_back({});
    // The unknown write prevents a memory-preserving leaf shortcut, while
    // leaving the independently computed scalar return in r0 unchanged.
    Literal(3, kInputStart);
    Text.push_back(encode(Opcode::LD_DW_REG, 3, 3));
    Text.push_back(encode(Opcode::ST_DW_IMM, 3));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Caller = Text.size();
    const size_t CallerTarget = Literal(2, 0);
    const size_t WrapperCall = Text.size();
    Text.push_back({});
    Text.push_back(encode(Opcode::EXIT));
    const size_t Dispatch = Text.size();
    Text.push_back(encode(Opcode::CALL_REG, 1));
    Text.push_back(encode(Opcode::EXIT));
    const size_t Target = Text.size();
    Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
    Text.push_back(encode(Opcode::EXIT));
    Text[DispatchCall] =
        encode(Opcode::CALL_IMM, 0, 1, 0, Dispatch - DispatchCall - 1);
    Text[WrapperCall] =
        encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - WrapperCall - 1);
    const auto SetTarget = [&](size_t Slot, unsigned Register) {
      const auto Words = test::loadImm64(Register, kBytecodeStart + Target * 8);
      std::copy(Words.begin(), Words.end(), Text.begin() + Slot);
    };
    SetTarget(CallerTarget, 2);
    if (FixedTarget)
      SetTarget(*FixedTarget, 1);
    auto Image = test::makeImage(Text, {});
    for (size_t Entry : {Wrapper, Caller}) {
      Symbol Function;
      Function.Name = "caller_" + std::to_string(Entry);
      Function.Addr = kBytecodeStart + Entry * kInstructionSize;
      Function.IsFunc = true;
      Image.Symbols.push_back(std::move(Function));
    }
    auto Program = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Program))
        << llvm::toString(Program.takeError());
    auto Report = recoverCallXContexts(*Program);
    ASSERT_TRUE(static_cast<bool>(Report))
        << llvm::toString(Report.takeError());
    checkSummaryReferences(*Report);
    const auto Generic = llvm::find_if(Report->Summaries, [&](const auto &S) {
      return S.EntrySlot == Wrapper && S.GeneralizedInputs;
    });
    ASSERT_NE(Generic, Report->Summaries.end());
    EXPECT_EQ(Generic->ReturnValue.ValueKind,
              BoundArgument ? RegisterValue::Kind::Unknown
                            : RegisterValue::Kind::Constant);
    if (!BoundArgument) {
      EXPECT_EQ(Generic->ReturnValue.Value, 42u);
      EXPECT_FALSE(Generic->ReturnValueExpression);
    }
    const auto Concrete = llvm::find_if(Report->Summaries, [&](const auto &S) {
      return S.EntrySlot == Caller && !S.GeneralizedInputs;
    });
    ASSERT_NE(Concrete, Report->Summaries.end());
    EXPECT_EQ(Concrete->ReturnValue.ValueKind, RegisterValue::Kind::Constant);
    EXPECT_EQ(Concrete->ReturnValue.Value, 42u);
    Image.Entry = kBytecodeStart + Caller * kInstructionSize;
    auto Selected = analyze(Image);
    ASSERT_TRUE(static_cast<bool>(Selected))
        << llvm::toString(Selected.takeError());
    ExecutionEnvironment Environment;
    std::vector<uint8_t> Input(8);
    llvm::support::endian::write64le(Input.data(), kHeapStart);
    Environment.Memory.push_back({kInputStart, Input, false, "pointer"});
    Environment.Memory.push_back(
        {kHeapStart, std::vector<uint8_t>(8), true, "output"});
    auto Raw = executeRaw(*Selected, std::move(Environment));
    ASSERT_TRUE(static_cast<bool>(Raw)) << llvm::toString(Raw.takeError());
    ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
    EXPECT_EQ(Raw->ReturnValue, 42u);
    ++RawExecutions;
  }
  EXPECT_EQ(RawExecutions, 2u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

TEST(SBFCallXContexts, UnknownForwardingRootsCanTransferTheirProofGraph) {
  unsigned RawExecutions = 0;
  for (bool DirectProof : {false, true})
    for (bool OwnField : {false, true})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << "direct=" << DirectProof << ", field=" << OwnField
                     << ", reverse=" << Reverse);
        std::vector<test::EncodedInstruction> Text{encode(Opcode::EXIT)};
        const auto Literal = [&](unsigned R, uint64_t Value) {
          const size_t At = Text.size();
          const auto Words = test::loadImm64(R, Value);
          Text.insert(Text.end(), Words.begin(), Words.end());
          return At;
        };
        const size_t Wrapper = Text.size();
        Text.push_back(encode(Opcode::LD_DW_REG, 7, 1));
        Text.push_back(encode(Opcode::MOV64_REG, 8, 2));
        const size_t CommonTarget = Literal(4, 0);
        const size_t CommonCall = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::MOV64_REG, 0, 7));
        // Both values depend on the entry. Generic mode cannot replace this
        // binary operation with the single-input expression of either operand.
        Text.push_back(encode(Opcode::OR64_REG, 0, 8));
        if (OwnField)
          Text.push_back(encode(Opcode::ST_DW_IMM, 10, 0, -8, 91));
        Text.push_back(encode(Opcode::EXIT));
        std::vector<size_t> Entries, Calls, Literals, Indirect;
        for (unsigned I = 0; I != kCallXContextSpecializationsPerEntry; ++I) {
          Entries.push_back(Text.size());
          Literals.push_back(Literal(3, 0));
          const int16_t Offset = -16 * (I + 1);
          Text.push_back(encode(Opcode::ST_DW_REG, 10, 3, Offset));
          Text.push_back(encode(Opcode::MOV64_REG, 1, 10));
          Text.push_back(encode(Opcode::ADD64_IMM, 1, 0, 0, Offset));
          Text.push_back(encode(Opcode::MOV64_IMM, 2));
          Calls.push_back(Text.size());
          Text.push_back({});
          Indirect.push_back(Text.size());
          Text.push_back(encode(Opcode::CALL_REG, 0));
          Text.push_back(encode(Opcode::EXIT));
        }
        // A bridge keeps the root's immediate edges free of instantiated
        // expressions. The descendant keeps ownership of its exact proof.
        const size_t Bridge = Text.size();
        const size_t BridgeCall = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::EXIT));
        const size_t Common = Text.size();
        const size_t Skip = Text.size();
        Text.push_back({});
        const size_t CommonIndirect = Text.size();
        Text.push_back(encode(Opcode::CALL_REG, 4));
        const size_t Store = Literal(3, kInputStart);
        Text.push_back(encode(Opcode::LD_DW_REG, 3, 3));
        Text.push_back(encode(Opcode::ST_DW_REG, 3, 2));
        Text.push_back(encode(Opcode::MOV64_IMM, 0));
        Text.push_back(encode(Opcode::EXIT));
        const size_t First = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 42));
        Text.push_back(encode(Opcode::EXIT));
        const size_t Second = Text.size();
        Text.push_back(encode(Opcode::MOV64_IMM, 0, 0, 0, 43));
        Text.push_back(encode(Opcode::EXIT));
        const size_t RawProof = Text.size();
        Literal(1, kInputStart + 8);
        Text.push_back(encode(Opcode::MOV64_IMM, 2, 0, 0, 1));
        const size_t RawProofCall = Text.size();
        Text.push_back({});
        Text.push_back(encode(Opcode::EXIT));
        const auto SetLiteral = [&](size_t At, unsigned R, size_t Target) {
          const auto Words = test::loadImm64(R, kBytecodeStart + Target * 8);
          std::copy(Words.begin(), Words.end(), Text.begin() + At);
        };
        SetLiteral(CommonTarget, 4, First);
        const size_t Called = DirectProof ? Common : Bridge;
        Text[CommonCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Called - CommonCall - 1);
        Text[BridgeCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Common - BridgeCall - 1);
        Text[Skip] = encode(Opcode::JEQ64_IMM, 2, 0, Store - Skip - 1);
        Text[RawProofCall] =
            encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - RawProofCall - 1);
        for (size_t I = 0; I != Entries.size(); ++I) {
          Text[Calls[I]] =
              encode(Opcode::CALL_IMM, 0, 1, 0, Wrapper - Calls[I] - 1);
          SetLiteral(Literals[I], 3, I + 1 == Entries.size() ? Second : First);
        }
        auto Image = test::makeImage(Text, {});
        auto Named = Entries;
        Named.push_back(Wrapper);
        for (size_t Entry : Named) {
          Symbol Function;
          Function.Name = "caller_" + std::to_string(Entry);
          Function.Addr = kBytecodeStart + Entry * kInstructionSize;
          Function.IsFunc = true;
          Image.Symbols.push_back(std::move(Function));
        }
        auto Program = analyze(Image);
        ASSERT_TRUE(static_cast<bool>(Program))
            << llvm::toString(Program.takeError());
        if (Reverse)
          std::reverse(Program->Low.Edges.begin(), Program->Low.Edges.end());
        auto Report = recoverCallXContexts(*Program);
        ASSERT_TRUE(static_cast<bool>(Report))
            << llvm::toString(Report.takeError());
        checkSummaryReferences(*Report);
        EXPECT_FALSE(Report->SpecializationBudgetExhausted);
        EXPECT_LE(llvm::count_if(Report->Summaries,
                                 [&](const auto &S) {
                                   return S.EntrySlot == Wrapper &&
                                          !S.GeneralizedInputs;
                                 }),
                  kCallXContextSpecializationsPerEntry);
        const auto Generic =
            llvm::find_if(Report->Summaries, [&](const auto &S) {
              return S.EntrySlot == Wrapper && S.GeneralizedInputs;
            });
        ASSERT_NE(Generic, Report->Summaries.end());
        const bool Forwarded =
            llvm::any_of(Report->SummaryEdges, [&](const auto &C) {
              return C.CallerSummaryID == Generic->ID &&
                     C.CallerSlot == CommonCall &&
                     C.CalleeEntrySlot == Called && !C.GeneralizedInputs;
            });
        EXPECT_EQ(Forwarded, !OwnField && !DirectProof);
        EXPECT_TRUE(llvm::any_of(Report->Contexts, [&](const auto &C) {
          return C.CallXSlot == CommonIndirect && C.TargetSlot == First;
        }));
        for (size_t I = 0; I != Entries.size(); ++I) {
          const size_t Target = I + 1 == Entries.size() ? Second : First;
          const bool Proven =
              llvm::any_of(Report->Contexts, [&](const auto &C) {
                return C.CallXSlot == Indirect[I] && C.TargetSlot == Target;
              });
          EXPECT_EQ(Proven,
                    (!OwnField && !DirectProof) || I + 1 != Entries.size());
        }
        auto RawEntries = Entries;
        RawEntries.push_back(RawProof);
        for (size_t I = 0; I != RawEntries.size(); ++I) {
          Image.Entry = kBytecodeStart + RawEntries[I] * kInstructionSize;
          auto Selected = analyze(Image);
          ASSERT_TRUE(static_cast<bool>(Selected))
              << llvm::toString(Selected.takeError());
          ExecutionEnvironment Environment;
          std::vector<uint8_t> Input(16);
          llvm::support::endian::write64le(Input.data(), kHeapStart);
          llvm::support::endian::write64le(Input.data() + 8,
                                           kBytecodeStart + First * 8);
          Environment.Memory.push_back({kInputStart, Input, false, "pointer"});
          Environment.Memory.push_back(
              {kHeapStart, std::vector<uint8_t>(8), true, "output"});
          auto Raw = executeRaw(*Selected, std::move(Environment));
          ASSERT_TRUE(static_cast<bool>(Raw))
              << llvm::toString(Raw.takeError());
          ASSERT_EQ(Raw->Status, ExecutionStatus::Returned) << Raw->Error;
          EXPECT_EQ(Raw->ReturnValue, I == Entries.size()
                                          ? (kBytecodeStart + First * 8) | 1u
                                      : I + 1 == Entries.size() ? 43u
                                                                : 42u);
          ++RawExecutions;
        }
      }
  EXPECT_EQ(RawExecutions, 40u);
  RecordProperty("raw_executions", static_cast<int>(RawExecutions));
}

} // namespace neverd::sbf
