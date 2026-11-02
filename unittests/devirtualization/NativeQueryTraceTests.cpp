//===- NativeQueryTraceTests.cpp - Defined native query relations --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Independent native fixtures for retained scalar environment queries.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/BinaryInterpreterSpecialization.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/lift/X86/X86Regs.h"

#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

using namespace neverd;
using namespace neverd::analysis;

namespace {
constexpr va_t Entry = 0x1000;

struct QueryProgram {
  BinaryImage Image;
  SpecializationOptions Options;
  LowIRIndependenceContract Contract;

  explicit QueryProgram(std::initializer_list<uint8_t> Bytes) {
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
    Segment Code;
    Code.Name = ".text";
    Code.VA = Entry;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Data = Bytes;
    Code.Size = Code.FileSz = Code.Data.size();
    Image.Segments.push_back(std::move(Code));
    Options.ExplicitMachineState = true;
    Options.NormalNonfaultingExecution = true;
    Options.X64CetDisabled = true;
    Options.X64FlagsProfile = Contract.X64FlagsProfile =
        InterpreterMachineStateProfile::UserX64NoFaultV1;
    Options.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
    Contract.Frame = LowIRIndependenceFrame{{x86reg::RSP, 8}, -32, 8};
    Contract.ReturnRegisters = {{x86reg::RAX, 8}};
    Contract.NativePreservedState = LowIRNativePreservationRequirement{};
    Contract.ExternalQueries = LowIRExternalQueryModel::OrderedScalarV1;
  }

  void append(std::initializer_list<uint8_t> Bytes) {
    auto &Code = Image.Segments.front();
    Code.Data.insert(Code.Data.end(), Bytes);
    Code.Size = Code.FileSz = Code.Data.size();
  }

  SpecializationResult recover() const {
    return specializeBinaryInterpreter(Image, Entry, Options);
  }

  BinaryUndefinedIndependenceResult independence() const {
    return checkBinaryUndefinedIndependence(Image, Entry, Options, Contract);
  }

  BinaryLowIRRefinementResult
  compare(const LowFunc &Candidate,
          const LowIRRefinementLimits &Limits = {}) const {
    auto Selected = Contract;
    Selected.NativePreservedState->Quantifier =
        LowIRNativePreservationQuantifier::SelectedWitness;
    return checkBinaryLowIRRefinement(
        Image, Entry, Options, Candidate, Selected,
        LowIRRefinementWitness::LiftedBits, Limits);
  }
};

TEST(NativeQueryTrace, SharesFourDefinedResultsAndKeepsDefaultRefusal) {
  QueryProgram P({0x0f, 0xa2, 0xc3}); // CPUID; RET.
  for (auto R : {x86reg::RBX, x86reg::RCX, x86reg::RDX})
    P.Contract.ReturnRegisters.push_back({R, 8});
  const auto Raw = P.recover();
  ASSERT_TRUE(Raw.complete()) << Raw.Diagnostic;
  auto Default = P;
  Default.Contract.ExternalQueries = LowIRExternalQueryModel::None;
  EXPECT_FALSE(Default.independence().proved());
  EXPECT_FALSE(Default.compare(Raw.Residual).proved());
  const auto Independent = P.independence();
  ASSERT_TRUE(Independent.proved()) << Independent.Proof.Diagnostic;
  ASSERT_TRUE(Independent.Certificate->LowIR.NativePreservation);
  EXPECT_EQ(Independent.Certificate->LowIR.NativePreservation->Quantifier,
            LowIRNativePreservationQuantifier::AllUndefinedChoices);
  const auto &Queries = Independent.Certificate->LowIR.ExternalQueryTransitions;
  ASSERT_EQ(Queries.size(), 1U);
  EXPECT_EQ(Queries[0].InstructionAddress, Entry);
  EXPECT_EQ(Queries[0].Identity, "x64.cpuid");
  EXPECT_EQ(Queries[0].SemanticsVersion, 1U);
  EXPECT_EQ(Queries[0].OperationDigest.size(), 64U);
  EXPECT_EQ(Queries[0].DescriptorDigest.size(), 64U);
  const auto Relation = P.compare(Raw.Residual);
  ASSERT_TRUE(Relation.proved()) << Relation.Proof.Diagnostic;
  ASSERT_EQ(Relation.Proof.Certificate->ExternalQueryTransitions.size(), 1U);
  EXPECT_EQ(
      Relation.Proof.Certificate->ExternalQueryTransitions[0].DescriptorDigest,
      Queries[0].DescriptorDigest);
}

TEST(NativeQueryTrace, SelectorsAndOccurrencesRemainObservations) {
  // MOV EAX,7; MOV ECX,3; CPUID; XOR EAX,EAX; RET.
  QueryProgram P(
      {0xb8, 7, 0, 0, 0, 0xb9, 3, 0, 0, 0, 0x0f, 0xa2, 0x31, 0xc0, 0xc3});
  const auto Same = P.recover();
  ASSERT_TRUE(Same.complete()) << Same.Diagnostic;
  ASSERT_TRUE(P.compare(Same.Residual).proved());
  for (unsigned Offset : {1, 6}) {
    auto Changed = P;
    ++Changed.Image.Segments.front().Data[Offset];
    const auto Wrong = Changed.recover();
    ASSERT_TRUE(Wrong.complete()) << Wrong.Diagnostic;
    const auto Relation = P.compare(Wrong.Residual);
    EXPECT_EQ(Relation.Proof.Status, LowIRRefinementStatus::Different)
        << Relation.Proof.Diagnostic;
    EXPECT_FALSE(Relation.Certificate);
  }
  QueryProgram Dropped({0x31, 0xc0, 0xc3});
  const auto Missing = Dropped.recover();
  ASSERT_TRUE(Missing.complete()) << Missing.Diagnostic;
  EXPECT_EQ(P.compare(Missing.Residual).Proof.Status,
            LowIRRefinementStatus::Different);
}

TEST(NativeQueryTrace, EveryResultClearsItsUpperHalf) {
  QueryProgram Zero({0x0f, 0xa2, 0x31, 0xc0, 0xc3});
  const auto Candidate = Zero.recover();
  ASSERT_TRUE(Candidate.complete()) << Candidate.Diagnostic;
  for (uint8_t ModRM : {0xc0, 0xd8, 0xc8, 0xd0}) {
    QueryProgram P({0x0f, 0xa2, 0x48, 0x89, ModRM});
    P.append({0x48, 0xc1, 0xe8, 32, 0xc3}); // SHR RAX,32; RET.
    const auto Relation = P.compare(Candidate.Residual);
    EXPECT_TRUE(Relation.proved()) << Relation.Proof.Diagnostic;
  }
}

TEST(NativeQueryTrace, RepeatedAnswersRemainConservativelyIndependent) {
  QueryProgram P({0xb8, 7, 0, 0, 0, 0xb9, 3, 0, 0, 0, 0x0f, 0xa2, 0x49, 0x89,
                  0xc0}); // Save the first EAX result in R8.
  P.append({0xb8, 7, 0, 0, 0, 0xb9, 3, 0, 0, 0, 0x0f, 0xa2});
  auto Zero = P;
  P.append({0x4c, 0x31, 0xc0, 0xc3}); // XOR RAX,R8; RET.
  Zero.append({0x31, 0xc0, 0xc3});
  const auto Same = P.recover(), Changed = Zero.recover();
  ASSERT_TRUE(Same.complete()) << Same.Diagnostic;
  ASSERT_TRUE(Changed.complete()) << Changed.Diagnostic;
  ASSERT_TRUE(P.compare(Same.Residual).proved());
  EXPECT_EQ(P.compare(Changed.Residual).Proof.Status,
            LowIRRefinementStatus::Different);
}

TEST(NativeQueryTrace, UndefinedSelectorsRemainObservable) {
  // XOR leaves AF undefined; LAHF carries it into the CPUID leaf selector.
  QueryProgram P({0x31, 0xc0, 0x9f, 0x0f, 0xa2, 0xc3});
  const auto Result = P.independence();
  EXPECT_EQ(Result.Proof.Status, LowIRIndependenceStatus::Dependent)
      << Result.Proof.Diagnostic;
  EXPECT_FALSE(Result.Certificate);
}

TEST(NativeQueryTrace, QueriesRetainOrderRelativeToOrdinaryReads) {
  // CPUID; MOV R9,[R8]; CPUID; XOR EAX,EAX; RET.
  QueryProgram P({0x0f, 0xa2, 0x4d, 0x8b, 0x08, 0x0f, 0xa2, 0x31, 0xc0, 0xc3});
  QueryProgram Moved(
      {0x4d, 0x8b, 0x08, 0x0f, 0xa2, 0x0f, 0xa2, 0x31, 0xc0, 0xc3});
  P.Contract.ExternalReads = LowIRExternalReadModel::OrderedOrdinaryRAMV1;
  const auto Same = P.recover(), Changed = Moved.recover();
  ASSERT_TRUE(Same.complete()) << Same.Diagnostic;
  ASSERT_TRUE(Changed.complete()) << Changed.Diagnostic;
  ASSERT_TRUE(P.compare(Same.Residual).proved());
  EXPECT_EQ(P.compare(Changed.Residual).Proof.Status,
            LowIRRefinementStatus::Different);
}

TEST(NativeQueryTrace, PreservesUnaffectedRegistersAndFlags) {
  QueryProgram P({0x0f, 0xa2, 0xc3});
  for (auto R : {x86reg::RBP, x86reg::RSI, x86reg::RDI, x86reg::R8, x86reg::R9,
                 x86reg::R10, x86reg::R11, x86reg::R12, x86reg::R13,
                 x86reg::R14, x86reg::R15})
    P.Contract.PreservedRegisters.push_back({R, 8});
  for (auto R : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF,
                 x86reg::DF, x86reg::OF})
    P.Contract.PreservedRegisters.push_back({R, 1});
  const auto Result = P.independence();
  EXPECT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
}

TEST(NativeQueryTrace, QueryCountMayDependOnAnOrdinaryEntryInput) {
  // TEST EDI,EDI; JE skip; CPUID; skip: XOR EAX,EAX; RET.
  QueryProgram P({0x85, 0xff, 0x74, 2, 0x0f, 0xa2, 0x31, 0xc0, 0xc3});
  const auto Raw = P.recover();
  ASSERT_TRUE(Raw.complete()) << Raw.Diagnostic;
  const auto Result = P.compare(Raw.Residual);
  ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
  EXPECT_EQ(Result.Proof.OriginalPaths, 2U);
  EXPECT_EQ(Result.Proof.CandidatePaths, 2U);
  QueryProgram Always({0x0f, 0xa2, 0x31, 0xc0, 0xc3});
  const auto Changed = Always.recover();
  ASSERT_TRUE(Changed.complete()) << Changed.Diagnostic;
  EXPECT_EQ(P.compare(Changed.Residual).Proof.Status,
            LowIRRefinementStatus::Different);
}

TEST(NativeQueryTrace, ModelIsDigestBoundAndRejectsUnknownValues) {
  QueryProgram P({0xb8, 3, 0, 0, 0, 0xc3});
  const auto Enabled = P.independence();
  ASSERT_TRUE(Enabled.proved()) << Enabled.Proof.Diagnostic;
  EXPECT_TRUE(Enabled.Certificate->LowIR.ExternalQueryTransitions.empty());
  P.Contract.ExternalQueries = LowIRExternalQueryModel::None;
  const auto Default = P.independence();
  ASSERT_TRUE(Default.proved()) << Default.Proof.Diagnostic;
  EXPECT_NE(Enabled.Certificate->InputDigest, Default.Certificate->InputDigest);
  EXPECT_NE(Enabled.Certificate->LowIR.InputDigest,
            Default.Certificate->LowIR.InputDigest);
  P.Contract.ExternalQueries = static_cast<LowIRExternalQueryModel>(255);
  const auto Unknown = P.independence();
  EXPECT_EQ(Unknown.Proof.Status, LowIRIndependenceStatus::Invalid);
  EXPECT_FALSE(Unknown.Certificate);
}

TEST(NativeQueryTrace, RejectsStaticAndInductiveProofSurfaces) {
  QueryProgram P({0x0f, 0xa2, 0xc3});
  const auto Raw = P.recover();
  ASSERT_TRUE(Raw.complete()) << Raw.Diagnostic;
  LowIRIndependenceContract Static;
  Static.ExternalQueries = LowIRExternalQueryModel::OrderedScalarV1;
  const auto NoProvider =
      checkLowIRUndefinedIndependence(Raw.Residual, {}, Static);
  EXPECT_EQ(NoProvider.Status, LowIRIndependenceStatus::Unsupported);
  EXPECT_FALSE(NoProvider.Certificate);
  auto Selected = P.Contract;
  Selected.NativePreservedState->Quantifier =
      LowIRNativePreservationQuantifier::SelectedWitness;
  const auto Loop = checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                                   Raw.Residual, Selected, {});
  EXPECT_EQ(Loop.Proof.Status, LowIRRefinementStatus::Unsupported)
      << Loop.Proof.Diagnostic;
  EXPECT_FALSE(Loop.Certificate);
  const auto Inference = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Raw, Selected);
  EXPECT_FALSE(Inference.proved());
  EXPECT_EQ(Inference.Inference.Status, LowIRLoopInferenceStatus::Unsupported)
      << Inference.Inference.Diagnostic;
}

TEST(NativeQueryTrace, ObservationAndNodeBudgetsRemainMandatory) {
  QueryProgram P({0x0f, 0xa2, 0xc3});
  const auto Good = P.independence();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  LowIRIndependenceLimits Limits;
  Limits.MaxObservations = Good.Proof.Observations;
  const auto Exact = checkBinaryUndefinedIndependence(P.Image, Entry, P.Options,
                                                      P.Contract, Limits);
  ASSERT_TRUE(Exact.proved()) << Exact.Proof.Diagnostic;
  --Limits.MaxObservations;
  const auto Exhausted = checkBinaryUndefinedIndependence(
      P.Image, Entry, P.Options, P.Contract, Limits);
  EXPECT_EQ(Exhausted.Proof.Status, LowIRIndependenceStatus::BudgetExceeded);
  EXPECT_FALSE(Exhausted.Certificate);
  Limits = {};
  Limits.MaxSymbolicNodes = 1;
  const auto Nodes = checkBinaryUndefinedIndependence(P.Image, Entry, P.Options,
                                                      P.Contract, Limits);
  EXPECT_EQ(Nodes.Proof.Status, LowIRIndependenceStatus::BudgetExceeded);
  EXPECT_FALSE(Nodes.Certificate);
}

TEST(NativeQueryTrace, OtherNativeQueriesAndMalformedBundlesStayUnsupported) {
  for (uint8_t Opcode : {0x31, 0x32}) {
    QueryProgram P({0x0f, Opcode, 0xc3}); // RDTSC and RDMSR.
    P.Options.X64FlagsProfile = P.Contract.X64FlagsProfile =
        InterpreterMachineStateProfile::KernelX64NoFaultV1;
    EXPECT_FALSE(P.independence().proved());
  }
  QueryProgram P({0x0f, 0xa2, 0xc3});
  const auto Raw = P.recover();
  ASSERT_TRUE(Raw.complete()) << Raw.Diagnostic;
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    auto Candidate = Raw.Residual;
    bool Changed = false;
    for (auto &B : Candidate.Blocks)
      for (const auto &IB : B.InstructionBoundaries) {
        auto *Ops = B.Ops.data() + IB.FirstOp;
        if (IB.OpCount != 9 || Ops[0].Opcode != NdOp::INTRINSIC ||
            Ops[0].Inputs[0].Offset != uint64_t(Intrinsic::Cpuid))
          continue;
        if (Mutation == 0)
          Ops[0].Inputs[1].Size = 8;
        else if (Mutation == 1)
          Ops[0].Output = NdVar::reg(x86reg::RBX, 8);
        else if (Mutation == 2)
          Ops[2].Inputs[0] = Ops[1].Inputs[0];
        else
          Ops[5].Opcode = NdOp::COPY;
        Changed = true;
      }
    ASSERT_TRUE(Changed);
    const auto Result = P.compare(Candidate);
    EXPECT_FALSE(Result.proved()) << Mutation;
    EXPECT_FALSE(Result.Certificate);
  }
}

TEST(NativeQueryTrace, OnlyStrictBareNativeEncodingReceivesBothAudits) {
  const std::vector<std::vector<uint8_t>> Forms{
      {0x0f, 0xa2},       {0x66, 0x0f, 0xa2}, {0x67, 0x0f, 0xa2},
      {0x48, 0x0f, 0xa2}, {0xf3, 0x0f, 0xa2}, {0xf0, 0x0f, 0xa2}};
  for (auto Architecture : {Arch::X64, Arch::X86}) {
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Architecture));
    Dec.setStrict(true);
    for (const auto &Bytes : Forms) {
      DecodedInsn Insn{};
      const auto Size =
          Dec.decodeOneForLift(Bytes.data(), Bytes.size(), Entry, Insn);
      const bool Allowed = Architecture == Arch::X64 && Bytes.size() == 2;
      if (Size <= 0) {
        EXPECT_FALSE(Allowed);
        continue;
      }
      SCOPED_TRACE(::testing::Message()
                   << unsigned(Architecture) << '/' << unsigned(Bytes[0]) << '/'
                   << Insn.Raw->mnemonic);
      // In 32-bit mode 48 is DEC EAX, not a REX prefix on CPUID.
      if (Architecture == Arch::X86 && Bytes[0] == 0x48) {
        EXPECT_EQ(Size, 1);
        EXPECT_EQ(Insn.Raw->id, X86_INS_DEC);
        continue;
      }
      ASSERT_EQ(Size, int(Bytes.size()));
      ASSERT_EQ(Insn.Raw->id, X86_INS_CPUID);
      std::vector<LowOp> Ops;
      LowInstructionUndefinedEffects Effects;
      LowInstructionPreservedState Preserved;
      try {
        Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
      } catch (const UnliftedInstruction &) {
        EXPECT_FALSE(Allowed);
      }
      EXPECT_EQ(Effects.Coverage == LowUndefinedCoverage::Complete, Allowed);
      EXPECT_EQ(Preserved.Audit == LowPreservedStateAudit::LegacyIntegerV1,
                Allowed);
      if (Allowed) {
        EXPECT_TRUE(Effects.Effects.empty());
        EXPECT_EQ(Effects.OperationDigest, lowUndefinedOperationDigest(Ops));
        EXPECT_EQ(Preserved.StateSet,
                  LowPreservedStateSet::LegacyIntegerOpaqueV1);
      }
    }
  }
}

TEST(NativeQueryTrace, InconsistentDecoderDetailsCannotAuthorizeEitherAudit) {
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X64));
  Dec.setStrict(true);
  const uint8_t Bytes[]{0x0f, 0xa2};
  using Mutation = void (*)(cs_insn &);
  const Mutation Changes[]{[](auto &I) { I.bytes[1] = 0x31; },
                           [](auto &I) { I.bytes[I.size++] = 0x90; },
                           [](auto &I) { I.detail->x86.opcode[1] = 0x31; },
                           [](auto &I) { I.detail->x86.op_count = 1; },
                           [](auto &I) { I.detail->x86.addr_size = 4; },
                           [](auto &I) { I.detail->x86.prefix[0] = 0xf3; },
                           [](auto &I) { I.detail->x86.rex = 0x48; },
                           [](auto &I) { I.detail->x86.encoding.imm_size = 1; },
                           [](auto &I) { I.detail->x86.modrm = 0xc0; },
                           [](auto &I) { I.detail->x86.eflags = 1; }};
  for (auto Change : Changes) {
    DecodedInsn Insn{};
    ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), Entry, Insn), 2);
    Change(*Insn.Raw);
    std::vector<LowOp> Ops;
    LowInstructionUndefinedEffects Effects;
    Effects.Coverage = LowUndefinedCoverage::Complete;
    LowInstructionPreservedState Preserved;
    Preserved.Audit = LowPreservedStateAudit::LegacyIntegerV1;
    try {
      Dec.liftToLow(Insn, Ops, {}, {}, &Effects, &Preserved);
    } catch (const UnliftedInstruction &) {
    }
    EXPECT_NE(Effects.Coverage, LowUndefinedCoverage::Complete);
    EXPECT_EQ(Preserved, LowInstructionPreservedState{});
  }
}
} // namespace
