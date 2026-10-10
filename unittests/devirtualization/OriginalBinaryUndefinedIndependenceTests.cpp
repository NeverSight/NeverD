//===- OriginalBinaryUndefinedIndependenceTests.cpp - Original graph proof ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/analysis/BinaryInterpreterSpecialization.h"
#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <vector>

using namespace neverd;
using namespace neverd::analysis;

namespace {
using Status = LowIRIndependenceStatus;
constexpr va_t Entry = 0x1000;

struct Program {
  BinaryImage Image;
  SpecializationOptions Options;
  LowIRIndependenceContract Contract;

  explicit Program(std::initializer_list<uint8_t> Bytes) {
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
    Options.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
    Contract.Frame = LowIRIndependenceFrame{{x86reg::RSP, 8}, -32, 8};
    Contract.ReturnRegisters = {{x86reg::RAX, 8}};
  }

  BinaryUndefinedIndependenceResult
  check(const LowIRIndependenceLimits &Limits = {}) const {
    return checkBinaryUndefinedIndependence(Image, Entry, Options, Contract,
                                            Limits);
  }

  SpecializationWithIndependenceResult
  recover(const LowIRIndependenceLimits &Limits = {}) const {
    return specializeBinaryInterpreterWithIndependence(Image, Entry, Options,
                                                       Contract, Limits);
  }

  void flagsProfile() {
    Options.X64FlagsProfile = Contract.X64FlagsProfile =
        InterpreterMachineStateProfile::UserX64NoFaultV1;
  }

  void append(std::initializer_list<uint8_t> Bytes) {
    auto &Code = Image.Segments.front();
    Code.Data.insert(Code.Data.end(), Bytes);
    Code.Size = Code.FileSz = Code.Data.size();
  }

  void immediate(uint64_t Value, unsigned Bytes = 4) {
    for (unsigned I = 0; I != Bytes; ++I)
      append({static_cast<uint8_t>(Value >> (I * 8))});
  }

  void requireEAX(uint32_t Value) {
    append({0x3d}); // cmp eax, value; jne trap; mov eax, 0; ret; trap: int3.
    immediate(Value);
    append({0x75, 6, 0xb8, 0, 0, 0, 0, 0xc3, 0xcc});
  }
};

void expectRefusal(const Program &P, Status Expected,
                   const LowIRIndependenceLimits &Limits = {}) {
  const auto Checked = P.check(Limits);
  EXPECT_EQ(Checked.Proof.Status, Expected) << Checked.Proof.Diagnostic;
  EXPECT_FALSE(Checked.proved());
  EXPECT_FALSE(Checked.Certificate.has_value());
  const auto Gated = P.recover(Limits);
  EXPECT_EQ(Gated.Independence.Proof.Status, Expected)
      << Gated.Independence.Proof.Diagnostic;
  EXPECT_FALSE(Gated.Independence.Certificate.has_value());
  EXPECT_FALSE(Gated.Recovery.complete());
  EXPECT_TRUE(Gated.Recovery.Residual.Blocks.empty());
}

Program skippedContinuation(bool Indirect) {
  // CALL helper; invalid long-mode bytes; MOV EAX,7; RET.
  // helper: ADD qword [RSP],2; RET. An indirect variant computes its target
  // with LEA before CALL RDX. Neither physical return reaches the inline data.
  if (Indirect)
    return Program({0x48, 0x8d, 0x15, 10,   0,    0, 0,   0xff,
                    0xd2, 0x16, 0x06, 0xb8, 7,    0, 0,   0,
                    0xc3, 0x48, 0x83, 0x04, 0x24, 2, 0xc3});
  return Program({0xe8, 8, 0, 0, 0, 0x16, 0x06, 0xb8, 7, 0, 0, 0, 0xc3, 0x48,
                  0x83, 0x04, 0x24, 2, 0xc3});
}

TEST(OriginalBinaryUndefinedIndependence,
     PhysicalReturnsCollectOnlyTheirActualDestinations) {
  for (bool Indirect : {false, true}) {
    auto P = skippedContinuation(Indirect);
    const auto R = P.recover();
    ASSERT_TRUE(R.Independence.proved()) << R.Independence.Proof.Diagnostic;
    EXPECT_TRUE(R.Recovery.complete()) << R.Recovery.Diagnostic;
    const auto &Insns = R.Independence.Certificate->Instructions;
    EXPECT_EQ(Insns.size(), Indirect ? 6U : 5U);
    const auto Continuation = Entry + (Indirect ? 9 : 5);
    EXPECT_FALSE(std::any_of(Insns.begin(), Insns.end(), [&](const auto &I) {
      return I.Origin.Address >= Continuation &&
             I.Origin.Address < Continuation + 2;
    }));
    EXPECT_EQ(std::count_if(Insns.begin(), Insns.end(),
                            [](const auto &I) {
                              return I.NativeStackControl ==
                                     SpecializationNativeStackControl::Return;
                            }),
              2);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     FeasibleCallContinuationsStillRequireCompleteInstructions) {
  for (bool Indirect : {false, true}) {
    auto P = skippedContinuation(Indirect);
    auto &Bytes = P.Image.Segments.front().Data;
    Bytes[Bytes.size() - 2] = 0; // Return to the original inline bytes.
    expectRefusal(P, Status::Unsupported);
    EXPECT_EQ(P.check().Proof.InstructionAddress, Entry + (Indirect ? 9 : 5));
  }
  // One feasible helper arm skips the data; ECX == 0 returns into it.
  Program Mixed({0xe8, 8,    0,    0,    0, 0x16, 0x06, 0xb8, 7,    0, 0,   0,
                 0xc3, 0x85, 0xc9, 0x74, 5, 0x48, 0x83, 0x04, 0x24, 2, 0xc3});
  expectRefusal(Mixed, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     DeferredCallContinuationsKeepCompleteTargetEnumeration) {
  // helper: AND ECX,1; SHL ECX,1; ADD [RSP],RCX; RET. The real return set
  // contains both the valid continuation and the invalid original address.
  Program P({0xe8, 8,    0,    0, 0,    0x16, 0x06, 0xb8, 7,    0,    0,   0,
             0xc3, 0x83, 0xe1, 1, 0xd1, 0xe1, 0x48, 0x01, 0x0c, 0x24, 0xc3});
  expectRefusal(P, Status::Unsupported);
  LowIRIndependenceLimits Limits;
  Limits.MaxIndirectTargets = 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     DeferredCallContinuationsNeverCertifyBudgetedPrefixes) {
  auto P = skippedContinuation(false);
  P.Contract.PreservedRegisters = {{x86reg::RSP, 8}};
  P.Contract.PreservedFrameRanges = {{0, 8}};
  const auto Good = P.check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  LowIRIndependenceLimits Limits;
  Limits.MaxInstructions = Good.Proof.Instructions;
  ASSERT_TRUE(P.check(Limits).proved());
  --Limits.MaxInstructions;
  expectRefusal(P, Status::BudgetExceeded, Limits);
  Limits = {};
  Limits.MaxBlockVisits = Good.Proof.BlockVisits - 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
  Limits = {};
  Limits.MaxSolverQueries = Good.Proof.SolverQueries - 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
  Program Cycle({0xe8, 1, 0, 0, 0, 0x16, 0xeb, 0xfe});
  Limits = {};
  Limits.MaxPaths = 12;
  expectRefusal(Cycle, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     DeferredConditionalEdgesSkipOnlyInfeasibleInstructionInventories) {
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           // Dead taken edge outside the image.
           {0x39, 0xc0, 0x75, 0x40, 0xb8, 7, 0, 0, 0, 0xc3},
           // Dead taken edge to bytes that cannot decode in long mode.
           {0x39, 0xc0, 0x75, 6, 0xb8, 7, 0, 0, 0, 0xc3, 0x16},
           // Dead fallthrough; only the taken edge is executable code.
           {0x39, 0xc0, 0x74, 2, 0x16, 0x06, 0xb8, 7, 0, 0, 0, 0xc3}}) {
    Program P({});
    P.Image.Segments.front().Data = Bytes;
    P.append({});
    expectRefusal(P, Status::Unsupported);
    P.Contract.DeferNativeConditionalEdges = true;
    const auto Good = P.check();
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_EQ(Good.Certificate->Instructions.size(), 4U);
    EXPECT_TRUE(Good.Certificate->LowIR.Contract.DeferNativeConditionalEdges);
    EXPECT_TRUE(Good.Certificate->LowIR.NativeAuditBoundaries.empty());
    const auto Recovered = P.recover();
    EXPECT_TRUE(Recovered.Independence.proved());
    EXPECT_TRUE(Recovered.Recovery.complete()) << Recovered.Recovery.Diagnostic;
    // Reverse the branch: exactly the same missing/invalid bytes are reached.
    P.Image.Segments.front().Data[2] ^= 1;
    expectRefusal(P, Status::Unsupported);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     DeferredConditionalEdgesRequireSymbolicPathAndArbitraryControlProofs) {
  // ECX > 7 returns. On the other path ECX > 9 cannot reach the bad byte.
  Program P({0x83, 0xf9, 7, 0x77, 5, 0x83, 0xf9, 9, 0x77, 6, 0xb8, 7, 0, 0, 0,
             0xc3, 0x16});
  P.Contract.DeferNativeConditionalEdges = true;
  const auto Good = P.check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.Paths, 2U);
  P.Image.Segments.front().Data[7] = 3;
  expectRefusal(P, Status::Unsupported);
  // An architecture-arbitrary OF must not choose which arm is collected.
  Program Arbitrary({0x0f, 0xa3, 0xc8, 0x70, 6, 0xb8, 7, 0, 0, 0, 0xc3, 0x16});
  Arbitrary.flagsProfile();
  Arbitrary.Contract.DeferNativeConditionalEdges = true;
  expectRefusal(Arbitrary, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     DeferredConditionalEdgesKeepExactBudgetsAndAllFeasibleArms) {
  Program P({0x83, 0xf9, 7, 0x77, 5, 0x83, 0xf9, 9, 0x77, 6, 0xb8, 7, 0, 0, 0,
             0xc3, 0x16});
  P.Contract.DeferNativeConditionalEdges = true;
  const auto Good = P.check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    SCOPED_TRACE(Kind);
    LowIRIndependenceLimits Limits;
    switch (Kind) {
    case 0:
      Limits.MaxInstructions = Good.Proof.Instructions;
      break;
    case 1:
      Limits.MaxOperations = Good.Proof.Operations;
      break;
    case 2:
      Limits.MaxBlockVisits = Good.Proof.BlockVisits;
      break;
    case 3:
      Limits.MaxSolverQueries = Good.Proof.SolverQueries;
      break;
    }
    ASSERT_TRUE(P.check(Limits).proved());
    switch (Kind) {
    case 0:
      --Limits.MaxInstructions;
      break;
    case 1:
      --Limits.MaxOperations;
      break;
    case 2:
      --Limits.MaxBlockVisits;
      break;
    case 3:
      --Limits.MaxSolverQueries;
      break;
    }
    expectRefusal(P, Status::BudgetExceeded, Limits);
  }
  Program Cycle({0x39, 0xc0, 0x74, 0xfc});
  Cycle.Contract.DeferNativeConditionalEdges = true;
  LowIRIndependenceLimits Limits;
  Limits.MaxPaths = 12;
  expectRefusal(Cycle, Status::BudgetExceeded, Limits);
  // Either side may be selected by an ordinary input. No successful return
  // may hide the other side's trap, missing mapping or invalid instruction.
  for (const auto &Bad :
       std::vector<std::vector<uint8_t>>{{0xcc}, {0x16}, {0xe9, 0, 1, 0, 0}}) {
    Program Mixed({0x85, 0xc9, 0x74, 6, 0xb8, 7, 0, 0, 0, 0xc3});
    auto &Data = Mixed.Image.Segments.front().Data;
    Data.insert(Data.end(), Bad.begin(), Bad.end());
    Mixed.append({});
    Mixed.Contract.DeferNativeConditionalEdges = true;
    expectRefusal(Mixed, Bad.front() == 0xcc ? Status::ContractViolation
                                             : Status::Unsupported);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     SplitAlignedPointerPreservesFrameAccessAndStoredInput) {
  // LEA RDX,[RSP-65]; AND DL,0xf0; LEA RDI,[RDX+9];
  // MOV [RDI],ECX; MOV EAX,[RDI]; RET. At residue 8 the access is RSP-63.
  Program P({0x48, 0x8d, 0x54, 0x24, 0xbf, 0x80, 0xe2, 0xf0, 0x48, 0x8d, 0x7a,
             9, 0x89, 0x0f, 0x8b, 0x07, 0xc3});
  P.Contract.Frame->Begin = -128;
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {16, 8};
  const auto Good = P.check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  // The exact offset is still subject to the declared frame boundary.
  P.Contract.Frame->Begin = -62;
  expectRefusal(P, Status::Unsupported);
  P.Contract.Frame->Begin = -128;
  P.Options.EntryFrameAlignment.reset();
  P.Contract.Frame->EntryAlignment.reset();
  expectRefusal(P, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     StraightLineLengthDoesNotRepeatTheSameFeasibilityProof) {
  Program Short({0xb8, 7, 0, 0, 0, 0xc3}); // MOV EAX,7; RET.
  const auto Reference = Short.check();
  ASSERT_TRUE(Reference.proved()) << Reference.Proof.Diagnostic;
  Program Long({0xb8, 7, 0, 0, 0});
  for (unsigned I = 0; I != 128; ++I)
    Long.append({0x90});
  Long.append({0xc3});
  LowIRIndependenceLimits Limits;
  Limits.MaxSolverQueries = Reference.Proof.SolverQueries;
  const auto Good = Long.check(Limits);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.Instructions, 130U);
  EXPECT_EQ(Good.Certificate->Instructions.size(), 130U);
  EXPECT_EQ(Good.Proof.SolverQueries, Reference.Proof.SolverQueries);
  ASSERT_GT(Limits.MaxSolverQueries, 0U);
  --Limits.MaxSolverQueries;
  expectRefusal(Long, Status::BudgetExceeded, Limits);
  Limits = {};
  Limits.MaxInstructions = 129;
  expectRefusal(Long, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     LiteralTransfersReuseIncomingFeasibility) {
  Program Short({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Reference = Short.check();
  ASSERT_TRUE(Reference.proved()) << Reference.Proof.Diagnostic;
  Program Long({});
  for (unsigned I = 0; I != 128; ++I)
    Long.append({0xe9, 0, 0, 0, 0}); // JMP next instruction.
  Long.append({0xb8, 7, 0, 0, 0, 0xc3});
  LowIRIndependenceLimits Limits;
  Limits.MaxSolverQueries = Reference.Proof.SolverQueries;
  const auto Good = Long.check(Limits);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.Paths, 1U);
  EXPECT_EQ(Good.Proof.Instructions, 130U);
  EXPECT_EQ(Good.Certificate->Instructions.size(), 130U);
  ASSERT_GT(Good.Proof.SolverQueries, 0U);
  Limits.MaxSolverQueries = Good.Proof.SolverQueries - 1;
  expectRefusal(Long, Status::BudgetExceeded, Limits);
  Limits = {};
  Limits.MaxInstructions = 129;
  expectRefusal(Long, Status::BudgetExceeded, Limits);
  Limits = {};
  Limits.MaxIndirectTargets = 0;
  expectRefusal(Long, Status::Invalid, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     CompleteSingletonTargetsKeepTheIncomingDomain) {
  constexpr unsigned Transfers = 32;
  const auto Make = [](unsigned Count) {
    Program P({});
    P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {16, 8};
    for (unsigned I = 0; I != Count; ++I) {
      const auto Next = Entry + P.Image.Segments.front().Data.size() + 19;
      // LEA RAX,[RSP+bias]; AND EAX,15; ADD RAX,next-residue;
      // JMP RAX (three unreachable padding bytes).
      const uint8_t Bias = I + 1;
      P.append({0x48, 0x8d, 0x44, 0x24, Bias, 0x83, 0xe0, 15, 0x48, 0x05});
      P.immediate(Next - ((8 + Bias) & 15));
      P.append({0xff, 0xe0, 0x90, 0x90, 0x90});
    }
    P.append({0xb8, 7, 0, 0, 0, 0xc3});
    return P;
  };
  const auto Reference = Make(0).check();
  ASSERT_TRUE(Reference.proved()) << Reference.Proof.Diagnostic;
  const auto Indirect = Make(Transfers);
  LowIRIndependenceLimits Limits;
  Limits.MaxIndirectTargets = 1;
  // Each symbolic transfer needs one model and one completed exclusion query.
  Limits.MaxSolverQueries = Reference.Proof.SolverQueries + 2 * Transfers;
  const auto Good = Indirect.check(Limits);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.Paths, 1U);
  EXPECT_EQ(Good.Proof.Instructions,
            Reference.Proof.Instructions + 4 * Transfers);
  EXPECT_EQ(Good.Certificate->Instructions.size(), 130U);
  ASSERT_GT(Good.Proof.SolverQueries, 0U);
  Limits.MaxSolverQueries = Good.Proof.SolverQueries - 1;
  expectRefusal(Indirect, Status::BudgetExceeded, Limits);
  auto Unconstrained = Indirect;
  Unconstrained.Options.EntryFrameAlignment.reset();
  Unconstrained.Contract.Frame->EntryAlignment.reset();
  Limits = {};
  Limits.MaxIndirectTargets = 1;
  expectRefusal(Unconstrained, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     RepeatedFeasibilityDoesNotMergeBranchesOrEntryDomains) {
  // TEST EDI,EDI; JE trap; MOV EAX,7; RET; trap: UD2.
  Program P({0x85, 0xff, 0x74, 6, 0xb8, 7, 0, 0, 0, 0xc3, 0x0f, 0x0b});
  P.Contract.DeferNativeConditionalEdges = true;
  const auto Input = NdVar::reg(x86reg::RDI, 8);
  for (uint64_t Value : {1, 0, 2}) {
    P.Options.EntryConstants = {{Input, Value}};
    P.Contract.EntryConstants = {{Input, Value}};
    if (Value) {
      const auto R = P.check();
      ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
    } else {
      expectRefusal(P, Status::ContractViolation);
    }
  }
  P.Options.EntryConstants.clear();
  P.Contract.EntryConstants.clear();
  expectRefusal(P, Status::ContractViolation);
  LowIRIndependenceLimits Limits;
  Limits.Solver.Blast.MaxGates = 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     RepeatedAlignedAccessesReuseTheCompleteFrameOffsetProof) {
  const auto Make = [](unsigned Loads) {
    Program P({0x48, 0x8d, 0x54, 0x24, 0xbf, 0x80, 0xe2, 0xf0, 0x48, 0x8d, 0x7a,
               9, 0x89, 0x0f});
    P.Contract.Frame->Begin = -128;
    P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {16, 8};
    for (unsigned I = 0; I != Loads; ++I)
      P.append({0x8b, 0x07}); // MOV EAX,[RDI].
    P.append({0xc3});
    return P;
  };
  const auto Short = Make(1).check();
  ASSERT_TRUE(Short.proved()) << Short.Proof.Diagnostic;
  const auto Long = Make(64);
  LowIRIndependenceLimits Limits;
  Limits.MaxSolverQueries = Short.Proof.SolverQueries;
  const auto Good = Long.check(Limits);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.Instructions, 69U);
  EXPECT_EQ(Good.Certificate->Instructions.size(), 69U);
  EXPECT_EQ(Good.Proof.SolverQueries, Short.Proof.SolverQueries);
  ASSERT_GT(Limits.MaxSolverQueries, 0U);
  --Limits.MaxSolverQueries;
  expectRefusal(Long, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     ReusedFrameProofDoesNotHideAChangedOutOfFrameAddress) {
  // The first aligned store is RSP-63; adding 128 puts the second outside
  // the frame under the same path predicate.
  Program P({0x48, 0x8d, 0x54, 0x24, 0xbf, 0x80, 0xe2, 0xf0,
             0x48, 0x8d, 0x7a, 9,    0x89, 0x0f, 0x48, 0x81,
             0xc7, 0x80, 0,    0,    0,    0x89, 0x0f, 0xc3});
  P.Contract.Frame->Begin = -128;
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {16, 8};
  expectRefusal(P, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     ReusedFrameProofKeepsDistinctIncomingPredicates) {
  // Form one aligned pointer, then branch on the entry stack's low bits.
  // The residue-8 fallthrough stores and returns before the pending taken
  // path, whose same address still has several possible frame offsets.
  Program P({0x48, 0x8d, 0x54, 0x24, 0xbf, 0x80, 0xe2, 0xf0,
             0x48, 0x8d, 0x7a, 9,    0x48, 0x89, 0xe0, 0x83,
             0xe0, 15,   0x83, 0xf8, 8,    0x75, 32});
  for (unsigned I = 0; I != 32; ++I)
    P.append({0x90});
  P.append({0x89, 0x0f, 0xc3});
  P.Contract.Frame->Begin = -128;
  const auto R = P.check();
  EXPECT_EQ(R.Proof.Status, Status::Unsupported) << R.Proof.Diagnostic;
  EXPECT_EQ(R.Proof.Paths, 1U);
  EXPECT_FALSE(R.proved());
  EXPECT_FALSE(R.Certificate);
}

TEST(OriginalBinaryUndefinedIndependence,
     StraightLineCertificateOwnsExactBytesAndFallbackMetadata) {
  // mov eax,7; ret. The optional memory-call route declines both instructions.
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Result = P.recover();
  ASSERT_TRUE(Result.Independence.proved())
      << Result.Independence.Proof.Diagnostic;
  ASSERT_TRUE(Result.Recovery.complete()) << Result.Recovery.Diagnostic;
  ASSERT_FALSE(Result.Recovery.Residual.Blocks.empty());
  const auto &Certificate = *Result.Independence.Certificate;
  ASSERT_EQ(Certificate.Instructions.size(), 2U);
  EXPECT_EQ(Certificate.Instructions[0].NativeBytes,
            (std::vector<uint8_t>{0xb8, 7, 0, 0, 0}));
  EXPECT_EQ(Certificate.Instructions[1].NativeBytes,
            (std::vector<uint8_t>{0xc3}));
  for (const auto &Insn : Certificate.Instructions) {
    EXPECT_EQ(Insn.NativeBytes.size(), Insn.Origin.Size);
    EXPECT_EQ(Insn.UndefinedEffects.Coverage, LowUndefinedCoverage::Complete);
    EXPECT_TRUE(Insn.UndefinedEffects.Effects.empty());
    EXPECT_EQ(Insn.UndefinedEffects.OpCount, Insn.Ops.size());
    EXPECT_EQ(Insn.UndefinedEffects.OperationDigest,
              lowUndefinedOperationDigest(Insn.Ops));
  }
  // Mutating the image cannot mutate the certificate's retained byte storage.
  P.Image.Segments[0].Data[1] = 9;
  EXPECT_EQ(Certificate.Instructions[0].NativeBytes[1], 7);
}

TEST(OriginalBinaryUndefinedIndependence,
     Disp32WordShiftKeepsMemoryAndUndefinedFlagObligations) {
  // MOV [RSP-8],RCX; SHL word [RSP-8],3 using disp32; MOVZX EAX,[RSP-8]; RET.
  // The full frame write remains observed; arbitrary AF is initially dead.
  Program P({0x48, 0x89, 0x4c, 0x24, 0xf8, 0x66, 0xc1, 0xa4, 0x24, 0xf8,
             0xff, 0xff, 0xff, 3,    0x0f, 0xb7, 0x44, 0x24, 0xf8, 0xc3});
  const auto Good = P.check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_EQ(Good.Certificate->Instructions.size(), 4U);
  const auto &Shift = Good.Certificate->Instructions[1];
  EXPECT_EQ(Shift.Origin.Address, Entry + 5);
  EXPECT_EQ(Shift.UndefinedEffects.Coverage, LowUndefinedCoverage::Complete);
  EXPECT_EQ(Shift.UndefinedEffects.Effects.size(), 2U);
  P.Contract.ReturnRegisters.push_back({x86reg::AF, 1});
  expectRefusal(P, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     OverlappingImmediateEntriesRetainBothInstructionInterpretations) {
  // TEST ECX,ECX; JZ second_mov; first_mov: MOV EAX,0x7b8; RET; RET.
  // second_mov starts one byte into first_mov and consumes its RET byte:
  // MOV EAX,0xc3000007; RET. Both paths are feasible for shared inputs.
  Program P({0x85, 0xc9, 0x74, 1, 0xb8, 0xb8, 7, 0, 0, 0xc3, 0xc3});
  expectRefusal(P, Status::Unsupported);
  P.Contract.AllowOverlappingNativeInstructions = true;
  const auto Good = P.check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_TRUE(Good.Certificate);
  ASSERT_EQ(Good.Certificate->Instructions.size(), 6u);
  const auto &Instructions = Good.Certificate->Instructions;
  const auto First =
      std::find_if(Instructions.begin(), Instructions.end(),
                   [](const auto &I) { return I.Origin.Address == Entry + 4; });
  const auto Second =
      std::find_if(Instructions.begin(), Instructions.end(),
                   [](const auto &I) { return I.Origin.Address == Entry + 5; });
  ASSERT_NE(First, Instructions.end());
  ASSERT_NE(Second, Instructions.end());
  EXPECT_EQ(First->NativeBytes, (std::vector<uint8_t>{0xb8, 0xb8, 7, 0, 0}));
  EXPECT_EQ(Second->NativeBytes, (std::vector<uint8_t>{0xb8, 7, 0, 0, 0xc3}));
  EXPECT_EQ(First->UndefinedEffects.OperationDigest,
            lowUndefinedOperationDigest(First->Ops));
  EXPECT_EQ(Second->UndefinedEffects.OperationDigest,
            lowUndefinedOperationDigest(Second->Ops));
}

TEST(OriginalBinaryUndefinedIndependence,
     IndirectReturnInsideEarlierImmediateRequiresCompleteEvidence) {
  // MOV EAX,0xc3909090; JMP RDX, with shared RDX pointing at the C3 byte
  // inside the MOV immediate. The separately decoded RET returns normally.
  Program P({0xb8, 0x90, 0x90, 0x90, 0xc3, 0xff, 0xe2});
  P.Contract.EntryConstants.push_back({NdVar::reg(x86reg::RDX, 8), Entry + 4});
  P.Options.EntryConstants.push_back({NdVar::reg(x86reg::RDX, 8), Entry + 4});
  const auto Strict = P.check();
  EXPECT_EQ(Strict.Proof.Status, Status::Unsupported);
  EXPECT_FALSE(Strict.Certificate);
  P.Contract.AllowOverlappingNativeInstructions = true;
  const auto Good = P.check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_EQ(Good.Certificate->Instructions.size(), 3u);
  LowIRIndependenceLimits Limits;
  Limits.MaxNativeInstructionBytes = 7; // Eight bytes of entry evidence.
  const auto Limited = P.check(Limits);
  EXPECT_EQ(Limited.Proof.Status, Status::BudgetExceeded);
  EXPECT_FALSE(Limited.Certificate);
}

TEST(OriginalBinaryUndefinedIndependence,
     UnreachableUnauditedBoundariesRequireOptInAndRetainExactReceipts) {
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0xd1, 0xd0}, {0xf0, 0x0f, 0xc1, 0x08}, {0xf3, 0xa4}}) {
    // CMP EAX,EAX; JNE boundary; MOV EAX,7; RET; boundary: body; RET.
    Program P({0x39, 0xc0, 0x75, 6, 0xb8, 7, 0, 0, 0, 0xc3});
    P.Image.Segments.front().Data.insert(P.Image.Segments.front().Data.end(),
                                         Bytes.begin(), Bytes.end());
    P.append({0xc3});
    // Compact explicit preservation ranges isolate the instruction budget
    // from the native wrapper's eight individual default byte obligations.
    P.Contract.PreservedRegisters = {{x86reg::RSP, 8}};
    P.Contract.PreservedFrameRanges = {{0, 8}};
    expectRefusal(P, Status::Unsupported);
    P.Contract.RetainUnauditedNativeBoundaries = true;
    const auto Good = P.check();
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    const auto &Inner = Good.Certificate->LowIR;
    ASSERT_EQ(Inner.NativeAuditBoundaries.size(), 1u);
    const auto &Receipt = Inner.NativeAuditBoundaries.front();
    EXPECT_EQ(Receipt.Kind,
              LowIRNativeAuditBoundaryKind::MissingUndefinedOutputs);
    EXPECT_EQ(Receipt.SemanticsVersion, 1u);
    EXPECT_EQ(Receipt.Boundary.Address, Entry + 10);
    EXPECT_EQ(Receipt.Boundary.Size, Bytes.size());
    EXPECT_EQ(Receipt.NativeBytesDigest.size(), 64u);
    EXPECT_EQ(Receipt.OperationDigest.size(), 64u);
    ASSERT_EQ(Good.Certificate->Instructions.size(), 5u);
    for (const auto &Insn : Good.Certificate->Instructions)
      if (Insn.Origin.Address == Entry + 10) {
        EXPECT_EQ(Insn.NativeBytes, Bytes);
        EXPECT_EQ(Insn.UndefinedEffects.Coverage,
                  LowUndefinedCoverage::Missing);
        EXPECT_EQ(Receipt.OperationDigest,
                  lowUndefinedOperationDigest(Insn.Ops));
      }
    LowIRIndependenceLimits Limits;
    Limits.MaxInstructions = 5;
    const auto Limited = P.check(Limits);
    ASSERT_TRUE(Limited.proved()) << Limited.Proof.Diagnostic;
    Limits.MaxInstructions = 4;
    expectRefusal(P, Status::BudgetExceeded, Limits);
    Limits = {};
    ASSERT_GT(Good.Proof.SolverQueries, 0u);
    Limits.MaxSolverQueries = Good.Proof.SolverQueries - 1;
    expectRefusal(P, Status::BudgetExceeded, Limits);
    // Making the branch feasible cannot turn the boundary into an empty body.
    P.Image.Segments.front().Data[2] = 0x74;
    expectRefusal(P, Status::Unsupported);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     BoundaryRequiresProofForSymbolicAndArbitraryControl) {
  // ECX > 7 returns; only ECX <= 7 reaches the second ECX > 9 test.
  Program Dead({0x83, 0xf9, 7, 0x77, 5, 0x83, 0xf9, 9, 0x77, 6, 0xb8, 7, 0, 0,
                0, 0xc3, 0xd1, 0xd0, 0xc3});
  Dead.Contract.RetainUnauditedNativeBoundaries = true;
  const auto Good = Dead.check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  // The second guard becomes feasible for ECX in [4,7].
  Dead.Image.Segments.front().Data[7] = 3;
  expectRefusal(Dead, Status::Unsupported);
  Program Arbitrary({0x0f, 0xa3, 0xc8, 0x70, 1, 0xc3, 0xd1, 0xd0, 0xc3});
  Arbitrary.flagsProfile();
  Arbitrary.Contract.RetainUnauditedNativeBoundaries = true;
  expectRefusal(Arbitrary, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     EntryIndirectCallAndReturnArrivalsCannotCrossAnUnauditedBoundary) {
  const std::vector<std::vector<uint8_t>> Programs = {
      {0xd1, 0xd0, 0xc3},
      {0x48, 0x8d, 0x05, 2, 0, 0, 0, 0xff, 0xe0, 0xd1, 0xd0, 0xc3},
      {0xe8, 1, 0, 0, 0, 0xc3, 0xd1, 0xd0, 0xc3},
      {0x68, 6, 0x10, 0, 0, 0xc3, 0xd1, 0xd0, 0xc3}};
  const uint64_t BoundaryOffsets[] = {0, 9, 6, 6};
  for (size_t I = 0; I != Programs.size(); ++I) {
    Program P({});
    P.Image.Segments.front().Data = Programs[I];
    P.append({});
    P.Contract.RetainUnauditedNativeBoundaries = true;
    expectRefusal(P, Status::Unsupported);
    const auto Result = P.check();
    EXPECT_EQ(Result.Proof.InstructionAddress, Entry + BoundaryOffsets[I]);
    EXPECT_EQ(Result.Proof.Diagnostic,
              "feasible path reaches an unaudited native boundary");
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     RegisterXaddDefinesFlagsWithoutClearingExistingDependencies) {
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0x0f, 0xc0, 0xc4},
                                         {0x0f, 0xc0, 0xe0},
                                         {0x66, 0x0f, 0xc1, 0xc8},
                                         {0x0f, 0xc1, 0xc8},
                                         {0x48, 0x0f, 0xc1, 0xc0},
                                         {0x4d, 0x0f, 0xc1, 0xc8}}) {
    Program P({});
    P.flagsProfile();
    auto &Code = P.Image.Segments.front();
    Code.Data = Bytes;
    P.append({0xc3});
    for (unsigned I = 1; I != 16; ++I)
      P.Contract.ReturnRegisters.push_back({I * 8, 8});
    for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                      x86reg::SF, x86reg::DF, x86reg::OF})
      P.Contract.ReturnRegisters.push_back({Flag, 1});
    LowIRIndependenceLimits Limits;
    Limits.MaxProducers = 0;
    const auto Good = P.check(Limits);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_TRUE(Good.Certificate->Instructions.front()
                    .UndefinedEffects.Effects.empty());
  }
  // BT; SETO AL; MOVZX EAX,AL; XADD EAX,ECX; RET. Empty XADD effects do
  // not turn an earlier arbitrary OF input into an independent result.
  Program Earlier({0x0f, 0xa3, 0xca, 0x0f, 0x90, 0xc0, 0x0f, 0xb6, 0xc0, 0x0f,
                   0xc1, 0xc8, 0xc3});
  Earlier.flagsProfile();
  expectRefusal(Earlier, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     MemoryXaddKeepsDefinedFrameUpdatesAndEarlierDependencies) {
  for (const auto &Bytes : std::vector<std::vector<uint8_t>>{
           {0x0f, 0xc0, 0x44, 0x24, 0xf0},
           {0x66, 0x0f, 0xc1, 0x44, 0x24, 0xf0},
           {0x0f, 0xc1, 0x84, 0x24, 0xf0, 0xff, 0xff, 0xff},
           {0x4c, 0x0f, 0xc1, 0x6c, 0x24, 0xf0}}) {
    Program P({});
    P.flagsProfile();
    P.Image.Segments.front().Data = Bytes;
    P.append({0xc3});
    for (unsigned I = 1; I != 16; ++I)
      P.Contract.ReturnRegisters.push_back({I * 8, 8});
    for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                      x86reg::SF, x86reg::DF, x86reg::OF})
      P.Contract.ReturnRegisters.push_back({Flag, 1});
    LowIRIndependenceLimits Limits;
    Limits.MaxProducers = 0;
    const auto Good = P.check(Limits);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_TRUE(Good.Certificate->Instructions.front()
                    .UndefinedEffects.Effects.empty());
    EXPECT_TRUE(P.recover(Limits).Recovery.complete());
    Limits.MaxOperations = Good.Proof.Operations;
    ASSERT_TRUE(P.check(Limits).proved());
    --Limits.MaxOperations;
    expectRefusal(P, Status::BudgetExceeded, Limits);
  }
  // An arbitrary earlier OF flows through AL into the observed frame sum,
  // even though XADD overwrites AL and defines all arithmetic flags itself.
  Program Earlier(
      {0x0f, 0xa3, 0xca, 0x0f, 0x90, 0xc0, 0x0f, 0xc0, 0x44, 0x24, 0xf0, 0xc3});
  Earlier.flagsProfile();
  expectRefusal(Earlier, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     DoubleShiftResultAndFlagDomainsUseExactCountThresholds) {
  for (bool Right : {false, true})
    for (unsigned Width : {2U, 4U, 8U})
      for (uint8_t Raw : {0, 1, 2, 15, 16, 17, 31, 32, 63, 64, 255}) {
        SCOPED_TRACE(::testing::Message()
                     << Right << '/' << Width << '/' << unsigned(Raw));
        Program P({});
        P.flagsProfile();
        if (Width == 2)
          P.append({0x66});
        if (Width == 8)
          P.append({0x48});
        P.append({0x0f, uint8_t(Right ? 0xac : 0xa4), 0xd0, Raw, 0xc3});
        const unsigned Count = Raw & (Width == 8 ? 63 : 31);
        const bool BadResult = Count > Width * 8;
        if (BadResult)
          expectRefusal(P, Status::Dependent);
        else {
          const auto Good = P.recover();
          ASSERT_TRUE(Good.Independence.proved())
              << Good.Independence.Proof.Diagnostic;
          EXPECT_TRUE(Good.Recovery.complete()) << Good.Recovery.Diagnostic;
        }
        // RET's LowIR operand observes RAX even if the explicit register
        // contract only names a flag. Discard AX without changing flags.
        auto FlagsOnly = P;
        FlagsOnly.Image.Segments.front().Data.pop_back();
        FlagsOnly.append({0xb8, 0, 0, 0, 0, 0xc3});
        for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                          x86reg::SF, x86reg::DF, x86reg::OF}) {
          FlagsOnly.Contract.ReturnRegisters = {{Flag, 1}};
          const bool Arbitrary = Flag == x86reg::DF   ? false
                                 : Flag == x86reg::AF ? Count != 0
                                 : Flag == x86reg::OF ? Count > 1
                                                      : BadResult;
          const auto R = FlagsOnly.check();
          EXPECT_EQ(R.Proof.Status,
                    Arbitrary ? Status::Dependent : Status::Proved)
              << Flag << '/' << R.Proof.Diagnostic;
          EXPECT_EQ(bool(R.Certificate), !Arbitrary);
        }
      }
}

TEST(OriginalBinaryUndefinedIndependence,
     DoubleShiftSavedCountAndDiscardedOutputsRespectProofBudgets) {
  // A symbolic word result may be arbitrary. Discarding AX is sufficient
  // only while none of the newly undefined arithmetic flags are observed.
  Program P({0x66, 0x0f, 0xa5, 0xd0, 0xb8, 7, 0, 0, 0, 0xc3});
  P.flagsProfile();
  const auto Good = P.check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(
      Good.Certificate->Instructions.front().UndefinedEffects.Effects.size(),
      7U);
  LowIRIndependenceLimits Limits;
  Limits.MaxProducers = Good.Proof.Producers;
  Limits.MaxOperations = Good.Proof.Operations;
  ASSERT_TRUE(P.check(Limits).proved());
  --Limits.MaxProducers;
  expectRefusal(P, Status::BudgetExceeded, Limits);
  Limits.MaxProducers = Good.Proof.Producers;
  --Limits.MaxOperations;
  expectRefusal(P, Status::BudgetExceeded, Limits);
  P.Contract.ReturnRegisters.push_back({x86reg::CF, 1});
  expectRefusal(P, Status::Dependent);
  // CL aliases the result, but the audit condition uses its entry snapshot.
  Program Aliased({0x66, 0x0f, 0xa5, 0xd1, 0xc3});
  Aliased.flagsProfile();
  Aliased.Contract.ReturnRegisters = {{x86reg::RCX, 2}};
  expectRefusal(Aliased, Status::Dependent);
  Program Earlier(
      {0x0f, 0xa3, 0xca, 0x0f, 0x90, 0xc0, 0x0f, 0xa4, 0xd0, 1, 0xc3});
  Earlier.flagsProfile();
  expectRefusal(Earlier, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     CompatibilityLeftShiftRetainsCountDependentRefusals) {
  for (uint8_t Count : {0, 1, 7, 8, 31, 32, 33}) {
    Program P({0xc0, 0xf0, Count, 0xc3}); // SAL /6 AL,imm8.
    P.flagsProfile();
    P.Contract.ReturnRegisters.push_back({x86reg::AF, 1});
    if (Count & 31) {
      expectRefusal(P, Status::Dependent);
    } else {
      const auto Good = P.check();
      ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    }
    P.Contract.ReturnRegisters.back() = {x86reg::CF, 1};
    if ((Count & 31) >= 8) {
      expectRefusal(P, Status::Dependent);
    } else {
      const auto Good = P.check();
      ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    }
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     BitTestsKeepDefinedCarryAndPreservedFlagsWithExactProducerBudget) {
  for (uint8_t Opcode : {0xa3, 0xab, 0xb3, 0xbb}) {
    Program P({0x0f, Opcode, 0xc8, 0xc3}); // bit-test family eax,ecx; ret.
    P.flagsProfile();
    P.Contract.ReturnRegisters = {
        {x86reg::RAX, 8}, {x86reg::CF, 1}, {x86reg::ZF, 1}, {x86reg::DF, 1}};
    LowIRIndependenceLimits Limits;
    Limits.MaxProducers = 4;
    const auto Good = P.check(Limits);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    ASSERT_EQ(
        Good.Certificate->Instructions.front().UndefinedEffects.Effects.size(),
        4u);
    Limits.MaxProducers = 3;
    expectRefusal(P, Status::BudgetExceeded, Limits);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     BitTestFreshFlagsAreIndependentAndRepeatedReadsStayCorrelated) {
  // BT; SETO DL; SETO AL; XOR AL,DL; MOVZX EAX,AL; RET.
  Program Same({0x0f, 0xa3, 0xc8, 0x0f, 0x90, 0xc2, 0x0f, 0x90, 0xc0, 0x30,
                0xd0, 0x0f, 0xb6, 0xc0, 0xc3});
  Same.flagsProfile();
  const auto Correlated = Same.check();
  ASSERT_TRUE(Correlated.proved()) << Correlated.Proof.Diagnostic;
  // Reading SF instead of OF must not reuse the other fresh bit.
  Same.Image.Segments[0].Data[7] = 0x98;
  expectRefusal(Same, Status::Dependent);
  // Even identical terminal results cannot hide control depending on OF.
  Program Branch({0x0f, 0xa3, 0xc8, 0x70, 1, 0x90, 0xc3});
  Branch.flagsProfile();
  expectRefusal(Branch, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     RotateUndefinedOverflowUsesMaskedCountBeforeNarrowModulo) {
  for (uint8_t ModRM : {0xc0, 0xc8})
    for (uint8_t Count : {0, 1, 8, 9, 16, 32, 33}) {
      Program P({0xc0, ModRM, Count, 0xc3}); // ROL/ROR AL,imm8.
      P.flagsProfile();
      P.Contract.ReturnRegisters.push_back({x86reg::OF, 1});
      if ((Count & 31) > 1) {
        expectRefusal(P, Status::Dependent);
      } else {
        const auto Good = P.check();
        ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
      }
    }
  for (uint8_t ModRM : {0xc0, 0xc8}) {
    Program P({0xd2, ModRM, 0xc3}); // All symbolic CL counts.
    P.flagsProfile();
    P.Contract.ReturnRegisters = {
        {x86reg::RAX, 8}, {x86reg::CF, 1}, {x86reg::ZF, 1}, {x86reg::SF, 1},
        {x86reg::AF, 1},  {x86reg::PF, 1}, {x86reg::DF, 1}};
    const auto Good = P.check();
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     UndefinedBranchCannotBeCertifiedAfterOrdinaryRecoveryPrunesIt) {
  // xor eax,eax; lahf; test ah,0x10; jne second; mov eax,0; ret;
  // second: mov eax,0; ret. Both return values agree, but control consumes AF.
  Program P({0x31, 0xc0, 0x9f, 0xf6, 0xc4, 0x10, 0x75, 0x06, 0xb8, 0,
             0,    0,    0,    0xc3, 0xb8, 0,    0,    0,    0,    0xc3});
  // An entry AF constant is killed by XOR's newly arbitrary AF production.
  // The ordinary selected LowIR value must not become architecture evidence.
  P.Options.EntryConstants.push_back({NdVar::reg(x86reg::AF, 1), 0});
  P.Contract.EntryConstants.push_back({NdVar::reg(x86reg::AF, 1), 0});
  const auto Ordinary = specializeBinaryInterpreter(P.Image, Entry, P.Options);
  ASSERT_TRUE(Ordinary.complete()) << Ordinary.Diagnostic;
  expectRefusal(P, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     OrdinaryInputBranchRetainsBothOriginalArms) {
  // test edi,edi; jne second; mov eax,0; ret; second: mov eax,1; ret.
  Program P(
      {0x85, 0xff, 0x75, 0x06, 0xb8, 0, 0, 0, 0, 0xc3, 0xb8, 1, 0, 0, 0, 0xc3});
  const auto Result = P.check();
  ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
  std::vector<va_t> Addresses;
  for (const auto &Insn : Result.Certificate->Instructions)
    Addresses.push_back(Insn.Origin.Address);
  std::sort(Addresses.begin(), Addresses.end());
  EXPECT_EQ(Addresses, (std::vector<va_t>{0x1000, 0x1002, 0x1004, 0x1009,
                                          0x100a, 0x100f}));
}

TEST(OriginalBinaryUndefinedIndependence,
     PhysicalCallAndReturnPreserveMachineState) {
  // mov eax,7; call helper; ret; helper: add eax,1; ret.
  Program P({0xb8, 7, 0, 0, 0, 0xe8, 1, 0, 0, 0, 0xc3, 0x83, 0xc0, 1, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.Independence.proved()) << R.Independence.Proof.Diagnostic;
  EXPECT_TRUE(R.Recovery.complete()) << R.Recovery.Diagnostic;
  EXPECT_EQ(R.Independence.Certificate->LowIR.Scope,
            LowIRIndependenceScope::CompleteFiniteNativePaths);
  const auto &Instructions = R.Independence.Certificate->Instructions;
  EXPECT_EQ(std::count_if(Instructions.begin(), Instructions.end(),
                          [](const auto &I) { return I.IsNativeCall; }),
            1);
  EXPECT_EQ(R.Independence.Proof.Paths, 1u);
}

TEST(OriginalBinaryUndefinedIndependence,
     RepeatedHelperUsesActualContinuationSlots) {
  // Two calls to the same helper must not share their different return slots.
  Program P({0xb8, 7, 0, 0, 0, 0xe8, 6,    0,    0, 0,
             0xe8, 1, 0, 0, 0, 0xc3, 0x83, 0xc0, 1, 0xc3});
  const auto R = P.check();
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  const auto &Trace = R.Certificate->LowIR.Instructions;
  EXPECT_EQ(
      std::count_if(Trace.begin(), Trace.end(),
                    [](const auto &I) { return I.Boundary.Address == 0x1013; }),
      2);
}

TEST(OriginalBinaryUndefinedIndependence,
     MemoryCallReadsBeforePushOverwritesTarget) {
  // mov rax,helper; mov [rsp-8],rax; call [rsp-8]; ret; helper: mov eax,7; ret.
  Program P({0x48, 0xb8, 0x14, 0x10, 0,    0,    0,    0,    0,
             0,    0x48, 0x89, 0x44, 0x24, 0xf8, 0xff, 0x54, 0x24,
             0xf8, 0xc3, 0xb8, 7,    0,    0,    0,    0xc3});
  const auto R = P.check();
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  EXPECT_EQ(R.Proof.Paths, 1u);
}

TEST(OriginalBinaryUndefinedIndependence,
     PartialReturnSlotWriteSelectsRealTarget) {
  // call helper; ret; helper: mov word [rsp],0x1010; ret; padding; destination.
  // The original fallthrough is 0x1005, not the modified return target 0x1010.
  Program P({0xe8, 1,    0,    0,    0,    0xc3, 0x66, 0xc7, 0x04, 0x24, 0x10,
             0x10, 0xc3, 0x90, 0x90, 0x90, 0xb8, 9,    0,    0,    0,    0xc3});
  const auto R = P.check();
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  const auto &Trace = R.Certificate->LowIR.Instructions;
  EXPECT_TRUE(std::any_of(Trace.begin(), Trace.end(), [](const auto &I) {
    return I.Boundary.Address == 0x1010;
  }));
  EXPECT_FALSE(std::any_of(Trace.begin(), Trace.end(), [](const auto &I) {
    return I.Boundary.Address == 0x1005;
  }));
}

TEST(OriginalBinaryUndefinedIndependence,
     DiscardedCallFrameReachesOuterReturn) {
  // call helper; ret; helper: add rsp,8; ret.
  Program P({0xe8, 1, 0, 0, 0, 0xc3, 0x48, 0x83, 0xc4, 8, 0xc3});
  const auto R = P.check();
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  EXPECT_EQ(R.Proof.Paths, 1u);
}

TEST(OriginalBinaryUndefinedIndependence,
     InternalReturnReclaimsStackArguments) {
  // sub rsp,24; call helper; ret; helper: mov eax,7; ret 24.
  Program P({0x48, 0x83, 0xec, 24, 0xe8, 1, 0, 0, 0, 0xc3, 0xb8, 7, 0, 0, 0,
             0xc2, 24, 0});
  const auto R = P.recover();
  ASSERT_TRUE(R.Independence.proved()) << R.Independence.Proof.Diagnostic;
  ASSERT_TRUE(R.Recovery.complete()) << R.Recovery.Diagnostic;
  EXPECT_EQ(R.Independence.Proof.Paths, 1u);
  // Incorrect cleanup cannot establish the mandatory restored outer frame.
  P.Image.Segments[0].Data[16] = 16;
  const auto Bad = P.check();
  EXPECT_FALSE(Bad.proved());
  EXPECT_FALSE(Bad.Certificate.has_value());
}

TEST(OriginalBinaryUndefinedIndependence, PrefixedReturnsDoNotAssumePopWidth) {
  for (auto Bytes : {std::initializer_list<uint8_t>{0x66, 0xc3},
                     std::initializer_list<uint8_t>{0x66, 0xc2, 8, 0},
                     std::initializer_list<uint8_t>{0xf3, 0xc3}}) {
    Program P(Bytes);
    expectRefusal(P, Status::Unsupported);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     GuardedAlignmentUsesTheWholeEntryDomain) {
  // Save the physical root, compute its low bits, allocate and align. Only
  // residue 3 calls the helper; every other residue restores the root and
  // returns a different result. The entry domain has no alignment assertion.
  // mov r11,rsp; mov rax,rsp; and eax,15; sub rsp,64; and rsp,-16;
  // cmp eax,3; jne fallback; call helper; mov rsp,r11; ret;
  // fallback: mov rsp,r11; mov eax,9; ret; helper: mov eax,7; ret.
  Program P({0x49, 0x89, 0xe3, 0x48, 0x89, 0xe0, 0x83, 0xe0, 0x0f, 0x48,
             0x83, 0xec, 0x40, 0x48, 0x83, 0xe4, 0xf0, 0x83, 0xf8, 0x03,
             0x75, 0x09, 0xe8, 0x0d, 0,    0,    0,    0x4c, 0x89, 0xdc,
             0xc3, 0x4c, 0x89, 0xdc, 0xb8, 9,    0,    0,    0,    0xc3,
             0xb8, 7,    0,    0,    0,    0xc3});
  P.Contract.Frame->Begin = -96;
  const auto R = P.recover();
  ASSERT_TRUE(R.Independence.proved()) << R.Independence.Proof.Diagnostic;
  EXPECT_EQ(R.Independence.Proof.Paths, 2u);
  ASSERT_TRUE(R.Recovery.complete()) << R.Recovery.Diagnostic;
  EXPECT_GT(R.Independence.Proof.SolverQueries, 0u);
}

TEST(OriginalBinaryUndefinedIndependence, CalleeCannotCorruptEntryReturnSlot) {
  // The internal return slot is [rsp]; [rsp+8] is the outer entry slot.
  Program P(
      {0xe8, 1, 0, 0, 0, 0xc3, 0x48, 0xc7, 0x44, 0x24, 8, 0, 0, 0, 0, 0xc3});
  expectRefusal(P, Status::ContractViolation);
}

TEST(OriginalBinaryUndefinedIndependence,
     FiniteIndirectTargetsRequireFullCoverage) {
  // and edi,1; add edi,0x1010; jmp rdi; padding; ret; ret.
  Program P({0x83, 0xe7, 1, 0x81, 0xc7, 0x10, 0x10, 0, 0, 0xff, 0xe7, 0x90,
             0x90, 0x90, 0x90, 0x90, 0xc3, 0xc3});
  LowIRIndependenceLimits Limits;
  Limits.MaxIndirectTargets = 2;
  const auto R = P.check(Limits);
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  EXPECT_EQ(R.Proof.Paths, 2u);
  Limits.MaxIndirectTargets = 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
  Limits.MaxIndirectTargets = 2;
  Limits.MaxPaths = 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     UndefinedIndirectTargetIsCheckedBeforePartition) {
  // XOR's arbitrary AF is copied into AH, isolated, then used as a target bit.
  Program P({0x31, 0xc0, 0x9f, 0x25, 0, 0x10, 0, 0, 0x48, 0x0d, 0, 0x20, 0, 0,
             0xff, 0xe0});
  const auto R = P.check();
  EXPECT_EQ(R.Proof.Status, Status::Dependent) << R.Proof.Diagnostic;
  EXPECT_NE(R.Proof.Diagnostic.find("control target"), std::string::npos);
  EXPECT_FALSE(R.Certificate);
}

TEST(OriginalBinaryUndefinedIndependence,
     UndefinedChoicesSurvivePhysicalCallAndReturn) {
  // xor eax,eax; call helper; lahf; isolate AF; form target; jmp rax;
  // helper: ret. The caller's arbitrary AF crosses both CALL and RET.
  Program P({0x31, 0xc0, 0xe8, 0x0e, 0, 0,    0, 0x9f, 0x25, 0,    0x10,
             0,    0,    0x48, 0x0d, 0, 0x20, 0, 0,    0xff, 0xe0, 0xc3});
  const auto R = P.check();
  EXPECT_EQ(R.Proof.Status, Status::Dependent) << R.Proof.Diagnostic;
  EXPECT_NE(R.Proof.Diagnostic.find("control target"), std::string::npos);
  EXPECT_FALSE(R.Certificate);
}

TEST(OriginalBinaryUndefinedIndependence,
     ImmutableReadsBindBytesAndDataMapping) {
  // mov rax,[rip+1]; ret; immutable eight-byte scalar.
  for (bool FixedStack : {false, true}) {
    SCOPED_TRACE(FixedStack);
    Program P({0x48, 0x8b, 5, 1, 0, 0, 0, 0xc3, 7, 0, 0, 0, 0, 0, 0, 0});
    if (FixedStack) {
      P.Contract.EntryConstants.push_back({NdVar::reg(x86reg::RSP, 8), 0x8000});
      P.Options.EntryConstants.push_back({NdVar::reg(x86reg::RSP, 8), 0x8000});
    }
    const auto First = P.check();
    ASSERT_TRUE(First.proved()) << First.Proof.Diagnostic;
    ASSERT_EQ(First.Certificate->Reads.size(), 1u);
    EXPECT_EQ(First.Certificate->Reads[0].Bytes[0], 7u);
    P.Image.Segments[0].Data[8] = 9;
    const auto Second = P.check();
    ASSERT_TRUE(Second.proved()) << Second.Proof.Diagnostic;
    EXPECT_NE(First.Certificate->InputDigest, Second.Certificate->InputDigest);
    P.Image.Segments[0].FileOff += 32;
    const auto Third = P.check();
    ASSERT_TRUE(Third.proved()) << Third.Proof.Diagnostic;
    EXPECT_NE(Second.Certificate->InputDigest, Third.Certificate->InputDigest);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     SeparateImmutableDataMappingIsBoundWithoutNativeInstructions) {
  // mov rax,[rip+0xff9]; ret. The data owner contains no fetched instructions,
  // so a code-mapping digest cannot accidentally cover its mapping metadata.
  Program P({0x48, 0x8b, 5, 0xf9, 0x0f, 0, 0, 0xc3});
  Segment Data;
  Data.VA = 0x2000;
  Data.Flags = SegmentFlags::Readable;
  Data.Data = {7, 0, 0, 0, 0, 0, 0, 0};
  Data.Size = Data.FileSz = Data.Data.size();
  P.Image.Segments.push_back(Data);
  const auto First = P.check();
  ASSERT_TRUE(First.proved()) << First.Proof.Diagnostic;
  ASSERT_EQ(First.Certificate->Reads.size(), 1U);
  EXPECT_EQ(First.Certificate->Reads[0].Address, Data.VA);
  for (const auto &Insn : First.Certificate->Instructions)
    EXPECT_LT(Insn.Origin.Address, Data.VA);
  P.Image.Segments[1].FileOff += 32;
  const auto Second = P.check();
  ASSERT_TRUE(Second.proved()) << Second.Proof.Diagnostic;
  EXPECT_EQ(First.Certificate->Reads[0].Bytes,
            Second.Certificate->Reads[0].Bytes);
  EXPECT_NE(First.Certificate->InputDigest, Second.Certificate->InputDigest);
}

TEST(OriginalBinaryUndefinedIndependence,
     UnreachableTrapRetainsBytesAndMissingEffects) {
  // xor eax,eax; jne trap; ret; trap. No bytes follow the trap, so inventing
  // a resumption edge would fail even though the original branch is untaken.
  for (bool InvalidOpcode : {false, true}) {
    SCOPED_TRACE(InvalidOpcode);
    Program P({0x31, 0xc0, 0x75, 1, 0xc3, 0xcc});
    if (InvalidOpcode) {
      P.Image.Segments[0].Data.back() = 0x0f;
      P.Image.Segments[0].Data.push_back(0x0b);
      ++P.Image.Segments[0].Size;
      ++P.Image.Segments[0].FileSz;
    }
    const auto R = P.recover();
    ASSERT_TRUE(R.Independence.proved()) << R.Independence.Proof.Diagnostic;
    EXPECT_TRUE(R.Recovery.complete()) << R.Recovery.Diagnostic;
    const auto &Insns = R.Independence.Certificate->Instructions;
    const auto It = std::find_if(Insns.begin(), Insns.end(), [](const auto &I) {
      return I.Origin.Control == LowInstructionControl::Terminator;
    });
    ASSERT_NE(It, Insns.end());
    EXPECT_EQ(It->Origin.Address, Entry + 5);
    EXPECT_EQ(It->NativeBytes, InvalidOpcode
                                   ? (std::vector<uint8_t>{0x0f, 0x0b})
                                   : (std::vector<uint8_t>{0xcc}));
    EXPECT_EQ(It->UndefinedEffects.Coverage, LowUndefinedCoverage::Missing);
    EXPECT_EQ(It->UndefinedEffects.OperationDigest,
              lowUndefinedOperationDigest(It->Ops));
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     DiscardedCallContinuationCanContainATrap) {
  // call helper; int3; helper: add rsp,8; mov eax,7; ret. The real native
  // stack determines the outer return; the discarded continuation is not an
  // instruction edge and carries no claim about the skipped byte.
  Program P(
      {0xe8, 1, 0, 0, 0, 0xcc, 0x48, 0x83, 0xc4, 8, 0xb8, 7, 0, 0, 0, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.Independence.proved()) << R.Independence.Proof.Diagnostic;
  EXPECT_TRUE(R.Recovery.complete()) << R.Recovery.Diagnostic;
  EXPECT_EQ(R.Independence.Proof.Paths, 1U);
  EXPECT_FALSE(
      std::any_of(R.Independence.Certificate->Instructions.begin(),
                  R.Independence.Certificate->Instructions.end(),
                  [](const auto &I) { return I.Origin.Address == Entry + 5; }));
}

TEST(OriginalBinaryUndefinedIndependence,
     FeasibleTrapCannotPublishAnyCompletedSibling) {
  // jc trap; ret; int3. An ordinary input can select either path. A completed
  // return cannot authorize the sibling exception path under this contract.
  Program Branch({0x72, 1, 0xc3, 0xcc});
  expectRefusal(Branch, Status::ContractViolation);
  Program Direct({0xcc});
  expectRefusal(Direct, Status::ContractViolation);
  Program InvalidOpcode({0x0f, 0x0b});
  expectRefusal(InvalidOpcode, Status::ContractViolation);
  // call helper; int3; helper: ret. The actual continuation is reached.
  Program Call({0xe8, 1, 0, 0, 0, 0xcc, 0xc3});
  expectRefusal(Call, Status::ContractViolation);
  // and eax,1; add rax,0x100b; jmp rax; ret; int3. Complete finite target
  // enumeration must retain the trap target as well as the normal return.
  Program Indirect(
      {0x83, 0xe0, 1, 0x48, 5, 0x0b, 0x10, 0, 0, 0xff, 0xe0, 0xc3, 0xcc});
  expectRefusal(Indirect, Status::ContractViolation);
  const auto OrdinaryTrap =
      specializeBinaryInterpreter(Direct.Image, Entry, Direct.Options);
  EXPECT_FALSE(OrdinaryTrap.complete());
  EXPECT_EQ(OrdinaryTrap.Status, SpecializationStatus::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     StaticallyUntakenBranchStillRequiresOriginalBytes) {
  // XOR makes JNE false in ordinary recovery; its target is unmapped 0x100a.
  Program P({0x31, 0xc0, 0x75, 0x06, 0xb8, 7, 0, 0, 0, 0xc3});
  const auto Ordinary = specializeBinaryInterpreter(P.Image, Entry, P.Options);
  ASSERT_TRUE(Ordinary.complete()) << Ordinary.Diagnostic;
  expectRefusal(P, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     StaticallyUntakenBranchStillRequiresArchitectureCoverage) {
  // XOR makes JNE false, but the original arm at 0x100a contains an unaudited
  // RCL. Collecting its bytes alone does not establish complete evidence.
  Program P({0x31, 0xc0, 0x75, 0x06, 0xb8, 7, 0, 0, 0, 0xc3, 0xd1, 0xd0, 0xc3});
  const auto Ordinary = specializeBinaryInterpreter(P.Image, Entry, P.Options);
  ASSERT_TRUE(Ordinary.complete()) << Ordinary.Diagnostic;
  expectRefusal(P, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     NativeReturnRequiresEntryStackPointerAndReturnAddress) {
  // add rsp,8; mov eax,7; ret: this is now a physical internal transfer, not
  // an outer exit. Its target load exceeds the accessible frame contract.
  Program Pivot({0x48, 0x83, 0xc4, 8, 0xb8, 7, 0, 0, 0, 0xc3});
  expectRefusal(Pivot, Status::Unsupported);
  // mov qword ptr [rsp],0; mov eax,7; ret: the return slot is observable even
  // when the caller opts out of ordinary final frame-byte observations.
  Program Slot({0x48, 0xc7, 0x04, 0x24, 0, 0, 0, 0, 0xb8, 7, 0, 0, 0, 0xc3});
  Slot.Contract.ObserveWrittenFrameBytes = false;
  expectRefusal(Slot, Status::ContractViolation);
}

TEST(OriginalBinaryUndefinedIndependence,
     SavedStackPointerAndReturnSlotMayBeRestored) {
  // push rcx; pop rcx; mov eax,7; ret.
  Program Stack({0x51, 0x59, 0xb8, 7, 0, 0, 0, 0xc3});
  const auto StackResult = Stack.check();
  EXPECT_TRUE(StackResult.proved()) << StackResult.Proof.Diagnostic;
  // mov rdx,[rsp]; mov qword ptr [rsp],0; mov [rsp],rdx; mov eax,7; ret.
  Program Slot({0x48, 0x8b, 0x14, 0x24, 0x48, 0xc7, 0x04, 0x24, 0, 0, 0,
                0,    0x48, 0x89, 0x14, 0x24, 0xb8, 7,    0,    0, 0, 0xc3});
  const auto SlotResult = Slot.check();
  EXPECT_TRUE(SlotResult.proved()) << SlotResult.Proof.Diagnostic;
}

TEST(OriginalBinaryUndefinedIndependence,
     CallNextPreservesOnePushWhileUnboundedTargetsAndCyclesRefuse) {
  Program DirectCall({0xe8, 0, 0, 0, 0, 0xc3});
  const auto Direct = DirectCall.check();
  ASSERT_TRUE(Direct.proved()) << Direct.Proof.Diagnostic;
  // CALL-next can lower to a physical push without a remaining CALL LowOp.
  // POP restores RSP, so native classification must still reject this graph
  // even when its LowIR satisfies the leaf return-preservation obligations.
  Program CallNextPop({0xe8, 0, 0, 0, 0, 0x58, 0xc3});
  const auto Pop = CallNextPop.check();
  ASSERT_TRUE(Pop.proved()) << Pop.Proof.Diagnostic;
  Program IndirectCall({0xff, 0xd0, 0xc3});
  expectRefusal(IndirectCall, Status::BudgetExceeded);
  Program IndirectBranch({0xff, 0xe0});
  expectRefusal(IndirectBranch, Status::BudgetExceeded);
  Program Loop({0xeb, 0xfe});
  expectRefusal(Loop, Status::BudgetExceeded);
  // The branch target 0x1005 decodes as NOP inside the MOV immediate.
  Program Overlap({0x85, 0xff, 0x75, 1, 0xb8, 0x90, 0x90, 0x90, 0x90, 0xc3});
  expectRefusal(Overlap, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     MissingArchitectureEffectsAndProfileProjectionRefuse) {
  Program Rotate({0xd1, 0xd0, 0xc3}); // rcl eax,1; ret
  expectRefusal(Rotate, Status::Unsupported);
  // RDSSPQ is projected to NOP only by the explicit CET-disabled profile.
  // That projection must retain Missing architecture metadata.
  Program ShadowStack({0xf3, 0x48, 0x0f, 0x1e, 0xc8, 0xb8, 7, 0, 0, 0, 0xc3});
  const auto Ordinary = specializeBinaryInterpreter(ShadowStack.Image, Entry,
                                                    ShadowStack.Options);
  ASSERT_TRUE(Ordinary.complete()) << Ordinary.Diagnostic;
  expectRefusal(ShadowStack, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence, ExecutionProfileIsMandatory) {
  for (unsigned Field = 0; Field != 3; ++Field) {
    SCOPED_TRACE(Field);
    Program P({0xb8, 7, 0, 0, 0, 0xc3});
    if (Field == 0)
      P.Options.ExplicitMachineState = false;
    else if (Field == 1)
      P.Options.NormalNonfaultingExecution = false;
    else
      P.Options.X64CetDisabled = false;
    expectRefusal(P, Status::Unsupported);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     LinkedFormatsRequireCompleteImageAndExceptionCoverage) {
  for (auto Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    Program P({0xb8, 7, 0, 0, 0, 0xc3}); // mov eax,7; ret
    P.Image.Format = Format;
    const auto Baseline = P.recover();
    ASSERT_TRUE(Baseline.Independence.proved())
        << Baseline.Independence.Proof.Diagnostic;
    ASSERT_TRUE(Baseline.Recovery.complete()) << Baseline.Recovery.Diagnostic;

    // Partial coverage has no structural or localized-function evidence in
    // this fixture; it must not acquire the meaning of an empty complete map.
    for (auto Parse :
         {ExceptionParseStatus::Partial, ExceptionParseStatus::Malformed}) {
      SCOPED_TRACE(static_cast<unsigned>(Parse));
      P.Image.ExceptionMetadata.ParseStatus = Parse;
      expectRefusal(P, Status::Unsupported);
    }
    P.Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
    P.Image.IsRelocatable = true;
    expectRefusal(P, Status::Invalid);
    P.Image.IsRelocatable = false;

    if (Format == BinaryFormat::COFF) {
      P.Image.LoadOnlyFunctionEntries.insert(Entry);
      expectRefusal(P, Status::Unsupported);
    }
  }
}

TEST(OriginalBinaryUndefinedIndependence, EntryContractsMustMatchRecovery) {
  Program MissingFrame({0xb8, 7, 0, 0, 0, 0xc3});
  MissingFrame.Contract.Frame.reset();
  expectRefusal(MissingFrame, Status::Invalid);
  Program Constants({0xb8, 7, 0, 0, 0, 0xc3});
  Constants.Options.EntryConstants.push_back({NdVar::reg(x86reg::RCX, 8), 3});
  expectRefusal(Constants, Status::Invalid);
  Program Endian({0xb8, 7, 0, 0, 0, 0xc3});
  Endian.Contract.ByteOrder = llvm::endianness::big;
  expectRefusal(Endian, Status::Invalid);
}

TEST(OriginalBinaryUndefinedIndependence,
     EntryFrameCannotOverlapImmutableCode) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  P.Options.EntryConstants.push_back({NdVar::reg(x86reg::RSP, 8), Entry});
  P.Contract.EntryConstants.push_back({NdVar::reg(x86reg::RSP, 8), Entry});
  // The bound [-32,8) frame overlaps the RX instruction mapping. A proof
  // cannot succeed using an inconsistent mutable-frame environment.
  expectRefusal(P, Status::InfeasibleEntry);
}

TEST(OriginalBinaryUndefinedIndependence, CollectionBudgetsDoNotPublishPrefix) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  LowIRIndependenceLimits Instructions;
  Instructions.MaxInstructions = 1;
  expectRefusal(P, Status::BudgetExceeded, Instructions);
  LowIRIndependenceLimits Blocks;
  Blocks.MaxBlockVisits = 1;
  expectRefusal(P, Status::BudgetExceeded, Blocks);
  LowIRIndependenceLimits Operations;
  Operations.MaxOperations = 1;
  expectRefusal(P, Status::BudgetExceeded, Operations);
}

TEST(OriginalBinaryUndefinedIndependence,
     WritableConflictingAndRelocatedMappingsRefuse) {
  Program Writable({0xb8, 7, 0, 0, 0, 0xc3});
  Writable.Image.Segments[0].Flags =
      Writable.Image.Segments[0].Flags | SegmentFlags::Writable;
  expectRefusal(Writable, Status::Unsupported);
  Program Conflicting({0xb8, 7, 0, 0, 0, 0xc3});
  Segment Duplicate = Conflicting.Image.Segments[0];
  Conflicting.Image.Segments.push_back(std::move(Duplicate));
  expectRefusal(Conflicting, Status::Unsupported);
  Program Fixed({0xb8, 7, 0, 0, 0, 0xc3});
  Fixed.Image.Relocations.push_back(
      {.Address = Entry + 1, .Type = llvm::ELF::R_X86_64_64});
  expectRefusal(Fixed, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     BaseRelocationScanningHasAMetadataBudget) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  LowIRIndependenceLimits Limits;
  Limits.MaxInstructions = 16;
  const auto Baseline = P.check(Limits);
  ASSERT_TRUE(Baseline.proved()) << Baseline.Proof.Diagnostic;
  // These fixups do not overlap code. Their quantity alone exceeds the
  // allowed input metadata and must be rejected before repeated scans.
  for (unsigned I = 0; I != 17; ++I)
    P.Image.BaseRelocations.push_back({0x4000 + 8 * I, 10});
  expectRefusal(P, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     EqualLengthByteChangesBindEvenWhenLowIRIsIdentical) {
  // Distinct ignored segment prefixes on NOP have the same lifted behavior
  // and instruction boundaries; binary evidence must still bind their bytes.
  Program P({0x2e, 0x90, 0xb8, 7, 0, 0, 0, 0xc3});
  const auto Before = P.check();
  ASSERT_TRUE(Before.proved()) << Before.Proof.Diagnostic;
  P.Image.Segments[0].Data[0] = 0x3e;
  const auto After = P.check();
  ASSERT_TRUE(After.proved()) << After.Proof.Diagnostic;
  EXPECT_EQ(Before.Certificate->LowIR.InputDigest,
            After.Certificate->LowIR.InputDigest);
  EXPECT_NE(Before.Certificate->InputDigest, After.Certificate->InputDigest);
  ASSERT_FALSE(Before.Certificate->Instructions.empty());
  ASSERT_FALSE(After.Certificate->Instructions.empty());
  EXPECT_EQ(Before.Certificate->Instructions[0].NativeBytes,
            (std::vector<uint8_t>{0x2e, 0x90}));
  EXPECT_EQ(After.Certificate->Instructions[0].NativeBytes,
            (std::vector<uint8_t>{0x3e, 0x90}));
}

TEST(OriginalBinaryUndefinedIndependence, PackedFlagsRequireMatchingOptIn) {
  Program P({0x9c, 0x58, 0xc3}); // pushfq; pop rax; ret.
  expectRefusal(P, Status::Unsupported);
  P.Options.X64FlagsProfile = InterpreterMachineStateProfile::UserX64NoFaultV1;
  expectRefusal(P, Status::Invalid);
  P.flagsProfile();
  const auto Valid = P.check();
  ASSERT_TRUE(Valid.proved()) << Valid.Proof.Diagnostic;
  ASSERT_EQ(Valid.Certificate->LowIR.NativeFlagTransitions.size(), 1U);
  const auto &Receipt = Valid.Certificate->LowIR.NativeFlagTransitions[0];
  EXPECT_EQ(Receipt.InstructionAddress, Entry);
  EXPECT_EQ(Receipt.SemanticsVersion, 1U);
  EXPECT_FALSE(Receipt.OperationDigest.empty());
  P.Options.X64FlagsProfile = P.Contract.X64FlagsProfile =
      static_cast<InterpreterMachineStateProfile>(255);
  expectRefusal(P, Status::Invalid);
}

TEST(OriginalBinaryUndefinedIndependence, PackedFlagsKeepSnapshotCorrelation) {
  // Two unchanged snapshots must agree, including symbolic IF/NT/ID at entry.
  // pushfq; pop rax; pushfq; pop rcx; cmp rax,rcx; jne trap; ret; int3.
  Program P({0x9c, 0x58, 0x9c, 0x59, 0x48, 0x39, 0xc8, 0x75, 1, 0xc3, 0xcc});
  P.flagsProfile();
  const auto Result = P.check();
  ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
  EXPECT_EQ(Result.Certificate->LowIR.NativeFlagTransitions.size(), 2U);
}

TEST(OriginalBinaryUndefinedIndependence, PackedFlagsEntryBitsAreCanonical) {
  const uint64_t Offsets[] = {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                              x86reg::SF, x86reg::DF, x86reg::OF};
  const unsigned Bits[] = {0, 2, 4, 6, 7, 10, 11};
  for (unsigned Combination = 0; Combination != 128; ++Combination) {
    SCOPED_TRACE(Combination);
    Program P({0x9c, 0x58, 0x25, 0xd5, 0x0c, 0, 0});
    P.flagsProfile();
    unsigned Expected = 0;
    for (unsigned I = 0; I != 7; ++I) {
      const uint64_t Value = (Combination >> I) & 1;
      Expected |= Value << Bits[I];
      P.Options.EntryConstants.push_back({NdVar::reg(Offsets[I], 1), Value});
      P.Contract.EntryConstants.push_back({NdVar::reg(Offsets[I], 1), Value});
    }
    P.requireEAX(Expected);
    const auto Result = P.check();
    ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
  }
  for (auto Location : {NdVar::reg(x86reg::AF, 1), NdVar::reg(x86reg::AF, 2),
                        NdVar::reg(x86reg::AF - 1, 2)}) {
    Program P({0x9c, 0x58, 0xc3});
    P.flagsProfile();
    P.Options.EntryConstants.push_back({Location, 2});
    P.Contract.EntryConstants.push_back({Location, 2});
    expectRefusal(P, Status::Invalid);
  }
}

TEST(OriginalBinaryUndefinedIndependence, PackedFlagsApplyUserPrivilegeMasks) {
  for (uint64_t Image : {uint64_t{0}, uint64_t{0x204002}, uint64_t{0x1a3002},
                         ~uint64_t{0x40100}}) {
    SCOPED_TRACE(Image);
    // Save entry IF. POPFQ changes NT/ID, preserves IF, ignores protected and
    // reserved image bits, and keeps architectural bit 1 set.
    Program P({0x9c, 0x59, 0x81, 0xe1, 0, 2, 0, 0, 0x48, 0xb8});
    P.immediate(Image, 8);
    P.append({0x50, 0x9d, 0x9c, 0x58, 0x25, 2, 0x42, 0x20, 0, 0x31, 0xc8});
    P.requireEAX(2 | (Image & 0x204000));
    P.flagsProfile();
    const auto Result = P.check();
    ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     PackedFlagsRejectFeasibleProfileExit) {
  // Unconstrained RDI can request TF or AC. It is not a safe entry assumption.
  Program Bad({0x57, 0x9d, 0xb8, 0, 0, 0, 0, 0xc3});
  Bad.flagsProfile();
  expectRefusal(Bad, Status::ContractViolation);
  // A real mask makes every image safe while still allowing dynamic NT/ID.
  Program Good({0x48, 0x81, 0xe7, 0xff, 0xfe, 0xfb, 0xff, 0x57, 0x9d, 0xb8, 0,
                0, 0, 0, 0xc3});
  Good.flagsProfile();
  const auto Result = Good.check();
  ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
}

Program arbitraryFlagToSystem(unsigned Bit, bool Clear, bool Restore) {
  Program P({});
  if (Restore)
    P.append({0x9c, 0x5b}); // Keep original packed flags in RBX.
  // XOR makes AF arbitrary. Copy it through an actual flags stack snapshot.
  P.append({0x31, 0xc0, 0x9c, 0x58, 0x25});
  P.immediate(Clear ? 0 : 0x10);
  for (unsigned I = 4; I != Bit; ++I)
    P.append({0x01, 0xc0}); // add eax,eax: no shift metadata assumption.
  P.append({0x83, 0xc8, 2, 0x50, 0x9d});
  if (Restore)
    P.append({0x53, 0x9d});
  P.append({0xb8, 0, 0, 0, 0, 0xc3});
  P.flagsProfile();
  P.Contract.ObserveWrittenFrameBytes = false;
  return P;
}

TEST(OriginalBinaryUndefinedIndependence, PackedFlagsAlwaysObserveFinalSystem) {
  for (unsigned Bit : {14u, 21u}) {
    SCOPED_TRACE(Bit);
    auto Clear = arbitraryFlagToSystem(Bit, true, false);
    const auto Valid = Clear.check();
    ASSERT_TRUE(Valid.proved()) << Valid.Proof.Diagnostic;
    auto Bad = arbitraryFlagToSystem(Bit, false, false);
    const auto Result = Bad.check();
    EXPECT_EQ(Result.Proof.Status, Status::Dependent)
        << Result.Proof.Diagnostic;
    EXPECT_NE(Result.Proof.Diagnostic.find("final system flags"),
              std::string::npos);
    EXPECT_FALSE(Result.Certificate.has_value());
    expectRefusal(Bad, Status::Dependent);
    // No ordinary register or frame observation can turn off this obligation.
    Bad.Contract.ReturnRegisters.clear();
    expectRefusal(Bad, Status::Dependent);
    auto Restore = arbitraryFlagToSystem(Bit, false, true);
    const auto Restored = Restore.check();
    ASSERT_TRUE(Restored.proved()) << Restored.Proof.Diagnostic;
  }
}

TEST(OriginalBinaryUndefinedIndependence, PackedFlagsCheckBothUndefinedTwins) {
  for (unsigned Bit : {8u, 18u}) {
    SCOPED_TRACE(Bit);
    auto Good = arbitraryFlagToSystem(Bit, true, false);
    const auto Valid = Good.check();
    ASSERT_TRUE(Valid.proved()) << Valid.Proof.Diagnostic;
    auto Bad = arbitraryFlagToSystem(Bit, false, false);
    expectRefusal(Bad, Status::ContractViolation);
  }
}

TEST(OriginalBinaryUndefinedIndependence, PackedFlagsPreserveArbitraryCopies) {
  // xor eax,eax; pushfq; pop rax; and eax,0x10; ret exposes the same AF choice.
  Program Bad({0x31, 0xc0, 0x9c, 0x58, 0x25, 0x10, 0, 0, 0, 0xc3});
  Bad.flagsProfile();
  Bad.Contract.ObserveWrittenFrameBytes = false;
  expectRefusal(Bad, Status::Dependent);
  // Copying one saved image and XORing its copies cancels that one choice.
  Program Good(
      {0x31, 0xc0, 0x9c, 0x59, 0x48, 0x89, 0xc8, 0x48, 0x31, 0xc8, 0xc3});
  Good.flagsProfile();
  Good.Contract.ObserveWrittenFrameBytes = false;
  const auto Result = Good.check();
  ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
}

TEST(OriginalBinaryUndefinedIndependence, PackedFlagsPersistAcrossNativeCall) {
  // Set ID, call a helper RET, then prove the subsequent snapshot has ID set.
  Program P({0xb8, 2, 0, 0x20, 0, 0x50, 0x9d, 0xe8});
  const size_t Displacement = P.Image.Segments.front().Data.size();
  P.immediate(0);
  P.append({0x9c, 0x58, 0x25, 0, 0, 0x20, 0});
  P.requireEAX(0x200000);
  const size_t Helper = P.Image.Segments.front().Data.size();
  P.append({0xc3});
  const uint32_t Relative = Helper - (Displacement + 4);
  for (unsigned I = 0; I != 4; ++I)
    P.Image.Segments.front().Data[Displacement + I] = Relative >> (I * 8);
  P.flagsProfile();
  const auto Result = P.check();
  ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
}

TEST(OriginalBinaryUndefinedIndependence, PackedFlagsBindProfileAndChargeWork) {
  Program Plain({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Before = Plain.check();
  ASSERT_TRUE(Before.proved()) << Before.Proof.Diagnostic;
  Plain.flagsProfile();
  const auto After = Plain.check();
  ASSERT_TRUE(After.proved()) << After.Proof.Diagnostic;
  EXPECT_NE(Before.Certificate->InputDigest, After.Certificate->InputDigest);
  EXPECT_NE(Before.Certificate->LowIR.InputDigest,
            After.Certificate->LowIR.InputDigest);
  auto P = arbitraryFlagToSystem(21, true, false);
  const auto Valid = P.check();
  ASSERT_TRUE(Valid.proved()) << Valid.Proof.Diagnostic;
  LowIRIndependenceLimits Limits;
  Limits.MaxOperations = Valid.Proof.Operations - 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
  Limits = {};
  Limits.MaxSolverQueries = Valid.Proof.SolverQueries - 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
  Limits = {};
  Limits.MaxObservations = Valid.Proof.Observations - 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     PackedFlagsKeepSiblingStatesSeparate) {
  // Set ID before the fork. The fallthrough path clears it and finishes first;
  // the taken sibling must retain its own copy of the pre-fork ID state.
  Program P({0xb8, 2, 0, 0x20, 0, 0x50, 0x9d, 0x85, 0xff, 0x75, 0});
  P.append({0xb8, 2, 0, 0, 0, 0x50, 0x9d, 0x9c, 0x58, 0x25, 0, 0, 0x20, 0});
  P.requireEAX(0);
  const size_t Sibling = P.Image.Segments.front().Data.size();
  P.Image.Segments.front().Data[10] = Sibling - 11;
  P.append({0x9c, 0x58, 0x25, 0, 0, 0x20, 0});
  P.requireEAX(0x200000);
  P.flagsProfile();
  // TEST's arbitrary AF can remain in a stack snapshot on the taken path;
  // this case isolates the system-state fork and its trap-based oracle.
  P.Contract.ObserveWrittenFrameBytes = false;
  const auto Result = P.check();
  ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
  EXPECT_EQ(Result.Proof.Paths, 2U);
}

TEST(OriginalBinaryUndefinedIndependence,
     PackedFlagsCannotCertifyASafeSibling) {
  // test edi,edi; jne bad; mov eax,0; ret; bad: push 0x102; popfq; mov eax,0;
  // ret.
  Program P({0x85, 0xff, 0x75, 6, 0xb8, 0,    0, 0, 0, 0xc3, 0x68,
             2,    1,    0,    0, 0x9d, 0xb8, 0, 0, 0, 0,    0xc3});
  P.flagsProfile();
  const auto Result = P.check();
  EXPECT_EQ(Result.Proof.Paths, 1U);
  expectRefusal(P, Status::ContractViolation);
}

TEST(OriginalBinaryUndefinedIndependence, PackedFlagsKeepDistinctProducers) {
  // Save AF from two distinct XORs, then compare only those saved bits.
  Program P({0x31, 0xc0, 0x9c, 0x59, 0x31, 0xd2, 0x9c, 0x58, 0x48, 0x31, 0xc8,
             0x25, 0x10, 0, 0, 0, 0xc3});
  P.flagsProfile();
  P.Contract.ObserveWrittenFrameBytes = false;
  expectRefusal(P, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence, PackedFlagsDoNotAdmitOtherModes) {
  const std::initializer_list<uint8_t> Programs[] = {
      {0x66, 0x9c, 0xc3}, {0x66, 0x9d, 0xc3}, {0xfa, 0xc3}, {0xfb, 0xc3}};
  for (auto Bytes : Programs) {
    Program P(Bytes);
    P.flagsProfile();
    expectRefusal(P, Status::Unsupported);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     FiniteLoopKeepsPersistentPackedFlags) {
  // Set NT, repeat a snapshot oracle three times, and leave with RSP restored.
  Program P({0xb8, 2, 0x40, 0, 0, 0x50, 0x9d, 0xb9, 3, 0, 0, 0});
  const size_t Loop = P.Image.Segments.front().Data.size();
  P.append({0x9c, 0x58, 0x25, 0, 0x40, 0x20, 0, 0x3d, 0, 0x40, 0, 0, 0x75, 0});
  const size_t BadEdge = P.Image.Segments.front().Data.size() - 1;
  P.append({0x83, 0xe9, 1, 0x75, 0});
  auto &Bytes = P.Image.Segments.front().Data;
  Bytes.back() = static_cast<uint8_t>(Loop - Bytes.size());
  P.append({0xb8, 0, 0, 0, 0, 0xc3});
  const size_t Trap = P.Image.Segments.front().Data.size();
  P.append({0xcc});
  P.Image.Segments.front().Data[BadEdge] = Trap - (BadEdge + 1);
  P.flagsProfile();
  const auto Result = P.check();
  ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
  EXPECT_EQ(Result.Proof.Paths, 1U);
  EXPECT_EQ(Result.Certificate->LowIR.NativeFlagTransitions.size(), 4U);
  EXPECT_GT(Result.Certificate->LowIR.Instructions.size(),
            Result.Certificate->Instructions.size());
  LowIRIndependenceLimits Limits;
  Limits.MaxPaths = Result.Proof.BlockVisits - 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     FiniteLoopCoversAllBoundedInputCounts) {
  // ecx = edi & 3; eax = 0; while (ecx) { eax += 7; --ecx; } return eax.
  Program P({0x89, 0xf9, 0x83, 0xe1, 3, 0xb8, 0,    0, 0,    0,    0x85, 0xc9,
             0x74, 8,    0x83, 0xc0, 7, 0x83, 0xe9, 1, 0x75, 0xf8, 0xc3});
  const auto Result = P.check();
  ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
  EXPECT_EQ(Result.Proof.Paths, 4U);
  // A partial unrolling cannot claim that these are the only input cases.
  LowIRIndependenceLimits Limits;
  Limits.MaxPaths = Result.Proof.BlockVisits - 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     InfiniteLoopCannotPublishSafeSibling) {
  // The fallthrough returns first; another shared ordinary input spins forever.
  Program P({0x85, 0xff, 0x75, 6, 0xb8, 0, 0, 0, 0, 0xc3, 0xeb, 0xfe});
  LowIRIndependenceLimits Limits;
  Limits.MaxPaths = 12;
  const auto Result = P.check(Limits);
  EXPECT_EQ(Result.Proof.Paths, 1U);
  EXPECT_EQ(Result.Proof.Status, Status::BudgetExceeded);
  EXPECT_FALSE(Result.Certificate.has_value());
  expectRefusal(P, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     LoopControlChecksChoicesBeforePruning) {
  // xor eax,eax; lahf; test ah,0x10; jne start; ret.
  Program P({0x31, 0xc0, 0x9f, 0xf6, 0xc4, 0x10, 0x75, 0xf8, 0xc3});
  expectRefusal(P, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     DisabledShadowStackKeepsWholeRegisters) {
  for (unsigned Width : {4u, 8u})
    for (unsigned Index = 0; Index != 16; ++Index) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Index);
      Program P({0xf3});
      if (Width == 8 || Index >= 8)
        P.append(
            {static_cast<uint8_t>(0x40 | (Width == 8 ? 8 : 0) | (Index >> 3))});
      P.append({0x0f, 0x1e, static_cast<uint8_t>(0xc8 | (Index & 7)), 0xc3});
      P.flagsProfile();
      // In the disabled profile RDSSPD also preserves the upper 32 bits.
      const uint64_t Value =
          Index == 4 ? (uint64_t{1} << 40) | 0x10000 : 0x1234567887654321;
      const NdVar Register = NdVar::reg(Index * 8, 8);
      P.Options.EntryConstants.push_back({Register, Value});
      P.Contract.EntryConstants.push_back({Register, Value});
      P.Contract.PreservedRegisters.push_back({Register.Offset, 8});
      const auto Result = P.check();
      ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
      ASSERT_EQ(Result.Certificate->LowIR.NativeProfileProjections.size(), 1U);
      EXPECT_EQ(Result.Certificate->Instructions[0].ProfileProjection,
                InterpreterProfileProjection::CetDisabledReadShadowStackV1);
      EXPECT_EQ(Result.Certificate->Instructions[0].UndefinedEffects.Coverage,
                LowUndefinedCoverage::Missing);
      EXPECT_EQ(Result.Certificate->LowIR.Instructions[0].Effects.Coverage,
                LowUndefinedCoverage::Missing);
    }
  Program OtherCET({0xf3, 0x48, 0x0f, 0xae, 0xe8, 0xc3}); // incsspq rax.
  OtherCET.flagsProfile();
  expectRefusal(OtherCET, Status::ContractViolation);
}

TEST(OriginalBinaryUndefinedIndependence,
     DisabledShadowStackIncrementMustStayUnreachable) {
  for (unsigned Width : {4u, 8u})
    for (unsigned Index = 0; Index != 16; ++Index) {
      SCOPED_TRACE(Width);
      SCOPED_TRACE(Index);
      // cmp eax,eax; jne trap; ret; trap: incssp rN. There are deliberately
      // no bytes after the faulting instruction: it has no normal successor.
      Program P({0x39, 0xc0, 0x75, 1, 0xc3, 0xf3});
      if (Width == 8 || Index >= 8)
        P.append(
            {static_cast<uint8_t>(0x40 | (Width == 8 ? 8 : 0) | (Index >> 3))});
      P.append({0x0f, 0xae, static_cast<uint8_t>(0xe8 | (Index & 7))});
      expectRefusal(P, Status::Unsupported);
      P.flagsProfile();
      const auto Result = P.check();
      ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
      ASSERT_EQ(Result.Certificate->Instructions.size(), 4U);
      const auto &Trap = Result.Certificate->Instructions.back();
      EXPECT_EQ(Trap.Origin.Control, LowInstructionControl::Terminator);
      EXPECT_EQ(Trap.UndefinedEffects.Coverage, LowUndefinedCoverage::Missing);
      ASSERT_EQ(Trap.Ops.size(), 1U);
      EXPECT_EQ(Trap.Ops[0].Opcode, NdOp::INTRINSIC);
      ASSERT_EQ(Result.Certificate->LowIR.NativeProfileProjections.size(), 1U);
      const auto &Receipt =
          Result.Certificate->LowIR.NativeProfileProjections.front();
      EXPECT_EQ(Receipt.BlockId, -1);
      EXPECT_EQ(Receipt.InstructionAddress, Trap.Origin.Address);
      EXPECT_EQ(
          Receipt.Kind,
          InterpreterProfileProjection::CetDisabledIncrementShadowStackTrapV1);
      // Shared ordinary inputs may now reach #UD. The profile cannot assume
      // this arm away, even if the increment register is zero.
      P.Image.Segments[0].Data[1] = 0xc8; // cmp eax,ecx.
      expectRefusal(P, Status::ContractViolation);
    }
}

TEST(OriginalBinaryUndefinedIndependence,
     DisabledShadowStackTrapCannotHideBehindASafeSibling) {
  // test edi,edi; jne trap; mov eax,0; ret; trap: incsspq rax.
  Program P({0x85, 0xff, 0x75, 6, 0xb8, 0, 0, 0, 0, 0xc3, 0xf3, 0x48, 0x0f,
             0xae, 0xe8});
  P.flagsProfile();
  const auto Result = P.check();
  EXPECT_EQ(Result.Proof.Paths, 1U);
  expectRefusal(P, Status::ContractViolation);
  Program Zero({0x31, 0xc0, 0xf3, 0x48, 0x0f, 0xae, 0xe8});
  Zero.flagsProfile();
  expectRefusal(Zero, Status::ContractViolation);
}

TEST(OriginalBinaryUndefinedIndependence,
     ShiftFlagsAreArbitraryOnlyForTheirMaskedCounts) {
  for (unsigned Width : {1u, 2u, 4u, 8u})
    for (unsigned Group : {4u, 5u, 7u})
      for (unsigned Count : {0u, 1u, 2u, Width * 8 - 1, Width * 8,
                             Width * 8 + 1, 32u, 64u, 255u}) {
        SCOPED_TRACE(::testing::Message()
                     << Width << '/' << Group << '/' << Count);
        Program P({});
        if (Width == 2)
          P.append({0x66});
        if (Width == 8)
          P.append({0x48});
        P.append({uint8_t(Width == 1 ? 0xc0 : 0xc1), uint8_t(0xc0 | Group << 3),
                  uint8_t(Count), 0xc3});
        const unsigned Masked = Count & (Width == 8 ? 63 : 31);
        for (unsigned Flag : {x86reg::AF, x86reg::OF, x86reg::CF}) {
          SCOPED_TRACE(Flag);
          P.Contract.ReturnRegisters = {{Flag, 1}};
          const bool Undefined = Flag == x86reg::AF ? Masked != 0
                                 : Flag == x86reg::OF
                                     ? Masked > 1
                                     : Group != 7 && Masked >= Width * 8;
          const auto R = P.check();
          EXPECT_EQ(R.Proof.Status,
                    Undefined ? Status::Dependent : Status::Proved)
              << R.Proof.Diagnostic;
          EXPECT_EQ(R.Certificate.has_value(), !Undefined);
        }
      }
}

TEST(OriginalBinaryUndefinedIndependence,
     ShiftVariableCountRemainsSharedUntilItDependsOnAnArbitraryValue) {
  // shl rax,cl; ret. The destination is independent of fresh flags.
  Program Ordinary({0x48, 0xd3, 0xe0, 0xc3});
  EXPECT_TRUE(Ordinary.check().proved());
  Ordinary.Contract.ReturnRegisters = {{x86reg::OF, 1}};
  expectRefusal(Ordinary, Status::Dependent);

  // and ecx,1; shl eax,cl; ret. OF is defined or preserved for both counts.
  Program Bounded({0x83, 0xe1, 1, 0xd3, 0xe0, 0xc3});
  Bounded.Contract.ReturnRegisters = {{x86reg::OF, 1}};
  EXPECT_TRUE(Bounded.check().proved());

  // xor eax,eax; lahf; mov cl,ah; and ecx,16; shr ecx,4;
  // mov edx,1; add eax,0; shl edx,cl; mov eax,0; ret.
  // The saved arbitrary AF selects 0 or 1. ADD redefines the old OF to 0,
  // and both possible shifts also leave OF 0. EDX still depends on the count.
  Program Arbitrary({0x31, 0xc0, 0x9f, 0x88, 0xe1, 0x83, 0xe1, 16,   0xc1,
                     0xe9, 4,    0xba, 1,    0,    0,    0,    0x83, 0xc0,
                     0,    0xd3, 0xe2, 0xb8, 0,    0,    0,    0,    0xc3});
  Arbitrary.Contract.ReturnRegisters = {{x86reg::OF, 1}};
  const auto Safe = Arbitrary.check();
  ASSERT_TRUE(Safe.proved()) << Safe.Proof.Diagnostic;
  Arbitrary.Contract.ReturnRegisters = {{x86reg::RDX, 4}};
  expectRefusal(Arbitrary, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     ShiftCountSnapshotPrecedesOverlappingDestinationWrites) {
  // mov ecx,1; shl ecx,cl; ret. Re-reading CL would incorrectly make OF
  // arbitrary because the destination changes CL to 2.
  Program One({0xb9, 1, 0, 0, 0, 0xd3, 0xe1, 0xc3});
  One.Contract.ReturnRegisters = {{x86reg::OF, 1}};
  EXPECT_TRUE(One.check().proved());
  // shl cl,cl maps 8 to 0. A late guard must not hide its arbitrary CF/OF.
  Program Eight({0xb9, 8, 0, 0, 0, 0xd2, 0xe1, 0xc3});
  for (unsigned Flag : {x86reg::AF, x86reg::OF, x86reg::CF}) {
    Eight.Contract.ReturnRegisters = {{Flag, 1}};
    expectRefusal(Eight, Status::Dependent);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     ShiftUndefinedCopiesAndSpillsRetainTheirActualProducer) {
  // shl eax,2; seto dl; seto al; xor al,dl; movzx eax,al; ret.
  Program Shared({0xc1, 0xe0, 2, 0x0f, 0x90, 0xc2, 0x0f, 0x90, 0xc0, 0x30, 0xd0,
                  0x0f, 0xb6, 0xc0, 0xc3});
  const auto Result = Shared.check();
  ASSERT_TRUE(Result.proved()) << Result.Proof.Diagnostic;
  // An intervening shift creates a distinct OF production.
  Program Fresh({0xc1, 0xe0, 2, 0x0f, 0x90, 0xc2, 0xc1, 0xe0, 2, 0x0f, 0x90,
                 0xc0, 0x30, 0xd0, 0x0f, 0xb6, 0xc0, 0xc3});
  expectRefusal(Fresh, Status::Dependent);

  // Spill and reload the same OF, cancel it, and clear the observable spill.
  Program Spill({0xc1, 0xe0, 2,    0x0f, 0x90, 0xc2, 0x88, 0x54, 0x24,
                 0xf8, 0x8a, 0x44, 0x24, 0xf8, 0x30, 0xd0, 0xc6, 0x44,
                 0x24, 0xf8, 0,    0x0f, 0xb6, 0xc0, 0xc3});
  const auto Cleared = Spill.check();
  ASSERT_TRUE(Cleared.proved()) << Cleared.Proof.Diagnostic;
  // Replace only the clearing store with NOPs: the frame reveals the value.
  std::fill_n(Spill.Image.Segments[0].Data.begin() + 16, 5, 0x90);
  expectRefusal(Spill, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     RepeatedShiftVisitsProduceFreshUndefinedFlags) {
  // mov ecx,2; xor ebx,ebx; mov eax,0;
  // loop: shl eax,2; seto al; xor bl,al; dec ecx; jnz loop;
  // movzx eax,bl; ret. Reusing one OF producer for both visits falsely
  // proves that the two arbitrary bits cancel.
  Program P({0xb9, 2,    0,    0,    0,    0x31, 0xdb, 0xb8, 0,    0,
             0,    0,    0xc1, 0xe0, 2,    0x0f, 0x90, 0xc0, 0x30, 0xc3,
             0xff, 0xc9, 0x75, 0xf4, 0x0f, 0xb6, 0xc3, 0xc3});
  expectRefusal(P, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     ShiftUndefinedBranchCannotHideBehindEqualReturnValues) {
  // shl eax,2; jo second; mov eax,0; ret; second: mov eax,0; ret.
  Program P(
      {0xc1, 0xe0, 2, 0x70, 6, 0xb8, 0, 0, 0, 0, 0xc3, 0xb8, 0, 0, 0, 0, 0xc3});
  expectRefusal(P, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     ShiftGuardedProducersRemainSubjectToProofBudgets) {
  Program P({0xd2, 0xe0, 0xc3}); // shl al,cl; ret.
  const auto Complete = P.check();
  ASSERT_TRUE(Complete.proved()) << Complete.Proof.Diagnostic;
  EXPECT_EQ(Complete.Proof.Producers, 3u);
  LowIRIndependenceLimits Limit;
  Limit.MaxProducers = 2;
  expectRefusal(P, Status::BudgetExceeded, Limit);
  Limit = {};
  Limit.MaxOperations = 2;
  expectRefusal(P, Status::BudgetExceeded, Limit);
}

Program indexedImmutableLoad(uint64_t Base = 0x4000, unsigned Bytes = 4) {
  // and ecx,1; movabs rdx,base; mov eax,[rdx+rcx*4]; ret.
  Program P({0x83, 0xe1, 1, 0x48, 0xba});
  P.immediate(Base, 8);
  if (Bytes == 8)
    P.append({0x48});
  if (Bytes == 2)
    P.append({0x66});
  P.append({uint8_t(Bytes == 1 ? 0x8a : 0x8b), 0x04,
            uint8_t(Bytes == 8   ? 0xca
                    : Bytes == 4 ? 0x8a
                    : Bytes == 2 ? 0x4a
                                 : 0x0a)});
  if (Bytes < 4)
    P.append({0x0f, uint8_t(Bytes == 1 ? 0xb6 : 0xb7), 0xc0});
  P.append({0xc3});
  Segment Data;
  Data.Name = ".rodata";
  Data.VA = Base;
  Data.Flags = SegmentFlags::Readable;
  Data.Size = Data.FileSz = Bytes * 2;
  for (unsigned I = 0; I < Bytes * 2; ++I)
    Data.Data.push_back(uint8_t(13 + I * 17));
  P.Image.Segments.push_back(std::move(Data));
  return P;
}

TEST(OriginalBinaryUndefinedIndependence,
     FiniteImmutableLoadsRetainEveryAddressWidthAndSelectedValue) {
  for (unsigned Bytes : {1u, 2u, 4u, 8u}) {
    SCOPED_TRACE(Bytes);
    auto P = indexedImmutableLoad(0x4000, Bytes);
    auto &Code = P.Image.Segments[0];
    Code.Data.pop_back(); // Replace RET with an independent arithmetic check.
    // Select by an ordinary direct branch and compare each loaded value to
    // the independently computed table word. A constant-folded choice traps.
    uint64_t A = 0, B = 0;
    for (unsigned I = 0; I < Bytes; ++I) {
      A |= uint64_t(P.Image.Segments[1].Data[I]) << (I * 8);
      B |= uint64_t(P.Image.Segments[1].Data[Bytes + I]) << (I * 8);
    }
    P.append({0x85, 0xc9, 0x75, 18}); // test ecx,ecx; jnz second.
    P.append({0x48, 0xba});
    P.immediate(A, 8);
    P.append({0x48, 0x39, 0xd0, 0x75, 23, 0xc3, 0x90, 0x90});
    P.append({0x48, 0xba});
    P.immediate(B, 8);
    P.append({0x48, 0x39, 0xd0, 0x75, 5, 0xc3, 0x90, 0x90, 0x90, 0x90, 0xcc});
    const auto R = P.check();
    ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
    ASSERT_EQ(R.Certificate->Reads.size(), 2u);
    EXPECT_EQ(R.Proof.Paths, 2u);
    std::vector<va_t> Addresses;
    for (const auto &Read : R.Certificate->Reads) {
      Addresses.push_back(Read.Address);
      EXPECT_EQ(Read.Bytes.size(), Bytes);
      EXPECT_FALSE(Read.Evidence.empty());
    }
    std::sort(Addresses.begin(), Addresses.end());
    EXPECT_EQ(Addresses, (std::vector<va_t>{0x4000, 0x4000 + Bytes}));
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     FiniteImmutableLoadEvidenceAndLimitsBindCertificates) {
  auto P = indexedImmutableLoad();
  const auto A = P.check();
  ASSERT_TRUE(A.proved()) << A.Proof.Diagnostic;
  P.Image.Segments[1].Data[7] ^= 0x80;
  const auto B = P.check();
  ASSERT_TRUE(B.proved()) << B.Proof.Diagnostic;
  EXPECT_NE(A.Certificate->InputDigest, B.Certificate->InputDigest);
  LowIRIndependenceLimits Limits;
  Limits.MaxImmutableLoadAddresses = 2;
  const auto C = P.check(Limits);
  ASSERT_TRUE(C.proved()) << C.Proof.Diagnostic;
  EXPECT_NE(B.Certificate->InputDigest, C.Certificate->InputDigest);
  EXPECT_NE(B.Certificate->LowIR.InputDigest, C.Certificate->LowIR.InputDigest);
  Limits.MaxImmutableLoadAddresses = 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
  Limits.MaxImmutableLoadAddresses = 0;
  expectRefusal(P, Status::Invalid, Limits);
}

TEST(
    OriginalBinaryUndefinedIndependence,
    FiniteImmutableLoadRejectsAnyUnmappedWritableUnbackedOrRelocatedCandidate) {
  for (unsigned Change = 0; Change != 4; ++Change) {
    SCOPED_TRACE(Change);
    auto P = indexedImmutableLoad();
    auto &Data = P.Image.Segments[1];
    if (Change == 0) {
      Data.Size = Data.FileSz = 4;
      Data.Data.resize(4);
    } else if (Change == 1) {
      Data.Flags = Data.Flags | SegmentFlags::Writable;
    } else if (Change == 3) {
      P.Image.Relocations.push_back(
          {.Address = 0x4004, .Type = llvm::ELF::R_X86_64_64});
    } else {
      // The second range is mapped, but not backed by immutable file bytes.
      Data.FileSz = 4;
      Data.Data.resize(4);
    }
    expectRefusal(P, Status::Unsupported);
  }
}

TEST(OriginalBinaryUndefinedIndependence,
     ArbitraryLoadAddressIsRejectedBeforeEqualDataCanHideIt) {
  // Produce OF with SHL, use SETO as a one-bit table selector, and return
  // identical data from both addresses. Memory-address observation still fails.
  auto P = indexedImmutableLoad();
  const std::vector<uint8_t> Prefix{0xc1, 0xe0, 2,    0x0f, 0x90,
                                    0xc1, 0x0f, 0xb6, 0xc9};
  auto &Code = P.Image.Segments[0];
  Code.Data.erase(Code.Data.begin(), Code.Data.begin() + 3);
  Code.Data.insert(Code.Data.begin(), Prefix.begin(), Prefix.end());
  Code.Size = Code.FileSz = Code.Data.size();
  std::fill(P.Image.Segments[1].Data.begin(), P.Image.Segments[1].Data.end(),
            0);
  expectRefusal(P, Status::Dependent);
}

TEST(OriginalBinaryUndefinedIndependence,
     UnboundedImmutableLoadCannotUseAFinitePrefix) {
  auto P = indexedImmutableLoad();
  auto &Code = P.Image.Segments[0];
  std::fill_n(Code.Data.begin(), 3,
              0x90); // Remove the selector's one-bit mask.
  expectRefusal(P, Status::BudgetExceeded);
}

TEST(OriginalBinaryUndefinedIndependence,
     FiniteImmutableLoadEnumerationUsesTheCurrentPathPredicate) {
  auto P = indexedImmutableLoad();
  auto &Code = P.Image.Segments[0];
  std::vector<uint8_t> Body(Code.Data.begin() + 3, Code.Data.end());
  Code.Data.resize(3); // Keep the ordinary one-bit selector.
  P.append({0x85, 0xc9, 0x75, static_cast<uint8_t>(Body.size())});
  Code.Data.insert(Code.Data.end(), Body.begin(), Body.end());
  Code.Data.insert(Code.Data.end(), Body.begin(), Body.end());
  Code.Size = Code.FileSz = Code.Data.size();
  LowIRIndependenceLimits Limits;
  Limits.MaxImmutableLoadAddresses = 1;
  const auto R = P.check(Limits);
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  ASSERT_EQ(R.Certificate->Reads.size(), 2u);
  EXPECT_EQ(R.Proof.Paths, 2u);
  Limits.MaxSolverQueries = R.Proof.SolverQueries - 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
}

TEST(OriginalBinaryUndefinedIndependence,
     OpaquePreservationIsExplicitAndComplete) {
  Program P(
      {0x83, 0xf9, 0, 0x74, 6, 0xb8, 7, 0, 0, 0, 0xc3, 0xb8, 8, 0, 0, 0, 0xc3});
  const auto Baseline = P.check();
  ASSERT_TRUE(Baseline.proved()) << Baseline.Proof.Diagnostic;
  EXPECT_FALSE(Baseline.Certificate->LowIR.NativePreservation);
  P.Contract.NativePreservedState.emplace();
  const auto R = P.check();
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  ASSERT_TRUE(R.Certificate->LowIR.NativePreservation);
  const auto &F = *R.Certificate->LowIR.NativePreservation;
  EXPECT_EQ(F.Quantifier,
            LowIRNativePreservationQuantifier::AllUndefinedChoices);
  EXPECT_EQ(F.StateSet, LowPreservedStateSet::LegacyIntegerOpaqueV1);
  EXPECT_EQ(F.Instructions, R.Proof.Instructions);
  EXPECT_EQ(F.ExecutionDigest.size(), 64U);
  EXPECT_EQ(R.Proof.Operations, Baseline.Proof.Operations);
  EXPECT_EQ(R.Proof.SolverQueries, Baseline.Proof.SolverQueries);
  EXPECT_EQ(R.Proof.Observations, Baseline.Proof.Observations);
  EXPECT_EQ(R.Proof.Paths, 2U);
  EXPECT_NE(R.Certificate->InputDigest, Baseline.Certificate->InputDigest);
  for (const auto &I : R.Certificate->Instructions)
    EXPECT_TRUE(matchesLowPreservedState(I.PreservedState, I.Origin,
                                         I.NativeBytes, I.Ops));
  LowIRIndependenceLimits Short;
  Short.MaxOperations = R.Proof.Operations - 1;
  expectRefusal(P, Status::BudgetExceeded, Short);
  P.Contract.NativePreservedState->StateSet = LowPreservedStateSet::None;
  expectRefusal(P, Status::Invalid);
}

TEST(OriginalBinaryUndefinedIndependence,
     OpaquePreservationChargesEveryNativeMetadataSpan) {
  Program P({});
  for (unsigned I = 0; I != 16; ++I)
    P.append({0x90});
  P.append({0xc3});
  P.Contract.NativePreservedState.emplace();

  // Derive the metadata cost independently from freshly decoded spans. This
  // straight-line program stays below that ceiling during symbolic execution.
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  const auto &Bytes = P.Image.Segments.front().Data;
  uint64_t Operations = 0, Instructions = 0;
  for (size_t Offset = 0; Offset != Bytes.size();) {
    DecodedInsn I{};
    ASSERT_GT(D.decodeOneForLift(Bytes.data() + Offset, Bytes.size() - Offset,
                                 Entry + Offset, I),
              0);
    std::vector<LowOp> Ops;
    D.liftToLow(I, Ops);
    Operations += Ops.size();
    ++Instructions;
    Offset += I.Size;
  }
  ASSERT_EQ(Instructions, 17U);
  LowIRIndependenceLimits Exact;
  Exact.MaxOperations = 2 * Operations + Instructions;
  const auto Good = P.check(Exact);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_LT(Good.Proof.Operations, Exact.MaxOperations);
  --Exact.MaxOperations;
  const auto Short = P.check(Exact);
  EXPECT_EQ(Short.Proof.Status, Status::BudgetExceeded);
  EXPECT_EQ(Short.Proof.Diagnostic,
            "native preservation metadata budget exhausted");
  EXPECT_FALSE(Short.Certificate);
}

TEST(OriginalBinaryUndefinedIndependence,
     OpaquePreservationTraversesInternalCallees) {
  for (bool Indirect : {false, true}) {
    auto P = skippedContinuation(Indirect);
    P.Contract.NativePreservedState.emplace();
    const auto R = P.check();
    ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
    ASSERT_TRUE(R.Certificate->LowIR.NativePreservation);
    EXPECT_EQ(R.Certificate->LowIR.NativePreservation->Instructions,
              R.Proof.Instructions);
  }
  // CALL helper; RET; helper: PXOR XMM6,XMM6; RET.
  Program Bad({0xe8, 1, 0, 0, 0, 0xc3, 0x66, 0x0f, 0xef, 0xf6, 0xc3});
  Bad.Contract.NativePreservedState.emplace();
  Bad.Contract.RetainUnauditedNativeBoundaries = true;
  expectRefusal(Bad, Status::Unsupported);
}

TEST(OriginalBinaryUndefinedIndependence,
     OpaquePreservationKeepsProfileAuthoritySeparate) {
  Program P({0xf3, 0x48, 0x0f, 0x1e, 0xc8, 0xc3});
  P.flagsProfile();
  P.Contract.NativePreservedState.emplace();
  const auto R = P.check();
  ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
  const auto &Read = R.Certificate->Instructions.front();
  EXPECT_EQ(Read.PreservedState.Audit,
            LowPreservedStateAudit::CetDisabledReadShadowStackV1);
  EXPECT_EQ(Read.UndefinedEffects.Coverage, LowUndefinedCoverage::Missing);
  ASSERT_TRUE(R.Certificate->LowIR.NativePreservation);
  EXPECT_EQ(R.Certificate->LowIR.NativePreservation->Instructions, 2U);
  P.Options.X64CetDisabled = false;
  expectRefusal(P, Status::Unsupported);
  Program Trap({0xf3, 0x48, 0x0f, 0xae, 0xe8, 0xc3});
  Trap.flagsProfile();
  Trap.Contract.NativePreservedState.emplace();
  expectRefusal(Trap, Status::ContractViolation);
}

TEST(OriginalBinaryUndefinedIndependence,
     OpaquePreservationRequiresTheCompleteIndirectDestinationSet) {
  // AND EDI,1; ADD EDI,0x1010; JMP RDI; padding; RET; RET.
  Program P({0x83, 0xe7, 1, 0x81, 0xc7, 0x10, 0x10, 0, 0, 0xff, 0xe7, 0x90,
             0x90, 0x90, 0x90, 0x90, 0xc3, 0xc3});
  P.Contract.NativePreservedState.emplace();
  P.Contract.RetainUnauditedNativeBoundaries = true;
  LowIRIndependenceLimits Limits;
  Limits.MaxIndirectTargets = 2;
  const auto Good = P.check(Limits);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_TRUE(Good.Certificate->LowIR.NativePreservation);
  EXPECT_EQ(Good.Proof.Paths, 2U);
  Limits.MaxIndirectTargets = 1;
  expectRefusal(P, Status::BudgetExceeded, Limits);
  P.Image.Segments.front().Data.pop_back();
  P.append({0x66, 0x0f, 0xef, 0xf6, 0xc3}); // second: PXOR XMM6,XMM6; RET.
  Limits.MaxIndirectTargets = 2;
  expectRefusal(P, Status::Unsupported, Limits);
}

} // namespace
