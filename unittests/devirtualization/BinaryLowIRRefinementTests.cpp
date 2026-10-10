//===- BinaryLowIRRefinementTests.cpp - Original-to-residual relations
//-----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../../lib/analysis/core/NativeUndefinedIndependence.h"
#include "gtest/gtest.h"

#include "neverd/analysis/BinaryInterpreterSpecialization.h"
#include "neverd/decode/Decoder.h"
#include "neverd/lift/X86Regs.h"

#include <algorithm>
#include <bit>
#include <set>

using namespace neverd;
using namespace neverd::analysis;

namespace {
using Status = LowIRRefinementStatus;
using Witness = LowIRRefinementWitness;
constexpr va_t Entry = 0x1000;

struct Program {
  BinaryImage Image;
  SpecializationOptions Options;
  LowIRIndependenceContract Contract;

  Program(std::initializer_list<uint8_t> Bytes) {
    Image.Arch = Arch::X64;
    Image.Bits = Bitness::Bits64;
    Image.Format = BinaryFormat::ELF;
    Image.ExceptionMetadata.ParseStatus = ExceptionParseStatus::Complete;
    Segment Code;
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
    Contract.Frame = LowIRIndependenceFrame{{x86reg::RSP, 8}, -64, 8};
    for (unsigned I = 0; I != 16; ++I)
      Contract.ReturnRegisters.push_back({I * 8, 8});
    for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                      x86reg::SF, x86reg::OF, x86reg::DF})
      Contract.ReturnRegisters.push_back({Flag, 1});
  }

  SpecializationResult recover() const {
    return specializeBinaryInterpreter(Image, Entry, Options);
  }
  BinaryLowIRRefinementResult
  check(const LowFunc &Candidate, Witness W = Witness::LiftedBits,
        const LowIRRefinementLimits &Limits = {}) const {
    return checkBinaryLowIRRefinement(Image, Entry, Options, Candidate,
                                      Contract, W, Limits);
  }
};

void refused(const BinaryLowIRRefinementResult &Result, Status S) {
  EXPECT_EQ(Result.Proof.Status, S) << Result.Proof.Diagnostic;
  EXPECT_FALSE(Result.proved());
  EXPECT_FALSE(Result.Certificate);
  EXPECT_FALSE(Result.Proof.Certificate);
}

Program repeatedNativeContexts() {
  Program P({0x49, 0x89, 0xfa, 0x41, 0x83, 0xe2, 1,    0x45, 0x85,
             0xd2, 0x74, 2,    0xeb, 0,    0xe3, 9,    0x48, 0x01,
             0xc8, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf5, 0xc3});
  P.Options.ControlRegisters = {{x86reg::R10, 8}};
  return P;
}

Program alternatingNativeContexts(bool Frame, bool Byte, bool Reverse,
                                  bool MirrorFrame = false) {
  Program P({});
  auto &Code = P.Image.Segments.front();
  const auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Code.Data.insert(Code.Data.end(), Bytes);
  };
  // The entry phase remains a function of an arbitrary input. Branching on
  // it lets recovery specialize two contexts of the same native loop.
  Emit({0x49, 0x89, 0xfa}); // mov r10,rdi
  if (Byte)
    Emit({0x41, 0x80, 0xe2, 1}); // and r10b,1
  else
    Emit({0x49, 0x83, 0xe2, 1}); // and r10,1
  const auto Toggle = [&](bool InFrame) {
    if (InFrame && Byte)
      Emit({0x80, 0x74, 0x24, 0xfb, 1}); // xor byte [rsp-5],1
    else if (InFrame)
      Emit({0x48, 0x83, 0x74, 0x24, 0xf8, 1});
    else if (Byte)
      Emit({0x41, 0x80, 0xf2, 1}); // xor r10b,1
    else
      Emit({0x49, 0x83, 0xf2, 1}); // xor r10,1
  };
  if (Reverse)
    Toggle(false);
  Emit({0x45, 0x84, 0xd2, 0x74, 2, 0xeb, 0}); // test r10b,r10b; split
  if (Frame) {
    if (Byte)
      Emit({0x44, 0x88, 0x54, 0x24, 0xfb});
    else
      Emit({0x4c, 0x89, 0x54, 0x24, 0xf8});
    if (!MirrorFrame)
      Emit({0x45, 0x31, 0xd2}); // Only the frame retains the phase.
  }
  Emit({0x39, 0xff}); // Normalize flags on every arrival.
  const auto Header = Code.Data.size();
  Emit({0xe3, 0}); // jrcxz done
  Emit({0x48, 0x01, 0xc8, 0x48, 0x8d, 0x49, 0xff});
  Toggle(Frame);
  if (MirrorFrame)
    Toggle(false);
  Emit({0x39, 0xff});
  const auto Backedge = Code.Data.size();
  Emit({0xeb, static_cast<uint8_t>(Header - (Backedge + 2))});
  Code.Data[Header + 1] = static_cast<uint8_t>(Code.Data.size() - (Header + 2));
  Emit({0xc3});
  Code.Size = Code.FileSz = Code.Data.size();
  const uint16_t Width = Byte ? 1 : 8;
  if (Frame)
    P.Options.ControlFrameSlots = {{Byte ? -5 : -8, Width}};
  if (!Frame || MirrorFrame)
    P.Options.ControlRegisters = {{x86reg::R10, Width}};
  P.Contract.ObserveWrittenFrameBytes = true;
  return P;
}

Program repeatedNativeFrameContexts() {
  // Only the frame separates two contexts after clearing the input register
  // and normalizing arithmetic flags. The loop preserves each phase.
  Program P({0x83, 0xe7, 1,    0x85, 0xff, 0x74, 11,   0x48, 0xc7, 0x44, 0x24,
             0xf8, 1,    0,    0,    0,    0xeb, 9,    0x48, 0xc7, 0x44, 0x24,
             0xf8, 0,    0,    0,    0,    0x31, 0xff, 0x39, 0xff, 0xe3, 9,
             0x48, 0x01, 0xc8, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf5, 0xc3});
  P.Options.ControlFrameSlots = {{-8, 8}};
  return P;
}

struct SingleNativeContext {
  Program Input;
  unsigned Shared = 0, Update = 0;

  SingleNativeContext(bool Frame, bool Byte, bool Reverse) : Input({}) {
    auto &Code = Input.Image.Segments.front();
    const auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
      Code.Data.insert(Code.Data.end(), Bytes);
    };
    const uint8_t Initial = Reverse ? 1 : 0, Steady = Initial ^ 1;
    const auto Store = [&](uint8_t Value) {
      if (Frame && Byte)
        Emit({0xc6, 0x44, 0x24, 0xfb, Value}); // movb [rsp-5], imm8
      else if (Frame)
        Emit({0x48, 0xc7, 0x44, 0x24, 0xf8, Value, 0, 0, 0});
      else if (Byte)
        Emit({0x41, 0xb2, Value}); // mov r10b, imm8
      else
        Emit({0x49, 0xc7, 0xc2, Value, 0, 0, 0});
    };
    const auto Jump = [&] {
      const auto Address = Code.Data.size();
      Emit({0xeb, static_cast<uint8_t>(Shared - (Address + 2))});
    };
    Store(Initial);
    Emit({0x48, 0x89, 0xf9, 0x39, 0xff, 0xeb, 0}); // rcx=rdi; cmp edi,edi
    Shared = Code.Data.size();
    if (Frame && Byte)
      Emit({0x80, 0x7c, 0x24, 0xfb, Steady});
    else if (Frame)
      Emit({0x48, 0x83, 0x7c, 0x24, 0xf8, Steady});
    else if (Byte)
      Emit({0x41, 0x80, 0xfa, Steady});
    else
      Emit({0x49, 0x83, 0xfa, Steady});
    const auto Enter = Code.Data.size();
    Emit({0x74, 0}); // je loop
    Store(Steady);
    Emit({0x39, 0xff}); // Match entry flags; only the stored phase differs.
    Jump();
    Code.Data[Enter + 1] = static_cast<uint8_t>(Code.Data.size() - (Enter + 2));
    const auto Exit = Code.Data.size();
    Emit({0xe3, 0}); // jrcxz done
    Update = Code.Data.size();
    Emit({0x48, 0x01, 0xc8, 0x48, 0x8d, 0x49, 0xff, 0x39, 0xff});
    Jump();
    Code.Data[Exit + 1] = static_cast<uint8_t>(Code.Data.size() - (Exit + 2));
    Emit({0xc3});
    Code.Size = Code.FileSz = Code.Data.size();
    const uint16_t Width = Byte ? 1 : 8;
    if (Frame)
      Input.Options.ControlFrameSlots = {{Byte ? -5 : -8, Width}};
    else
      Input.Options.ControlRegisters = {{x86reg::R10, Width}};
    Input.Contract.ObserveWrittenFrameBytes = true;
  }
};

Program guardedNativeChain(unsigned Count, bool Taken) {
  Program P({});
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {128, 8};
  auto &Code = P.Image.Segments.front();
  const auto Immediate = [&](uint32_t Value) {
    for (unsigned I = 0; I != 4; ++I)
      Code.Data.push_back(static_cast<uint8_t>(Value >> (8 * I)));
  };
  for (unsigned I = 0; I != Count; ++I) {
    // LEA RAX,[RSP+bias]; AND EAX,127; CMP EAX,entry-residue+bias.
    const uint8_t Bias = I + 1, Expected = (8 + Bias) & 127;
    Code.Data.insert(Code.Data.end(), {0x48, 0x8d, 0x44, 0x24, Bias, 0x83, 0xe0,
                                       127, 0x83, 0xf8, Expected, 0x0f});
    Code.Data.push_back(Taken ? 0x84 : 0x85);
    if (Taken) {
      Immediate(1); // JE next skips a trapping fallthrough.
      Code.Data.push_back(0xcc);
    } else {
      // JNE trap, after the final MOV EAX,7; RET.
      Immediate(Count * 17 + 6 - (Code.Data.size() + 4));
    }
  }
  Code.Data.insert(Code.Data.end(), {0xb8, 7, 0, 0, 0, 0xc3, 0xcc});
  Code.Size = Code.FileSz = Code.Data.size();
  return P;
}

TEST(BinaryLowIRRefinement, ProvedNativeBranchChoiceKeepsTheIncomingDomain) {
  for (bool Taken : {false, true}) {
    SCOPED_TRACE(Taken);
    auto P = guardedNativeChain(32, Taken);
    auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    LowIRRefinementLimits Limits;
    Limits.Execution.Solver.Blast.MaxGates = 512;
    const auto Good = P.check(Recovery.Residual, Witness::LiftedBits, Limits);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_EQ(Good.Proof.OriginalPaths, 1U);
    EXPECT_EQ(Good.Proof.CandidatePaths, 1U);
    EXPECT_EQ(Good.Proof.TerminalPairs, 1U);
    ASSERT_GT(Good.Proof.SolverQueries, 0U);
    Limits.Execution.MaxSolverQueries = Good.Proof.SolverQueries;
    ASSERT_TRUE(
        P.check(Recovery.Residual, Witness::LiftedBits, Limits).proved());
    --Limits.Execution.MaxSolverQueries;
    refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
            Status::BudgetExceeded);
    Limits.Execution.MaxSolverQueries = Good.Proof.SolverQueries;
    Limits.Execution.Solver.Blast.MaxGates = 1;
    refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
            Status::BudgetExceeded);
    Limits.Execution.Solver.Blast.MaxGates = 512;

    P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {128, 9};
    refused(P.check(Recovery.Residual), Status::ContractViolation);
    P.Options.EntryFrameAlignment.reset();
    P.Contract.Frame->EntryAlignment.reset();
    refused(P.check(Recovery.Residual), Status::ContractViolation);
    P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {128, 8};
    P.Image.Segments.front().Data[10] ^= 1;
    refused(P.check(Recovery.Residual), Status::ContractViolation);
    P.Image.Segments.front().Data[10] ^= 1;

    bool Changed = false;
    for (auto &B : Recovery.Residual.Blocks)
      for (auto &O : B.Ops)
        if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
            O.Output.isReg() && O.Output.Offset == x86reg::RAX &&
            O.NumInputs == 1 && O.Inputs[0] == NdVar::scalar(7, 4)) {
          O.Inputs[0].Offset = 9;
          Changed = true;
        }
    ASSERT_TRUE(Changed);
    // A different terminal observation must still be checked after coverage.
    refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
            Status::Different);
  }
}

TEST(BinaryLowIRRefinement, NativeBranchEncodingsHaveBoundedLifetimes) {
  for (bool Taken : {false, true}) {
    SCOPED_TRACE(Taken);
    auto P = guardedNativeChain(48, Taken);
    const auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    LowIRRefinementLimits Limits;
    // Each branch fits this search budget. Retaining unrelated earlier
    // branch encodings makes later queries run out of propagations.
    Limits.Execution.Solver.Sat.MaxPropagations = 512;
    const auto Good = P.check(Recovery.Residual, Witness::LiftedBits, Limits);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_EQ(Good.Proof.OriginalPaths, 1U);
    EXPECT_EQ(Good.Proof.CandidatePaths, 1U);
    EXPECT_EQ(Good.Proof.TerminalPairs, 1U);
    ASSERT_GT(Good.Proof.SolverQueries, 0U);
    Limits.Execution.MaxSolverQueries = Good.Proof.SolverQueries;
    ASSERT_TRUE(
        P.check(Recovery.Residual, Witness::LiftedBits, Limits).proved());
    --Limits.Execution.MaxSolverQueries;
    refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
            Status::BudgetExceeded);
    Limits.Execution.MaxSolverQueries = Good.Proof.SolverQueries;

    auto Bad = Recovery.Residual;
    bool Changed = false;
    for (auto &B : Bad.Blocks)
      for (auto &O : B.Ops)
        if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
            O.Output.isReg() && O.Output.Offset == x86reg::RAX &&
            O.NumInputs == 1 && O.Inputs[0] == NdVar::scalar(7, 4)) {
          O.Inputs[0].Offset = 9;
          Changed = true;
        }
    ASSERT_TRUE(Changed);
    refused(P.check(Bad, Witness::LiftedBits, Limits), Status::Different);
  }
}

TEST(BinaryLowIRRefinement, InterveningQueriesRetainFeasibleDomains) {
  for (unsigned Count : {8U, 32U}) {
    for (bool Taken : {false, true}) {
      SCOPED_TRACE(Count);
      SCOPED_TRACE(Taken);
      auto P = guardedNativeChain(Count, Taken);
      auto &Code = P.Image.Segments.front();
      // LEA RDI,[RDI+RSI*4]; XOR RDI,RSI; TEST RDI,RDI; JE alternate.
      // Each arm has a data-dependent domain. Proved branches in the chain
      // interleave UNSAT obligations with repeated feasibility checks.
      std::vector<uint8_t> Bytes{0x48, 0x8d, 0x3c, 0xb7, 0x48, 0x31,
                                 0xf7, 0x48, 0x85, 0xff, 0x0f, 0x84};
      const uint32_t Skip = Code.Data.size();
      for (unsigned I = 0; I != 4; ++I)
        Bytes.push_back(static_cast<uint8_t>(Skip >> (8 * I)));
      Bytes.insert(Bytes.end(), Code.Data.begin(), Code.Data.end());
      Bytes.insert(Bytes.end(), {0xb8, 11, 0, 0, 0, 0xc3});
      Code.Data = std::move(Bytes);
      Code.Size = Code.FileSz = Code.Data.size();
      P.Contract.ObserveWrittenFrameBytes = true;
      const auto Recovery = P.recover();
      ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
      const auto Good = P.check(Recovery.Residual);
      ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
      // These independent fixtures retain their full logical query costs
      // even when an intervening complete answer avoids backend work.
      EXPECT_EQ(Good.Proof.SolverQueries,
                Count == 8 ? (Taken ? 37u : 29u) : (Taken ? 109u : 77u));
      ASSERT_GT(Good.Proof.SolverQueries, 0U);
      LowIRRefinementLimits Limits;
      Limits.Execution.MaxSolverQueries = Good.Proof.SolverQueries;
      ASSERT_TRUE(
          P.check(Recovery.Residual, Witness::LiftedBits, Limits).proved());
      --Limits.Execution.MaxSolverQueries;
      refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
              Status::BudgetExceeded);
      Limits.Execution.MaxSolverQueries = Good.Proof.SolverQueries;
      Limits.Execution.Solver.Blast.MaxGates = 1;
      refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
              Status::BudgetExceeded);
      for (unsigned Value : {7U, 11U}) {
        auto Bad = Recovery.Residual;
        bool Changed = false;
        for (auto &B : Bad.Blocks)
          for (auto &O : B.Ops)
            if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
                O.Output.isReg() && O.Output.Offset == x86reg::RAX &&
                O.NumInputs == 1 && O.Inputs[0].isConst() &&
                O.Inputs[0].Offset == Value) {
              O.Inputs[0].Offset += 2;
              Changed = true;
            }
        ASSERT_TRUE(Changed);
        // Both terminal observations still need independent equality proofs.
        refused(P.check(Bad), Status::Different);
      }
    }
  }
}

TEST(BinaryLowIRRefinement, SingletonTargetsRetainTheirBranchDomains) {
  // TEST EDI,1; JNZ odd. Each arm computes a distinct singleton target from
  // the symbolic entry stack, then returns a distinct ECX value.
  Program P({0xf7, 0xc7, 1, 0, 0, 0, 0x75, 16});
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {16, 8};
  auto &Code = P.Image.Segments.front();
  for (uint8_t Bias : {1, 2}) {
    const uint32_t Target = Entry + 40 + (Bias - 1) * 6;
    const uint32_t Base = Target - (8 + Bias);
    Code.Data.insert(Code.Data.end(), {0x48, 0x8d, 0x44, 0x24, Bias, 0x83, 0xe0,
                                       15, 0x48, 0x05});
    for (unsigned I = 0; I != 4; ++I)
      Code.Data.push_back(static_cast<uint8_t>(Base >> (8 * I)));
    Code.Data.insert(Code.Data.end(), {0xff, 0xe0});
  }
  Code.Data.insert(Code.Data.end(),
                   {0xb9, 7, 0, 0, 0, 0xc3, 0xb9, 9, 0, 0, 0, 0xc3});
  Code.Size = Code.FileSz = Code.Data.size();
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxIndirectTargets = 1;
  const auto Good = P.check(Recovery.Residual, Witness::LiftedBits, Limits);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.OriginalPaths, 2U);
  EXPECT_EQ(Good.Proof.CandidatePaths, 2U);
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
          O.Output.isReg() && O.Output.Offset == x86reg::RCX &&
          O.NumInputs == 1 && O.Inputs[0] == NdVar::scalar(7, 4)) {
        O.Inputs[0].Offset = 9;
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
          Status::Different);
}

Program indirectNativeChain(unsigned Count, bool Frame, bool Split) {
  Program P({});
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {128, 8};
  P.Contract.ObserveWrittenFrameBytes = true;
  auto &Code = P.Image.Segments.front();
  const auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Code.Data.insert(Code.Data.end(), Bytes);
  };
  const auto Immediate = [&](uint32_t Value) {
    for (unsigned I = 0; I != 4; ++I)
      Code.Data.push_back(static_cast<uint8_t>(Value >> (8 * I)));
  };
  if (Split) {
    Emit({0xf7, 0xc7, 1, 0, 0, 0, 0x0f, 0x85});
    Immediate(0);
  }
  for (unsigned Arm = 0; Arm != (Split ? 2U : 1U); ++Arm) {
    if (Arm) {
      const uint32_t Offset = Code.Data.size() - 12;
      for (unsigned I = 0; I != 4; ++I)
        Code.Data[8 + I] = static_cast<uint8_t>(Offset >> (8 * I));
    }
    for (unsigned I = 0; I != Count; ++I) {
      const uint8_t Bias = I + 1;
      if (Frame) {
        // LEA RDX,[RSP+bias]; AND RDX,-128; MOV [RDX],RAX.
        // The full entry residue proves that every store addresses RSP-8.
        Emit({0x48, 0x8d, 0x54, 0x24, Bias, 0x48, 0x83, 0xe2, 0x80, 0x48, 0x89,
              0x02});
      }
      const uint32_t Target = Entry + Code.Data.size() + 16;
      Emit({0x48, 0x8d, 0x44, 0x24, Bias, 0x83, 0xe0, 127, 0x48, 0x05});
      Immediate(Target - ((8 + Bias) & 127));
      Emit({0xff, 0xe0});
    }
    Emit({0xb9});
    Immediate(Arm ? 11 : 7);
    Emit({0xc3});
  }
  Code.Size = Code.FileSz = Code.Data.size();
  return P;
}

TEST(BinaryLowIRRefinement, NativeTargetDomainsKeepIndependentProjections) {
  for (unsigned Count : {8U, 12U})
    for (bool Frame : {false, true})
      for (bool Split : {false, true}) {
        SCOPED_TRACE(Count);
        SCOPED_TRACE(Frame);
        SCOPED_TRACE(Split);
        auto P = indirectNativeChain(Count, Frame, Split);
        const auto Recovery = P.recover();
        ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
        LowIRRefinementLimits Limits;
        Limits.Execution.MaxIndirectTargets = 1;
        const auto Good =
            P.check(Recovery.Residual, Witness::LiftedBits, Limits);
        ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
        const auto &Q = Good.Proof;
        // Independent baseline costs include every final exclusion query.
        const unsigned SingleQueries = Frame ? 6 * Count + 13 : 2 * Count + 5;
        EXPECT_EQ(Q.SolverQueries,
                  Split ? 2 * SingleQueries + 6 : SingleQueries);
        EXPECT_EQ(Q.OriginalPaths, Split ? 2U : 1U);
        EXPECT_EQ(Q.CandidatePaths, Split ? 2U : 1U);
        Limits.Execution.MaxSolverQueries = Q.SolverQueries;
        ASSERT_TRUE(
            P.check(Recovery.Residual, Witness::LiftedBits, Limits).proved());
        --Limits.Execution.MaxSolverQueries;
        refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
                Status::BudgetExceeded);
        Limits.Execution.MaxSolverQueries = Q.SolverQueries;
        Limits.Execution.MaxIndirectTargets = 0;
        refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
                Status::Invalid);
        Limits.Execution.MaxIndirectTargets = 1;
        Limits.Execution.Solver.Blast.MaxGates = 1;
        refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
                Status::BudgetExceeded);
        for (unsigned V : {7U, 11U}) {
          if (V == 11 && !Split)
            continue;
          auto Bad = Recovery.Residual;
          bool Changed = false;
          for (auto &B : Bad.Blocks)
            for (auto &O : B.Ops)
              if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
                  O.Output.isReg() && O.Output.Offset == x86reg::RCX &&
                  O.NumInputs == 1 && O.Inputs[0].isConst() &&
                  O.Inputs[0].Offset == V) {
                O.Inputs[0].Offset += 2;
                Changed = true;
              }
          ASSERT_TRUE(Changed);
          refused(P.check(Bad), Status::Different);
        }
      }
}

TEST(BinaryLowIRRefinement, MemoryIndirectDispatchKeepsTargetSnapshot) {
  // AND ECX,3; LEA RAX,[RIP+table]; JMP [RAX+RCX*8]; four return arms.
  Program P({0x83, 0xe1, 3, 0x48, 0x8d, 0x05, 27,   0, 0, 0, 0xff, 0x24, 0xc8,
             0xb8, 7,    0, 0,    0,    0xc3, 0xb8, 9, 0, 0, 0,    0xc3, 0xb8,
             11,   0,    0, 0,    0xc3, 0xb8, 13,   0, 0, 0, 0xc3});
  auto &Code = P.Image.Segments.front();
  ASSERT_EQ(Code.Data.size(), 37U);
  for (uint64_t Target : {Entry + 13, Entry + 19, Entry + 25, Entry + 31})
    for (unsigned I = 0; I != 8; ++I)
      Code.Data.push_back(static_cast<uint8_t>(Target >> (I * 8)));
  Code.Size = Code.FileSz = Code.Data.size();
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.OriginalPaths, 4U);
  EXPECT_EQ(Good.Proof.CandidatePaths, 4U);
  auto Changed = Recovery.Residual;
  ASSERT_EQ(Changed.FunctionTemporaries.size(), 1U);
  bool Mutated = false;
  for (auto &B : Changed.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::COPY && O.Output.isTemp() &&
          O.Output.Offset == Changed.FunctionTemporaries[0].Offset) {
        O.Inputs[0] = NdVar::scalar(Entry + 13, 8);
        Mutated = true;
      }
  ASSERT_TRUE(Mutated);
  refused(P.check(Changed), Status::Different);
  Changed = Recovery.Residual;
  Changed.FunctionTemporaries.clear();
  refused(P.check(Changed), Status::Invalid);
}

TEST(BinaryLowIRRefinement, FiniteReturnDispatchKeepsPreCleanupTarget) {
  // Reserve a cleanup slot and call a helper. It replaces its return slot
  // with one of four arms, then RET 8 restores the outer stack pointer.
  Program P({0x48, 0x83, 0xec, 8,    0xe8, 24,   0,    0,    0,    0xb8,
             7,    0,    0,    0,    0xc3, 0xb8, 9,    0,    0,    0,
             0xc3, 0xb8, 11,   0,    0,    0,    0xc3, 0xb8, 13,   0,
             0,    0,    0xc3, 0x83, 0xe1, 3,    0x48, 0x8d, 0x05, 0xde,
             0xff, 0xff, 0xff, 0x48, 0x8d, 0x0c, 0x49, 0x48, 0x8d, 0x04,
             0x48, 0x48, 0x89, 0x04, 0x24, 0xc2, 8,    0});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.OriginalPaths, 4U);
  EXPECT_EQ(Good.Proof.CandidatePaths, 4U);
}

TEST(BinaryLowIRRefinement, FiniteMemoryCallKeepsOverwrittenTargetSlot) {
  // Select a callee into [RSP-8]. CALL reads it before pushing the return
  // address into that same slot, so a later reload is not a target snapshot.
  Program P({0x83, 0xe1, 1,    0x48, 0x8d, 0x05, 18,   0,    0,    0,
             0x48, 0x8d, 0x0c, 0x49, 0x48, 0x8d, 0x04, 0x48, 0x48, 0x89,
             0x44, 0x24, 0xf8, 0xff, 0x54, 0x24, 0xf8, 0xc3, 0xb8, 7,
             0,    0,    0,    0xc3, 0xb8, 9,    0,    0,    0,    0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.OriginalPaths, 2U);
  EXPECT_EQ(Good.Proof.CandidatePaths, 2U);
}

TEST(BinaryLowIRRefinement,
     Disp32WordShiftChecksTheCompleteRecoveredCandidate) {
  Program P({0x48, 0x89, 0x4c, 0x24, 0xf8, 0x66, 0xc1, 0xa4, 0x24, 0xf8,
             0xff, 0xff, 0xff, 3,    0x0f, 0xb7, 0x44, 0x24, 0xf8, 0xc3});
  auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto Good = P.check(R.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  bool Changed = false;
  for (auto &B : R.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::INT_LEFT && O.Output.Size == 2 &&
          O.NumInputs == 2 && O.Inputs[1] == NdVar::scalar(3, 2)) {
        O.Inputs[1] = NdVar::scalar(2, 2);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(R.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, PhysicalReturnDestinationsKeepFullStateRelation) {
  // The helper adjusts its return slot to skip two invalid inline bytes.
  Program P({0xe8, 8, 0, 0, 0, 0x16, 0x06, 0xb8, 7, 0, 0, 0, 0xc3, 0x48, 0x83,
             0x04, 0x24, 2, 0xc3});
  auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  const auto Good = P.check(R.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  bool Changed = false;
  for (auto &B : R.Residual.Blocks)
    for (auto &O : B.Ops)
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
          O.Output.isReg() && O.Output.Offset == x86reg::RAX &&
          O.NumInputs == 1 && O.Inputs[0].isConst() &&
          O.Inputs[0].Offset == 7) {
        O.Inputs[0].Offset = 9;
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(R.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, DeferredConditionalEdgesKeepFullStateRelation) {
  // CMP EAX,EAX; JE suffix; invalid bytes; suffix: MOV EAX,7; RET.
  Program P({0x39, 0xc0, 0x74, 2, 0x16, 0x06, 0xb8, 7, 0, 0, 0, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  refused(P.check(Recovery.Residual), Status::Unsupported);
  P.Contract.DeferNativeConditionalEdges = true;
  for (auto W : {Witness::LiftedBits, Witness::ZeroBits}) {
    const auto Good = P.check(Recovery.Residual, W);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_TRUE(
        Good.Certificate->Relation.Contract.DeferNativeConditionalEdges);
    EXPECT_EQ(Good.Certificate->Instructions.size(), 4U);
    EXPECT_TRUE(Good.Certificate->Relation.NativeAuditBoundaries.empty());
  }
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
          O.Output.isReg() && O.Output.Offset == x86reg::RAX &&
          O.NumInputs == 1 && O.Inputs[0] == NdVar::scalar(7, 4)) {
        O.Inputs[0].Offset = 9;
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, DeferredEdgesRespectSelectedUndefinedWitness) {
  // ADD establishes OF=1; BT makes it arbitrary, keeping the old lifted bit.
  // Only ZeroBits takes JNO into the invalid byte. Independence must refuse.
  Program P({0xb8, 0xff, 0xff, 0xff, 0x7f, 0x83, 0xc0, 1, 0x0f, 0xa3,
             0xc8, 0x71, 6,    0xb8, 7,    0,    0,    0, 0xc3, 0x16});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  P.Contract.DeferNativeConditionalEdges = true;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  refused(P.check(Recovery.Residual, Witness::ZeroBits), Status::Unsupported);
  const auto Independent =
      checkBinaryUndefinedIndependence(P.Image, Entry, P.Options, P.Contract);
  EXPECT_EQ(Independent.Proof.Status, LowIRIndependenceStatus::Dependent);
  EXPECT_FALSE(Independent.Certificate);
}

TEST(BinaryLowIRRefinement, DeferredEdgePolicyKeepsDigestsAndPlanValidation) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete());
  const auto Strict = P.check(Recovery.Residual);
  ASSERT_TRUE(Strict.proved());
  P.Contract.DeferNativeConditionalEdges = true;
  const auto Deferred = P.check(Recovery.Residual);
  ASSERT_TRUE(Deferred.proved()) << Deferred.Proof.Diagnostic;
  EXPECT_NE(Strict.Certificate->Relation.OriginalDigest,
            Deferred.Certificate->Relation.OriginalDigest);
  EXPECT_NE(Strict.Certificate->Relation.InputDigest,
            Deferred.Certificate->Relation.InputDigest);
  EXPECT_NE(Strict.Certificate->InputDigest, Deferred.Certificate->InputDigest);
  const auto Loop = checkBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery.Residual, P.Contract, {});
  refused(Loop, Status::Invalid);
  EXPECT_NE(Loop.Proof.Diagnostic.find("nonempty cutpoint plan"),
            std::string::npos);
  const auto Inferred = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  EXPECT_EQ(Inferred.Inference.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(Inferred.Inference.Plan);
  refused(Inferred.Refinement, Status::Unsupported);
}

TEST(BinaryLowIRRefinement, SplitAlignedFrameStoresKeepTheFullStateRelation) {
  Program P({0x48, 0x8d, 0x54, 0x24, 0xbf, 0x80, 0xe2, 0xf0, 0x48, 0x8d, 0x7a,
             9, 0x89, 0x0f, 0x8b, 0x07, 0xc3});
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment = {16, 8};
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::STORE && O.NumInputs == 2) {
        O.Inputs[1] = NdVar::scalar(0, 4);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, RepeatedFeasibilityKeepsCandidateStateChecks) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  auto &Code = P.Image.Segments.front();
  Code.Data.insert(Code.Data.end() - 1, 128, 0x90);
  Code.Size = Code.FileSz = Code.Data.size();
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if ((O.Opcode == NdOp::COPY || O.Opcode == NdOp::INT_ZEXT) &&
          O.Output.isReg() && O.Output.Offset == x86reg::RAX &&
          O.NumInputs == 1 && O.Inputs[0].isConst() &&
          O.Inputs[0].Offset == 7) {
        O.Inputs[0].Offset = 9;
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, EntryAlignmentUsesTheOriginalSymbolicStack) {
  Program P({0x48, 0x89, 0xe0, 0xc3}); // mov rax,rsp; ret.
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  // OR preserves the declared low bits but cannot discard higher root bits.
  bool Changed = false;
  for (auto &Block : Recovery.Residual.Blocks)
    for (auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::COPY && Op.Output == NdVar::reg(x86reg::RAX, 8) &&
          Op.Inputs[0] == NdVar::reg(x86reg::RSP, 8)) {
        Op.Opcode = NdOp::INT_OR;
        Op.addInput(NdVar::scalar(3, 8));
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual), Status::Different);
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment =
      InterpreterEntryAlignment{16, 3};
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Certificate->Relation.Contract.Frame->EntryAlignment,
            P.Options.EntryFrameAlignment);
  P.Options.EntryFrameAlignment->Residue = 4;
  P.Contract.Frame->EntryAlignment = P.Options.EntryFrameAlignment;
  refused(P.check(Recovery.Residual), Status::Different);
  P.Options.EntryFrameAlignment->Residue = 3;
  P.Contract.Frame->EntryAlignment = P.Options.EntryFrameAlignment;
  for (auto &Block : Recovery.Residual.Blocks)
    for (auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INT_OR && Op.Output == NdVar::reg(x86reg::RAX, 8))
        Op.Inputs[1] = NdVar::scalar(35, 8);
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, EntryAlignmentRequiresMatchingValidContracts) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete());
  P.Options.EntryFrameAlignment = InterpreterEntryAlignment{16, 3};
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Contract.Frame->EntryAlignment = {16, 4};
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Contract.Frame->EntryAlignment = P.Options.EntryFrameAlignment;
  ASSERT_TRUE(P.check(Recovery.Residual).proved());
  const auto Independent =
      checkBinaryUndefinedIndependence(P.Image, Entry, P.Options, P.Contract);
  ASSERT_TRUE(Independent.proved()) << Independent.Proof.Diagnostic;
  EXPECT_EQ(Independent.Certificate->LowIR.Contract.Frame->EntryAlignment,
            P.Options.EntryFrameAlignment);
  P.Options.EntryFrameAlignment.reset();
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment;
  P.Options.FrameBaseRegister->Offset = x86reg::RAX;
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Options.FrameBaseRegister.reset();
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Options.FrameBaseRegister = symbolic::SymRegisterRange{x86reg::RSP, 8};
  P.Options.EntryFrameAlignment = P.Contract.Frame->EntryAlignment =
      InterpreterEntryAlignment{3, 1};
  refused(P.check(Recovery.Residual), Status::Invalid);
}

TEST(BinaryLowIRRefinement, ActualResidualAndOriginalBytesAreBound) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3}); // mov eax,7; ret.
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Certificate->Relation.Scope,
            LowIRRefinementScope::CompleteFiniteNativeToLowIRPaths);
  ASSERT_EQ(Good.Certificate->Instructions.size(), 2U);
  EXPECT_EQ(Good.Certificate->Instructions[0].NativeBytes,
            (std::vector<uint8_t>{0xb8, 7, 0, 0, 0}));
  P.Image.Segments[0].Data[1] = 9;
  refused(P.check(Recovery.Residual), Status::Different);
  EXPECT_EQ(Good.Certificate->Instructions[0].NativeBytes[1], 7);
}

TEST(BinaryLowIRRefinement, RegisterCaseDispatchChecksBothNativeControlCases) {
  // mov eax,16; mov edx,32; test cl,1; cmovz rax,rdx; jmp body;
  // body: mov r8,rsp; sub r8,rax; mov byte ptr [r8],90; ret.
  Program P({0xb8, 16,   0,    0,    0,    0xba, 32,   0,    0,  0,
             0xf6, 0xc1, 1,    0x48, 0x0f, 0x44, 0xc2, 0xeb, 0,  0x49,
             0x89, 0xe0, 0x49, 0x29, 0xc0, 0x41, 0xc6, 0,    90, 0xc3});
  P.Options.ControlRegisters = {{x86reg::RAX, 8}};
  P.Options.DiscoverControlState = true;
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  EXPECT_GT(Recovery.ControlRefinements, 0u);
  // The native memory checker currently requires a unique frame offset at
  // every access. Bind each selector separately here; the recovery above
  // and source runtime regression keep the selector unconstrained.
  for (uint64_t Input : {0, 1}) {
    P.Options.EntryConstants = {{NdVar::reg(x86reg::RCX, 8), Input}};
    P.Contract.EntryConstants = {{NdVar::reg(x86reg::RCX, 8), Input}};
    const auto Good = P.check(Recovery.Residual);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_TRUE(Good.Certificate->Relation.Contract.ObserveWrittenFrameBytes);
  }
  // A deliberately misrouted case must be rejected by the independent
  // native comparison, even if the residual graph itself remains well formed.
  bool Changed = false;
  for (auto &Block : Recovery.Residual.Blocks)
    for (auto &Op : Block.Ops)
      if (Op.Opcode == NdOp::INT_EQUAL && Op.Inputs[0].isReg() &&
          Op.Inputs[0].Offset == x86reg::RAX && Op.Inputs[0].Size == 8 &&
          Op.Inputs[1].isConst()) {
        ++Op.Inputs[1].Offset;
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  bool Rejected = false;
  for (uint64_t Input : {0, 1}) {
    P.Options.EntryConstants = {{NdVar::reg(x86reg::RCX, 8), Input}};
    P.Contract.EntryConstants = {{NdVar::reg(x86reg::RCX, 8), Input}};
    const auto Bad = P.check(Recovery.Residual);
    if (Bad.Proof.Status == Status::Different) {
      refused(Bad, Status::Different);
      Rejected = true;
    }
  }
  EXPECT_TRUE(Rejected);
}

TEST(BinaryLowIRRefinement, OverlappingEntriesKeepBothFeasibleBranchResults) {
  for (uint8_t Branch : {0x74, 0x75}) {
    // TEST ECX,ECX; JZ/JNZ second_mov; MOV EAX,0x7b8; RET; RET.
    // second_mov starts inside the first MOV and returns 0xc3000007.
    Program P({0x85, 0xc9, Branch, 1, 0xb8, 0xb8, 7, 0, 0, 0xc3, 0xc3});
    const auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    refused(P.check(Recovery.Residual), Status::Unsupported);
    P.Contract.AllowOverlappingNativeInstructions = true;
    const auto Good = P.check(Recovery.Residual);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    ASSERT_EQ(Good.Certificate->Instructions.size(), 6u);
    EXPECT_EQ(Good.Proof.OriginalPaths, 2u);
    EXPECT_EQ(Good.Proof.CandidatePaths, 2u);
    EXPECT_TRUE(
        Good.Certificate->Relation.Contract.AllowOverlappingNativeInstructions);
    for (uint64_t Value : {UINT64_C(0x7b8), UINT64_C(0xc3000007)}) {
      auto Changed = Recovery.Residual;
      bool Found = false;
      for (auto &Block : Changed.Blocks)
        for (auto &Op : Block.Ops)
          // Recovery can propagate the immediate through later zero-extends.
          // Change every data use for just this arm, not only a dead COPY.
          if (Op.Opcode == NdOp::COPY || Op.Opcode == NdOp::INT_ZEXT)
            for (unsigned I = 0; I != Op.NumInputs; ++I)
              if (Op.Inputs[I].isConst() && Op.Inputs[I].Offset == Value) {
                ++Op.Inputs[I].Offset;
                Found = true;
              }
      ASSERT_TRUE(Found);
      refused(P.check(Changed), Status::Different);
    }
    // The shared byte changes both native interpretations. Neither branch
    // can be ignored merely because its entry is inside another instruction.
    P.Image.Segments.front().Data[6] = 8;
    refused(P.check(Recovery.Residual), Status::Different);
  }
}

TEST(BinaryLowIRRefinement, NativeOverlapOptionBindsDigestsAndRejectsLoopAPIs) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Strict = P.check(Recovery.Residual);
  ASSERT_TRUE(Strict.proved()) << Strict.Proof.Diagnostic;
  P.Contract.AllowOverlappingNativeInstructions = true;
  const auto Finite = P.check(Recovery.Residual);
  ASSERT_TRUE(Finite.proved()) << Finite.Proof.Diagnostic;
  EXPECT_NE(Strict.Certificate->Relation.OriginalDigest,
            Finite.Certificate->Relation.OriginalDigest);
  EXPECT_NE(Strict.Certificate->Relation.InputDigest,
            Finite.Certificate->Relation.InputDigest);
  EXPECT_NE(Strict.Certificate->InputDigest, Finite.Certificate->InputDigest);
  LowIRLoopRefinementPlan Plan;
  refused(checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                         Recovery.Residual, P.Contract, Plan),
          Status::Unsupported);
  const auto Inferred = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  EXPECT_EQ(Inferred.Inference.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(Inferred.Inference.Plan);
  refused(Inferred.Refinement, Status::Unsupported);
}

TEST(BinaryLowIRRefinement, UndefinedCopiesAndStackRemainObservable) {
  // xor eax,eax; pushfq; pop rcx; mov rdx,rcx; ret. AF remains observable in
  // both copied registers, the flags bank and the written push slot.
  Program P({0x31, 0xc0, 0x9c, 0x59, 0x48, 0x89, 0xca, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_FALSE(Good.Certificate->Relation.Producers.empty());
  EXPECT_EQ(Good.Certificate->Relation.Witness, Witness::LiftedBits);
  const auto Strict =
      checkBinaryUndefinedIndependence(P.Image, Entry, P.Options, P.Contract);
  EXPECT_EQ(Strict.Proof.Status, LowIRIndependenceStatus::Dependent);
  EXPECT_FALSE(Strict.Certificate);
  refused(P.check(Recovery.Residual, Witness::ZeroBits), Status::Different);
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::COPY && O.Output == NdVar::reg(x86reg::RDX, 8)) {
        O.Inputs[0] = NdVar::scalar(0, 8);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, GuardedShiftSupportsAllSymbolicCounts) {
  // shl eax,cl; pushfq; pop rdx; ret. Zero count preserves flags; the witness
  // may select AF/OF only when their audited instruction guards activate.
  Program P({0xd3, 0xe0, 0x9c, 0x5a, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_GE(Good.Certificate->Relation.Producers.size(), 2U);
  refused(P.check(Recovery.Residual, Witness::ZeroBits), Status::Different);
}

TEST(BinaryLowIRRefinement, UnreachableBoundaryDoesNotHideReachableSuffix) {
  // CMP EAX,EAX; JE suffix; RCL EDX,1; suffix: MOV EAX,7; RET.
  // Collection sees the dead fallthrough boundary before the taken suffix.
  Program P({0x39, 0xc0, 0x74, 2, 0xd1, 0xd2, 0xb8, 7, 0, 0, 0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  refused(P.check(Recovery.Residual), Status::Unsupported);
  P.Contract.RetainUnauditedNativeBoundaries = true;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_EQ(Good.Certificate->Relation.NativeAuditBoundaries.size(), 1u);
  EXPECT_EQ(
      Good.Certificate->Relation.NativeAuditBoundaries.front().Boundary.Address,
      Entry + 4);
  EXPECT_TRUE(
      std::any_of(Good.Certificate->Instructions.begin(),
                  Good.Certificate->Instructions.end(),
                  [](const auto &I) { return I.Origin.Address == Entry + 6; }));
  // RCR EDX,1 is another valid, equally sized instruction with Missing
  // coverage. Its unreachable bytes still bind all three proof digests.
  P.Image.Segments.front().Data[5] = 0xda;
  const auto ChangedBoundary = P.check(Recovery.Residual);
  ASSERT_TRUE(ChangedBoundary.proved()) << ChangedBoundary.Proof.Diagnostic;
  EXPECT_NE(Good.Certificate->Relation.OriginalDigest,
            ChangedBoundary.Certificate->Relation.OriginalDigest);
  EXPECT_NE(Good.Certificate->Relation.InputDigest,
            ChangedBoundary.Certificate->Relation.InputDigest);
  EXPECT_NE(Good.Certificate->InputDigest,
            ChangedBoundary.Certificate->InputDigest);
  P.Image.Segments.front().Data[7] = 8;
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, BoundaryUnreachabilityIsRelativeToSelectedWitness) {
  // ADD establishes OF=1; BT makes it arbitrary while its ordinary lift keeps
  // the old bit. LiftedBits cannot take JNO, but ZeroBits does take it.
  Program P({0xb8, 0xff, 0xff, 0xff, 0x7f, 0x83, 0xc0, 1,    0x0f, 0xa3, 0xc8,
             0x71, 6,    0xb8, 7,    0,    0,    0,    0xc3, 0xd1, 0xd2, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  P.Contract.RetainUnauditedNativeBoundaries = true;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_EQ(Good.Certificate->Relation.NativeAuditBoundaries.size(), 1u);
  refused(P.check(Recovery.Residual, Witness::ZeroBits), Status::Unsupported);
  const auto Independent =
      checkBinaryUndefinedIndependence(P.Image, Entry, P.Options, P.Contract);
  EXPECT_EQ(Independent.Proof.Status, LowIRIndependenceStatus::Dependent);
  EXPECT_FALSE(Independent.Certificate);
}

TEST(BinaryLowIRRefinement, BoundaryPolicyKeepsDigestsAndPlanValidation) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Strict = P.check(Recovery.Residual);
  ASSERT_TRUE(Strict.proved()) << Strict.Proof.Diagnostic;
  P.Contract.RetainUnauditedNativeBoundaries = true;
  const auto Finite = P.check(Recovery.Residual);
  ASSERT_TRUE(Finite.proved()) << Finite.Proof.Diagnostic;
  EXPECT_TRUE(Finite.Certificate->Relation.NativeAuditBoundaries.empty());
  EXPECT_NE(Strict.Certificate->Relation.OriginalDigest,
            Finite.Certificate->Relation.OriginalDigest);
  EXPECT_NE(Strict.Certificate->Relation.InputDigest,
            Finite.Certificate->Relation.InputDigest);
  EXPECT_NE(Strict.Certificate->InputDigest, Finite.Certificate->InputDigest);
  LowIRLoopRefinementPlan Plan;
  const auto Loop = checkBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery.Residual, P.Contract, Plan);
  refused(Loop, Status::Invalid);
  EXPECT_NE(Loop.Proof.Diagnostic.find("nonempty cutpoint plan"),
            std::string::npos);
  const auto Inferred = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  EXPECT_EQ(Inferred.Inference.Status, LowIRLoopInferenceStatus::Unsupported);
  EXPECT_FALSE(Inferred.Inference.Plan);
  refused(Inferred.Refinement, Status::Unsupported);
}

TEST(BinaryLowIRRefinement, XaddHasNoFreshBitsAndRejectsAlteredDefinedOutputs) {
  for (const auto &Bytes :
       std::vector<std::vector<uint8_t>>{{0x0f, 0xc0, 0xc4},
                                         {0x0f, 0xc0, 0xe0},
                                         {0x0f, 0xc1, 0xc8},
                                         {0x48, 0x0f, 0xc1, 0xc0},
                                         {0x4d, 0x0f, 0xc1, 0xc8}}) {
    Program P({});
    auto &Code = P.Image.Segments.front();
    Code.Data = Bytes;
    Code.Data.push_back(0xc3);
    Code.Size = Code.FileSz = Code.Data.size();
    const auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    for (auto W : {Witness::LiftedBits, Witness::ZeroBits}) {
      const auto Good = P.check(Recovery.Residual, W);
      ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
      EXPECT_TRUE(Good.Certificate->Relation.Producers.empty());
    }
  }
  Program P({0x0f, 0xc1, 0xc8, 0xc3}); // XADD EAX,ECX; RET.
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  for (unsigned Field = 0; Field != 3; ++Field) {
    auto Changed = Recovery.Residual;
    bool Found = false;
    for (auto &Block : Changed.Blocks)
      for (auto &Op : Block.Ops)
        if (!Found && (Field == 0   ? Op.Opcode == NdOp::INT_ADD
                       : Field == 1 ? Op.Output == NdVar::reg(x86reg::RCX, 4)
                                    : Op.Output == NdVar::reg(x86reg::CF, 1))) {
          Op.Opcode = NdOp::COPY;
          Op.NumInputs = 1;
          Op.Inputs[0] = NdVar::scalar(0, Op.Output.Size);
          Found = true;
        }
    ASSERT_TRUE(Found);
    refused(P.check(Changed), Status::Different);
  }
}

TEST(BinaryLowIRRefinement, MemoryXaddBindsSumAddressSourceAndFlags) {
  // LEA RCX,[RSP-16]; XADD [RCX],ECX; RET. Writing ECX zero-extends RCX,
  // while the memory update must use the complete original address.
  Program P({0x48, 0x8d, 0x4c, 0x24, 0xf0, 0x0f, 0xc1, 0x09, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  for (auto W : {Witness::LiftedBits, Witness::ZeroBits}) {
    const auto Good = P.check(Recovery.Residual, W);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_TRUE(Good.Certificate->Relation.Producers.empty());
  }
  for (unsigned Field = 0; Field != 4; ++Field) {
    auto Changed = Recovery.Residual;
    bool Found = false;
    for (auto &Block : Changed.Blocks)
      for (auto &Op : Block.Ops) {
        if (Found)
          continue;
        if (Field < 2 && Op.Opcode == NdOp::STORE) {
          if (Field == 0)
            Op.Inputs[1] = NdVar::scalar(0, Op.Inputs[1].Size);
          else
            Op.Inputs[0] = NdVar::reg(x86reg::RSP, 8);
          Found = true;
        } else if ((Field == 2 && Op.Output.isReg() &&
                    Op.Output.Offset == x86reg::RCX && Op.Output.Size == 4) ||
                   (Field == 3 && Op.Output == NdVar::reg(x86reg::CF, 1))) {
          Op.Opcode = NdOp::COPY;
          Op.NumInputs = 1;
          Op.Inputs[0] = NdVar::scalar(0, Op.Output.Size);
          Found = true;
        }
      }
    ASSERT_TRUE(Found) << Field;
    const auto Bad = P.check(Changed);
    refused(Bad, Field == 1 ? Status::ContractViolation : Status::Different);
  }
}

TEST(BinaryLowIRRefinement, DoubleShiftWitnessesBindArbitraryAndDefinedSlices) {
  for (bool Right : {false, true})
    for (const auto &Prefix :
         std::vector<std::vector<uint8_t>>{{0x66}, {}, {0x48}}) {
      Program P({});
      auto &Code = P.Image.Segments.front();
      Code.Data = Prefix;
      // Destination RCX aliases CL; count must remain the entry low byte.
      Code.Data.insert(Code.Data.end(),
                       {0x0f, uint8_t(Right ? 0xad : 0xa5), 0xd1, 0xc3});
      Code.Size = Code.FileSz = Code.Data.size();
      const auto R = P.recover();
      ASSERT_TRUE(R.complete()) << R.Diagnostic;
      const auto Good = P.check(R.Residual);
      ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
      EXPECT_FALSE(Good.Certificate->Relation.Producers.empty());
      refused(P.check(R.Residual, Witness::ZeroBits), Status::Different);
    }
  Program P({0x66, 0x0f, 0xa4, 0xd0, 16, 0xc3});
  const auto R = P.recover();
  ASSERT_TRUE(R.complete()) << R.Diagnostic;
  ASSERT_TRUE(P.check(R.Residual).proved());
  // Count exactly 16 defines the low word and CF.
  for (unsigned Field = 0; Field != 3; ++Field) {
    auto Candidate = R.Residual;
    bool Found = false;
    for (auto &B : Candidate.Blocks)
      for (auto &O : B.Ops)
        if (!Found && (Field == 0   ? O.Output == NdVar::reg(x86reg::RAX, 2)
                       : Field == 1 ? O.Output == NdVar::reg(x86reg::CF, 1)
                                    : O.Output == NdVar::reg(x86reg::RAX, 2))) {
          O.Opcode = NdOp::COPY;
          O.NumInputs = 1;
          if (Field == 2)
            O.Output = NdVar::reg(x86reg::RAX, 8);
          O.Inputs[0] = NdVar::scalar(0, O.Output.Size);
          Found = true;
        }
    ASSERT_TRUE(Found);
    refused(P.check(Candidate), Status::Different);
  }
  Program Excess({0x66, 0x0f, 0xa4, 0xd0, 17, 0xc3});
  auto ExcessRecovery = Excess.recover();
  ASSERT_TRUE(ExcessRecovery.complete()) << ExcessRecovery.Diagnostic;
  ASSERT_TRUE(Excess.check(ExcessRecovery.Residual).proved());
  bool Found = false;
  for (auto &B : ExcessRecovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Output == NdVar::reg(x86reg::RAX, 2)) {
        // This selected low-word witness is already zero. Clearing the
        // remaining register bits still changes architecturally defined state.
        O.Opcode = NdOp::COPY;
        O.Output = NdVar::reg(x86reg::RAX, 8);
        O.NumInputs = 1;
        O.Inputs[0] = NdVar::scalar(0, 8);
        Found = true;
      }
  ASSERT_TRUE(Found);
  refused(Excess.check(ExcessRecovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, CompatibilityShiftUsesTheSameSelectedValueWitness) {
  Program P(
      {0xd3, 0xf0, 0x9c, 0x5a, 0xc3}); // SAL /6 EAX,CL; PUSHFQ; POP RDX; RET.
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_GE(Good.Certificate->Relation.Producers.size(), 2U);
  refused(P.check(Recovery.Residual, Witness::ZeroBits), Status::Different);
}

TEST(BinaryLowIRRefinement,
     BitTestsAndOverlappingRotatesKeepFullStateWitnesses) {
  const std::vector<std::vector<uint8_t>> Instructions = {
      {0x0f, 0xa3, 0xc8}, {0x0f, 0xab, 0xc8}, {0x0f, 0xb3, 0xc8},
      {0x0f, 0xbb, 0xc8}, {0xd2, 0xc1},       {0xd2, 0xc9}};
  for (const auto &Bytes : Instructions) {
    Program P({});
    auto &Code = P.Image.Segments.front();
    Code.Data = Bytes;
    Code.Data.insert(Code.Data.end(),
                     {0x9c, 0x5a, 0xc3}); // PUSHFQ; POP RDX; RET.
    Code.Size = Code.FileSz = Code.Data.size();
    auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    const auto Good = P.check(Recovery.Residual);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    ASSERT_FALSE(Good.Certificate->Relation.Producers.empty());
    refused(P.check(Recovery.Residual, Witness::ZeroBits), Status::Different);
    bool Changed = false;
    for (auto &B : Recovery.Residual.Blocks)
      for (auto &O : B.Ops)
        if (O.Opcode == NdOp::COPY && O.Output == NdVar::reg(x86reg::RDX, 8)) {
          O.Inputs[0] = NdVar::scalar(0, 8);
          Changed = true;
        }
    ASSERT_TRUE(Changed);
    refused(P.check(Recovery.Residual), Status::Different);
  }
}

TEST(BinaryLowIRRefinement, PhysicalCallAndModifiedReturnTarget) {
  // call body; nop; ret; body: inc qword [rsp]; ret. The modified continuation
  // skips the NOP. Original execution must load the actual stack target.
  Program P({0xe8, 2, 0, 0, 0, 0x90, 0xc3, 0x48, 0xff, 0x04, 0x24, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_TRUE(Good.Certificate->Relation.Contract.ObserveWrittenFrameBytes);
}

TEST(BinaryLowIRRefinement, FiniteLoopsCheckEveryInputPath) {
  // mov ecx,edi; and ecx,3; mov eax,0; test ecx,ecx; jz done;
  // again: inc eax; dec ecx; jnz again; done: ret.
  Program P({0x89, 0xf9, 0x83, 0xe1, 3,    0xb8, 0,    0,    0,    0,   0x85,
             0xc9, 0x74, 6,    0xff, 0xc0, 0xff, 0xc9, 0x75, 0xfa, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = P.check(Recovery.Residual);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.OriginalPaths, 4U);
  EXPECT_EQ(Good.Proof.CandidatePaths, 4U);
  LowIRRefinementLimits Limits;
  Limits.Execution.MaxBlockVisits = Good.Proof.BlockVisits - 1;
  refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
          Status::BudgetExceeded);
}

TEST(BinaryLowIRLoopRefinement, UnboundedInputCountAndOriginalBytes) {
  // top: jrcxz done; add rax,rcx;
  // lea rcx,[rcx-1]; jmp top; done: ret. No finite unrolling can cover all
  // uint64 input counts. The rank excludes wraparound using the JRCXZ guard.
  Program P(
      {0xe3, 9, 0x48, 0x01, 0xc8, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf5, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  LowIRLoopCutpoint Cut;
  Cut.OriginalAddress = Entry;
  Cut.UseEntryPrefix = true;
  unsigned Mappings = 0;
  for (const auto &Origin : Recovery.Origins)
    if (Origin.NativeInstruction.Address == Cut.OriginalAddress) {
      Cut.CandidateAddress = Origin.ResidualAddress;
      ++Mappings;
    }
  ASSERT_EQ(Mappings, 1U);
  const auto Parameter = [&](uint64_t Offset, uint16_t Bytes) {
    const auto T = NdVar::tmp(Cut.Inputs.size() * 8, Bytes);
    const LowIRLoopLocation L{LowIRLoopSpace::Register, Offset, Bytes};
    Cut.Inputs.push_back({LowIRLoopSide::Original, L, T});
    Cut.OriginalState.push_back({L, T});
    Cut.CandidateState.push_back({L, T});
    return T;
  };
  Parameter(x86reg::RAX, 8);
  Cut.Rank = {Parameter(x86reg::RCX, 8)};
  for (auto Flag :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    Parameter(Flag, 1);
  LowIRLoopRefinementPlan Plan{{Cut}};
  const auto Check = [&] {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                          Recovery.Residual, P.Contract, Plan);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Certificate->Relation.Scope,
            LowIRRefinementScope::InductiveNativeToLowIRLoops);
  EXPECT_EQ(Good.Proof.LoopInitiations, 1U);
  EXPECT_EQ(Good.Proof.RankingChecks, 1U);
  EXPECT_LT(Good.Proof.Instructions, 30U);
  // A changed native decrement must be re-executed, not matched only by PC.
  P.Image.Segments[0].Data[8] = 0xfe;
  refused(Check(), Status::Different);
}

TEST(BinaryLowIRLoopRefinement, CandidateTemporaryRetainsItsRealEntryValue) {
  // top: jrcxz done; add rax,rdx; lea rcx,[rcx-1]; jmp top; done: ret.
  Program P(
      {0xe3, 9, 0x48, 0x01, 0xd0, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf5, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  auto &Candidate = Recovery.Residual;
  LowIRLoopCutpoint Cut;
  Cut.OriginalAddress = Entry;
  Cut.UseEntryPrefix = true;
  unsigned Mappings = 0;
  for (const auto &Origin : Recovery.Origins)
    if (Origin.NativeInstruction.Address == Entry) {
      Cut.CandidateAddress = Origin.ResidualAddress;
      ++Mappings;
    }
  ASSERT_EQ(Mappings, 1U);
  const auto Parameter = [&](uint64_t Offset, uint16_t Bytes) {
    const auto T = NdVar::tmp(Cut.Inputs.size() * 8, Bytes);
    const LowIRLoopLocation L{LowIRLoopSpace::Register, Offset, Bytes};
    Cut.Inputs.push_back({LowIRLoopSide::Original, L, T});
    Cut.OriginalState.push_back({L, T});
    Cut.CandidateState.push_back({L, T});
    return T;
  };
  Parameter(x86reg::RAX, 8);
  Cut.Rank = {Parameter(x86reg::RCX, 8)};
  for (auto Flag :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    Parameter(Flag, 1);

  constexpr uint64_t Saved = uint64_t{1} << 60;
  constexpr va_t PrefixAddress = 0x80000000;
  LowBlock Prefix;
  Prefix.Id = -1;
  unsigned Replaced = 0;
  for (auto &B : Candidate.Blocks) {
    ASSERT_NE(B.StartAddr, PrefixAddress);
    Prefix.Id = std::max(Prefix.Id, B.Id);
    if (B.StartAddr == Candidate.Entry)
      Prefix.Succs = {B.Id};
    for (auto &O : B.Ops)
      for (unsigned I = 0; I != O.NumInputs; ++I)
        if (O.Inputs[I] == NdVar::reg(x86reg::RDX, 8)) {
          O.Inputs[I] = NdVar::tmp(Saved, 8);
          ++Replaced;
        }
  }
  ASSERT_GT(Replaced, 0U);
  ASSERT_EQ(Prefix.Succs.size(), 1U);
  ++Prefix.Id;
  Prefix.StartAddr = PrefixAddress;
  Prefix.EndAddr = PrefixAddress + 1;
  LowOp Save;
  Save.Opcode = NdOp::COPY;
  Save.Output = NdVar::tmp(Saved, 8);
  Save.addInput(NdVar::reg(x86reg::RDX, 8));
  Save.Addr = PrefixAddress;
  Save.Seq = 0;
  LowOp Branch;
  Branch.Opcode = NdOp::BRANCH;
  Branch.addInput(NdVar::scalar(Candidate.Entry, 8));
  Branch.Addr = PrefixAddress;
  Branch.Seq = 1;
  Prefix.Ops = {Save, Branch};
  LowInstructionBoundary Boundary;
  Boundary.Address = PrefixAddress;
  Boundary.Size = 1;
  Boundary.OpCount = 2;
  Boundary.Control = LowInstructionControl::Branch;
  Boundary.ControlFlags = LowInstructionControlFlag::Branch;
  Boundary.Immediate = Candidate.Entry;
  Prefix.InstructionBoundaries = {Boundary};
  for (auto *Roots : {&Candidate.ModuleAnalysisRoots,
                      &Candidate.OrdinaryModuleAnalysisRoots}) {
    for (va_t Root : *Roots)
      ASSERT_EQ(Root, Candidate.Entry);
    if (!Roots->empty())
      *Roots = {PrefixAddress};
  }
  Candidate.Entry = PrefixAddress;
  Candidate.FunctionTemporaries = {{Saved, 8}};
  Candidate.Blocks.push_back(std::move(Prefix));
  const LowIRLoopRefinementPlan Plan{{Cut}};
  const auto Check = [&] {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options, Candidate,
                                          P.Contract, Plan);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Certificate->Relation.Scope,
            LowIRRefinementScope::InductiveNativeToLowIRLoops);
  EXPECT_EQ(Good.Proof.RankingChecks, 1U);
  Candidate.Blocks.back().Ops.front().Inputs[0] = NdVar::reg(x86reg::R8, 8);
  refused(Check(), Status::Different);
  Candidate.Blocks.back().Ops.front().Inputs[0] = NdVar::reg(x86reg::RDX, 8);
  Candidate.FunctionTemporaries.clear();
  refused(Check(), Status::Invalid);
}

TEST(BinaryLowIRLoopInference, ChangingTemporaryStateBelongsOnlyToCandidate) {
  // nop; top: jrcxz done; add rax,rcx; lea rcx,[rcx-1]; jmp top; done: ret.
  // The candidate maintains an additional private counter. Its changing bits
  // still need an exact template, even though the native program has no slot.
  Program P({0x90, 0xe3, 9, 0x48, 0x01, 0xc8, 0x48, 0x8d, 0x49, 0xff, 0xeb,
             0xf5, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  constexpr uint64_t Saved = uint64_t{1} << 60;
  Recovery.Residual.FunctionTemporaries = {{Saved, 8}};
  unsigned Initialized = 0, Updated = 0;
  for (auto &B : Recovery.Residual.Blocks) {
    auto Previous = B.Ops;
    B.Ops.clear();
    for (auto &Boundary : B.InstructionBoundaries) {
      const auto Origin = std::find_if(
          Recovery.Origins.begin(), Recovery.Origins.end(),
          [&](const auto &O) { return O.ResidualAddress == Boundary.Address; });
      const auto Begin = Boundary.FirstOp;
      Boundary.FirstOp = B.Ops.size();
      B.Ops.insert(B.Ops.end(), Previous.begin() + Begin,
                   Previous.begin() + Begin + Boundary.OpCount);
      if (Origin == Recovery.Origins.end())
        continue; // keep synthetic recovery control boundaries unchanged
      const auto Native = Origin->NativeInstruction.Address;
      if (Native != Entry && Native != Entry + 6)
        continue;
      LowOp Update;
      Update.Addr = Boundary.Address;
      Update.Seq = Boundary.OpCount++;
      if (Native == Entry) {
        Update.Opcode = NdOp::COPY;
        Update.Output = NdVar::tmp(Saved, 8);
        Update.addInput(NdVar::scalar(0, 8));
        ++Initialized;
      } else {
        Update.Opcode = NdOp::INT_ADD;
        Update.Output = NdVar::tmp(Saved, 3);
        Update.addInput(Update.Output);
        Update.addInput(NdVar::scalar(1, 3));
        ++Updated;
      }
      B.Ops.push_back(Update);
    }
  }
  ASSERT_GT(Initialized, 0U);
  ASSERT_GT(Updated, 0U);
  const auto Good = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(Good.Inference.inferred()) << Good.Inference.Diagnostic;
  ASSERT_TRUE(Good.proved()) << Good.Refinement.Proof.Diagnostic;
  EXPECT_EQ(Good.Refinement.Certificate->Relation.Scope,
            LowIRRefinementScope::InductiveNativeToLowIRLoops);
  auto Plan = *Good.Inference.Plan;
  bool Parameter = false;
  for (const auto &Cut : Plan.Cutpoints) {
    for (const auto &Input : Cut.Inputs)
      if (Input.Location.Space == LowIRLoopSpace::FunctionTemporary) {
        EXPECT_TRUE(Input.Side == LowIRLoopSide::Candidate ||
                    Input.Side == LowIRLoopSide::CandidatePrefix);
        Parameter |= Input.Side == LowIRLoopSide::Candidate;
      }
    for (const auto &Assignment : Cut.OriginalState)
      EXPECT_NE(Assignment.Location.Space, LowIRLoopSpace::FunctionTemporary);
  }
  EXPECT_TRUE(Parameter);
  const auto Check = [&](const LowIRLoopRefinementPlan &Proposal) {
    return checkBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery.Residual, P.Contract, Proposal);
  };
  // Fixed prefix reads use the same candidate-only namespace, whether or not
  // this particular inference heuristic retains a fixed-bit predicate.
  for (auto &Cut : Plan.Cutpoints) {
    uint64_t Next = 0;
    for (const auto &Input : Cut.Inputs)
      Next = std::max(Next, Input.Temporary.Offset + Input.Temporary.Size);
    for (const auto &Expression : Cut.Expressions)
      Next = std::max(Next, Expression.Output.Offset + Expression.Output.Size);
    Cut.Inputs.push_back({LowIRLoopSide::CandidatePrefix,
                          {LowIRLoopSpace::FunctionTemporary, Saved, 8},
                          NdVar::tmp(Next, 8)});
  }
  ASSERT_TRUE(Check(Plan).proved());
  for (bool Input : {false, true}) {
    auto Bad = Plan;
    for (auto &Cut : Bad.Cutpoints) {
      if (Input) {
        for (auto &I : Cut.Inputs)
          if (I.Location.Space == LowIRLoopSpace::FunctionTemporary)
            I.Side = LowIRLoopSide::Original;
      } else {
        for (const auto &Assignment : Cut.CandidateState)
          if (Assignment.Location.Space == LowIRLoopSpace::FunctionTemporary)
            Cut.OriginalState.push_back(Assignment);
      }
    }
    refused(Check(Bad), Status::Invalid);
  }
  auto Missing = Plan;
  for (auto &Cut : Missing.Cutpoints)
    std::erase_if(Cut.CandidateState, [](const auto &A) {
      return A.Location.Space == LowIRLoopSpace::FunctionTemporary;
    });
  refused(Check(Missing), Status::Different);
  P.Image.Segments[0].Data[9] = 0xfe; // the original now decrements by two
  refused(Check(Plan), Status::Different);
}

TEST(BinaryLowIRLoopRefinement, NativeCollectionChecksEveryInductionDomain) {
  for (bool Deferred : {false, true}) {
    SCOPED_TRACE(Deferred);
    // top: jrcxz done; test rcx,rcx; jne body; bad: RCL EDX,1;
    // body: add rax,rcx; lea rcx,[rcx-1]; jmp top; done: ret.
    Program P({0xe3, 16, 0x48, 0x85, 0xc9, 0x75, 2, 0xd1, 0xd2, 0x48, 0x01,
               0xc8, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xee, 0xc3});
    const auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    if (Deferred) {
      // The candidate is an untrusted hint. Mutate the native dead arm after
      // recovery: neither invalid byte may be collected by the proof.
      P.Image.Segments[0].Data[7] = 0x16;
      P.Image.Segments[0].Data[8] = 0x06;
    }
    LowIRLoopCutpoint Cut;
    Cut.OriginalAddress = Entry;
    Cut.UseEntryPrefix = true;
    unsigned Mappings = 0;
    for (const auto &Origin : Recovery.Origins)
      if (Origin.NativeInstruction.Address == Entry) {
        Cut.CandidateAddress = Origin.ResidualAddress;
        ++Mappings;
      }
    ASSERT_EQ(Mappings, 1U);
    const auto Parameter = [&](uint64_t Offset, uint16_t Bytes) {
      const auto T = NdVar::tmp(Cut.Inputs.size() * 8, Bytes);
      const LowIRLoopLocation L{LowIRLoopSpace::Register, Offset, Bytes};
      Cut.Inputs.push_back({LowIRLoopSide::Original, L, T});
      Cut.OriginalState.push_back({L, T});
      Cut.CandidateState.push_back({L, T});
      return T;
    };
    Parameter(x86reg::RAX, 8);
    Cut.Rank = {Parameter(x86reg::RCX, 8)};
    for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                      x86reg::SF, x86reg::OF})
      Parameter(Flag, 1);
    const LowIRLoopRefinementPlan Plan{{Cut}};
    const auto Check = [&](const LowIRRefinementLimits &Limits = {}) {
      return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                            Recovery.Residual, P.Contract, Plan,
                                            Witness::LiftedBits, Limits);
    };
    refused(Check(), Status::Unsupported);
    P.Contract.DeferNativeConditionalEdges = Deferred;
    P.Contract.RetainUnauditedNativeBoundaries = !Deferred;
    const auto Good = Check();
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    EXPECT_EQ(Good.Certificate->Relation.Scope,
              LowIRRefinementScope::InductiveNativeToLowIRLoops);
    EXPECT_EQ(Good.Proof.RankingChecks, 1U);
    EXPECT_LT(Good.Proof.Instructions, 32U);
    EXPECT_EQ(Good.Certificate->Relation.NativeAuditBoundaries.size(),
              Deferred ? 0U : 1U);
    const auto Automatic = inferAndCheckBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery, P.Contract);
    ASSERT_TRUE(Automatic.Inference.inferred())
        << Automatic.Inference.Diagnostic;
    ASSERT_TRUE(Automatic.proved()) << Automatic.Refinement.Proof.Diagnostic;
    const auto OtherWitness = checkBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery.Residual, P.Contract, Plan,
        Witness::ZeroBits);
    ASSERT_TRUE(OtherWitness.proved()) << OtherWitness.Proof.Diagnostic;
    if (Deferred) {
      P.Contract.RetainUnauditedNativeBoundaries = true;
      const auto Combined = Check();
      ASSERT_TRUE(Combined.proved()) << Combined.Proof.Diagnostic;
      EXPECT_TRUE(Combined.Certificate->Relation.NativeAuditBoundaries.empty());
      EXPECT_NE(Good.Certificate->InputDigest,
                Combined.Certificate->InputDigest);
      P.Contract.RetainUnauditedNativeBoundaries = false;
    }
    if (!Deferred) {
      EXPECT_EQ(
          Good.Certificate->Relation.NativeAuditBoundaries[0].Boundary.Address,
          Entry + 7);
      P.Image.Segments[0].Data[8] = 0xda; // Dead RCR, still bound to the proof.
      const auto Changed = Check();
      ASSERT_TRUE(Changed.proved()) << Changed.Proof.Diagnostic;
      EXPECT_NE(Good.Certificate->InputDigest,
                Changed.Certificate->InputDigest);
      EXPECT_NE(Good.Certificate->Relation.OriginalDigest,
                Changed.Certificate->Relation.OriginalDigest);
      P.Image.Segments[0].Data[8] = 0xd2;
    }
    LowIRRefinementLimits Short;
    Short.Execution.MaxSolverQueries = Good.Proof.SolverQueries;
    ASSERT_TRUE(Check(Short).proved());
    Short.Execution.MaxSolverQueries = Good.Proof.SolverQueries - 1;
    refused(Check(Short), Status::BudgetExceeded);
    Short = {};
    Short.Execution.MaxInstructions = Good.Proof.Instructions - 1;
    refused(Check(Short), Status::BudgetExceeded);
    P.Image.Segments[0].Data[5] = 0x74; // JE makes the bad arm feasible.
    refused(Check(), Status::Unsupported);
    P.Image.Segments[0].Data[5] = 0x75;
    P.Image.Segments[0].Data[15] =
        0xfe; // A wrong rank update is still checked.
    refused(Check(), Status::Different);
  }
}

TEST(BinaryLowIRLoopRefinement, PhysicalCallsAndEarlierStackWrites) {
  // top: jrcxz done; call body; lea rcx,[rcx-1]; jmp top;
  // done: ret; body: add rax,rcx; ret.
  Program P({0xe3, 11, 0xe8, 7, 0, 0, 0, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf3,
             0xc3, 0x48, 0x01, 0xc8, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  LowIRLoopCutpoint Cut;
  // The callee entry has one recovery context: the physical call has already
  // written its continuation. The outer header has separate first/loop
  // contexts and cannot serve as a single paired cutpoint.
  Cut.OriginalAddress = Entry + 14;
  unsigned Mappings = 0;
  for (const auto &Origin : Recovery.Origins)
    if (Origin.NativeInstruction.Address == Cut.OriginalAddress) {
      Cut.CandidateAddress = Origin.ResidualAddress;
      ++Mappings;
    }
  ASSERT_EQ(Mappings, 1U);
  Cut.UseEntryPrefix = true;
  const auto Parameter = [&](LowIRLoopLocation L) {
    const auto T = NdVar::tmp(Cut.Inputs.size() * 8, L.Bytes);
    Cut.Inputs.push_back({LowIRLoopSide::Original, L, T});
    Cut.OriginalState.push_back({L, T});
    Cut.CandidateState.push_back({L, T});
    return T;
  };
  Parameter({LowIRLoopSpace::Register, x86reg::RAX, 8});
  Cut.Rank = {Parameter({LowIRLoopSpace::Register, x86reg::RCX, 8})};
  for (auto Flag :
       {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF, x86reg::SF, x86reg::OF})
    Parameter({LowIRLoopSpace::Register, Flag, 1});
  LowOp Nonzero;
  Nonzero.Opcode = NdOp::INT_NOTEQUAL;
  Nonzero.Output = NdVar::tmp(64, 1);
  Nonzero.addInput(Cut.Rank.front());
  Nonzero.addInput(NdVar::scalar(0, 8));
  Cut.Expressions = {Nonzero};
  Cut.Predicate = Nonzero.Output;
  LowIRLoopRefinementPlan Plan{{Cut}};
  const auto Check = [&] {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                          Recovery.Residual, P.Contract, Plan);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.RankingChecks, 1U);
  std::set<va_t> Addresses;
  for (const auto &I : Good.Certificate->Instructions)
    EXPECT_TRUE(Addresses.insert(I.Origin.Address).second);
  EXPECT_EQ(Addresses.size(), 7U);
  // The contract includes the return slot written before an earlier cut.
  // A candidate that writes a different continuation cannot hide the effect.
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::STORE) {
        auto Memory = lowMemoryOperands(O);
        ASSERT_TRUE(Memory.Complete);
        O.Inputs[Memory.StoredValue - O.Inputs] = NdVar::scalar(0, 8);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(Check(), Status::Different);
}

TEST(BinaryLowIRLoopInference, JoinedNativeEntryArmsUseCheckedGeneralization) {
  // Both arms initialize eax to 7. The joined loop adds the unsigned count
  // using LEA, keeping the TEST flags unchanged across every iteration.
  // test rdx,rdx; jz alternate; mov eax,7; jmp loop;
  // alternate: mov eax,7; loop: jrcxz done; lea rax,[rax+rcx];
  // lea rcx,[rcx-1]; jmp loop; done: ret.
  Program P({0x48, 0x85, 0xd2, 0x74, 7,    0xb8, 7,    0,    0,    0,
             0xeb, 5,    0xb8, 7,    0,    0,    0,    0xe3, 10,   0x48,
             0x8d, 0x04, 0x08, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf4, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Check = [&] {
    return inferAndCheckBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                                  Recovery, P.Contract);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.Inference.inferred()) << Good.Inference.Diagnostic;
  ASSERT_TRUE(Good.proved()) << Good.Refinement.Proof.Diagnostic;
  bool Generalized = false;
  for (const auto &Cut : Good.Inference.Plan->Cutpoints)
    Generalized |= Cut.GeneralizeEntryPrefix;
  EXPECT_TRUE(Generalized);
  // Changing the other original arm must fail even when the candidate and
  // its inferred plan still describe the old initialization.
  P.Image.Segments[0].Data[13] = 8;
  const auto Bad = Check();
  EXPECT_TRUE(Bad.Inference.inferred()) << Bad.Inference.Diagnostic;
  refused(Bad.Refinement, Status::Different);
}

TEST(BinaryLowIRLoopRefinement, GeneralizedDomainRechecksNativeTrapGuards) {
  // Entry excludes rdx == 0 from the loop. Removing that witness domain
  // admits a trap to the proposed invariant even though every real run is
  // safe. Generalization must reject the proposal, not assume the old guard.
  // test rdx,rdx; jz done; loop: test rdx,rdx; jz fault;
  // jrcxz done; lea rcx,[rcx-1]; jmp loop; done: ret; fault: ud2.
  Program P({0x48, 0x85, 0xd2, 0x74, 13,   0x48, 0x85, 0xd2, 0x74, 9,   0xe3,
             6,    0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf3, 0xc3, 0x0f, 0x0b});
  // Ordinary recovery refuses an opaque trap. Construct the candidate from
  // a separate image with a return on that arm; only the native checker may
  // establish that this difference is unreachable under a proposed domain.
  auto Candidate = P;
  Candidate.Image.Segments[0].Data[19] = 0xc3;
  Candidate.Image.Segments[0].Data[20] = 0x90;
  const auto Recovery = Candidate.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  LowIRLoopCutpoint Cut;
  Cut.OriginalAddress = Entry + 5;
  unsigned Matches = 0;
  for (const auto &Origin : Recovery.Origins)
    if (Origin.NativeInstruction.Address == Cut.OriginalAddress) {
      Cut.CandidateAddress = Origin.ResidualAddress;
      ++Matches;
    }
  ASSERT_EQ(Matches, 1U);
  Cut.UseEntryPrefix = true;
  const LowIRLoopLocation Count{LowIRLoopSpace::Register, x86reg::RCX, 8};
  Cut.Inputs = {{LowIRLoopSide::Original, Count, NdVar::tmp(0, 8)}};
  Cut.OriginalState = Cut.CandidateState = {{Count, NdVar::tmp(0, 8)}};
  Cut.Rank = {NdVar::tmp(0, 8)};
  LowIRLoopRefinementPlan Plan{{Cut}};
  const auto Check = [&] {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                          Recovery.Residual, P.Contract, Plan);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  Plan.Cutpoints[0].GeneralizeEntryPrefix = true;
  refused(Check(), Status::ContractViolation);
}

TEST(BinaryLowIRLoopInference, CounterAndCallsUseInferredCheckedPlans) {
  for (bool Calls : {false, true}) {
    Program P(Calls ? std::initializer_list<uint8_t>{0xe3, 11, 0xe8, 7, 0, 0, 0,
                                                     0x48, 0x8d, 0x49, 0xff,
                                                     0xeb, 0xf3, 0xc3, 0x48,
                                                     0x01, 0xc8, 0xc3}
                    : std::initializer_list<uint8_t>{0xe3, 9, 0x48, 0x01, 0xc8,
                                                     0x48, 0x8d, 0x49, 0xff,
                                                     0xeb, 0xf5, 0xc3});
    const auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    const auto R = inferAndCheckBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery, P.Contract);
    ASSERT_TRUE(R.Inference.inferred()) << R.Inference.Diagnostic;
    ASSERT_TRUE(R.proved()) << R.Refinement.Proof.Diagnostic;
    EXPECT_EQ(R.Refinement.Certificate->Relation.Scope,
              LowIRRefinementScope::InductiveNativeToLowIRLoops);
    // Adding repeated-origin candidates must retain the successful unique
    // native cuts before attempting a different context correspondence.
    for (const auto &C : R.Inference.Plan->Cutpoints)
      EXPECT_EQ(std::count_if(Recovery.Origins.begin(), Recovery.Origins.end(),
                              [&](const auto &O) {
                                return O.NativeInstruction.Address ==
                                       C.OriginalAddress;
                              }),
                1);
    // Origin metadata cannot excuse a different original instruction.
    P.Image.Segments[0].Data[Calls ? 10 : 8] = 0xfe;
    const auto Changed = inferAndCheckBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery, P.Contract);
    EXPECT_TRUE(Changed.Inference.inferred()) << Changed.Inference.Diagnostic;
    EXPECT_FALSE(Changed.proved());
    EXPECT_FALSE(Changed.Refinement.Certificate);
  }
}

TEST(BinaryLowIRLoopRefinement, NestedPrefixReplaysActualNativeEntry) {
  // xor eax,eax; outer: test rcx,rcx; jz done; mov rdx,r8;
  // inner: test rdx,rdx; jz next; add rax,rdx; dec rdx; jmp inner;
  // next: dec rcx; jmp outer; done: ret.
  Program P({0x31, 0xc0, 0x48, 0x85, 0xc9, 0x74, 0x15, 0x4c, 0x89, 0xc2,
             0x48, 0x85, 0xd2, 0x74, 0x08, 0x48, 0x01, 0xd0, 0x48, 0xff,
             0xca, 0xeb, 0xf3, 0x48, 0xff, 0xc9, 0xeb, 0xe6, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  LowIRLoopRefinementPlan Plan;
  for (bool Inner : {false, true}) {
    LowIRLoopCutpoint Cut;
    Cut.OriginalAddress = Entry + (Inner ? 15 : 7);
    unsigned Matches = 0;
    for (const auto &Origin : Recovery.Origins)
      if (Origin.NativeInstruction.Address == Cut.OriginalAddress) {
        Cut.CandidateAddress = Origin.ResidualAddress;
        ++Matches;
      }
    ASSERT_EQ(Matches, 1U);
    Cut.UseEntryPrefix = Inner;
    uint64_t Next = 0;
    const auto Temp = [&](uint16_t Bytes) {
      const auto V = NdVar::tmp(Next, Bytes);
      Next += 8;
      return V;
    };
    const auto Parameter = [&](uint64_t Offset, uint16_t Bytes = 8) {
      const LowIRLoopLocation L{LowIRLoopSpace::Register, Offset, Bytes};
      const auto T = Temp(Bytes);
      Cut.Inputs.push_back({LowIRLoopSide::Original, L, T});
      Cut.OriginalState.push_back({L, T});
      Cut.CandidateState.push_back({L, T});
      return T;
    };
    Parameter(x86reg::RAX);
    const auto OuterCount = Parameter(x86reg::RCX);
    const auto InnerCount = Parameter(x86reg::RDX);
    for (auto Flag : {x86reg::CF, x86reg::PF, x86reg::AF, x86reg::ZF,
                      x86reg::SF, x86reg::OF})
      Parameter(Flag, 1);
    const auto EntryCount = Temp(8);
    Cut.Inputs.push_back({LowIRLoopSide::Entry,
                          {LowIRLoopSpace::Register, x86reg::RCX, 8},
                          EntryCount});
    const auto Expr = [&](NdOp Code, NdVar A, NdVar B) {
      LowOp Op;
      Op.Opcode = Code;
      Op.Output = Temp(1);
      Op.addInput(A);
      Op.addInput(B);
      Cut.Expressions.push_back(Op);
      return Op.Output;
    };
    Cut.Predicate = Expr(NdOp::INT_LESSEQUAL, OuterCount, EntryCount);
    const auto OuterNonzero =
        Expr(NdOp::INT_NOTEQUAL, OuterCount, NdVar::scalar(0, 8));
    Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, OuterNonzero);
    if (Inner) {
      const auto EntryInner = Temp(8);
      Cut.Inputs.push_back({LowIRLoopSide::Entry,
                            {LowIRLoopSpace::Register, x86reg::R8, 8},
                            EntryInner});
      const auto InnerBound = Expr(NdOp::INT_LESSEQUAL, InnerCount, EntryInner);
      const auto InnerNonzero =
          Expr(NdOp::INT_NOTEQUAL, InnerCount, NdVar::scalar(0, 8));
      Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, InnerBound);
      Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, InnerNonzero);
    }
    Cut.Rank = {OuterCount, NdVar::scalar(Inner ? 0 : 1, 1),
                Inner ? InnerCount : NdVar::scalar(0, 8)};
    Plan.Cutpoints.push_back(std::move(Cut));
  }
  const auto Check = [&] {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                          Recovery.Residual, P.Contract, Plan);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.RankingChecks, 4U);
  EXPECT_GT(Good.Proof.LoopInitiations, 1U);
  // Removing the outer counter's entry bound must not turn a prefix seen
  // only for positive inputs into a globally assumed reachable state.
  Plan.Cutpoints[0].Predicate = NdVar::scalar(1, 1);
  refused(Check(), Status::Different);
  const auto Automatic = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(Automatic.Inference.inferred()) << Automatic.Inference.Diagnostic;
  ASSERT_TRUE(Automatic.proved()) << Automatic.Refinement.Proof.Diagnostic;
  EXPECT_GT(Automatic.Inference.Plan->Cutpoints.size(), 1U);
}

TEST(BinaryLowIRLoopInference, PackedFlagsStayConstrainedAcrossWidening) {
  // xor eax,eax; pushfq; pop r11; loop: jrcxz done; push r11; popfq;
  // inc rax; pushfq; pop r11; lea rcx,[rcx-1]; jmp loop; done: ret.
  // The loop carries both a packed flags image and the physical flags bank.
  // Equal bits can have different expressions after abstracting the counter.
  Program P({0x31, 0xc0, 0x9c, 0x41, 0x5b, 0xe3, 0x0f, 0x41,
             0x53, 0x9d, 0x48, 0xff, 0xc0, 0x9c, 0x41, 0x5b,
             0x48, 0x8d, 0x49, 0xff, 0xeb, 0xef, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto R = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(R.Inference.inferred()) << R.Inference.Diagnostic;
  ASSERT_TRUE(R.proved()) << R.Refinement.Proof.Diagnostic;
  EXPECT_GT(R.Inference.WideningRounds, 1U);
  EXPECT_GT(R.Refinement.Proof.RankingChecks, 0U);
}

TEST(BinaryLowIRLoopRefinement, GuardedCutsReadProfiledSystemFlags) {
  Program P(
      {0xe3, 9, 0x48, 0x01, 0xc8, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf5, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Inferred = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(Inferred.proved()) << Inferred.Refinement.Proof.Diagnostic;
  ASSERT_EQ(Inferred.Inference.Plan->Cutpoints.size(), 1U);
  const auto Original = Inferred.Inference.Plan->Cutpoints.front();
  LowIRLoopRefinementPlan Plan;
  const LowIRLoopLocation Flags{LowIRLoopSpace::SystemFlags, 0, 8};
  for (uint64_t Value : {0, 512}) {
    auto Cut = Original;
    uint64_t Next = 0;
    for (const auto &I : Cut.Inputs)
      Next = std::max(Next, I.Temporary.Offset + 8);
    for (const auto &O : Cut.Expressions)
      Next = std::max(Next, O.Output.Offset + 8);
    const auto Input = NdVar::tmp(Next, 8);
    Cut.Inputs.push_back({LowIRLoopSide::Entry, Flags, Input});
    Next += 8;
    const auto Expr = [&](NdOp Code, NdVar A, NdVar B, uint16_t Bytes) {
      LowOp O;
      O.Opcode = Code;
      O.Output = NdVar::tmp(Next, Bytes);
      Next += 8;
      O.addInput(A);
      O.addInput(B);
      Cut.Expressions.push_back(O);
      return O.Output;
    };
    const auto Bit = Expr(NdOp::INT_AND, Input, NdVar::scalar(512, 8), 8);
    const auto Selected =
        Expr(NdOp::INT_EQUAL, Bit, NdVar::scalar(Value, 8), 1);
    Cut.Predicate = Expr(NdOp::BOOL_AND, Cut.Predicate, Selected, 1);
    Cut.OriginalGuards = Cut.CandidateGuards = {{Flags, 512, Value}};
    Plan.Cutpoints.push_back(std::move(Cut));
  }
  const auto Good = checkBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery.Residual, P.Contract, Plan);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  EXPECT_EQ(Good.Proof.LoopInitiations, 2U);
  for (auto Location : {LowIRLoopLocation{LowIRLoopSpace::SystemFlags, 1, 8},
                        LowIRLoopLocation{LowIRLoopSpace::SystemFlags, 0, 4}}) {
    auto Wrong = Plan;
    Wrong.Cutpoints[0].OriginalGuards[0].Location = Location;
    refused(checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                           Recovery.Residual, P.Contract,
                                           Wrong),
            Status::Invalid);
  }
}

TEST(BinaryLowIRLoopRefinement, GuardedCutsCannotBypassUnauditedBoundaries) {
  Program P({0xd1, 0xd2, 0xc3}); // RCL EDX,1 has missing effect coverage.
  Program Safe({0xc3});
  const auto Candidate = Safe.recover();
  ASSERT_TRUE(Candidate.complete()) << Candidate.Diagnostic;
  P.Contract.RetainUnauditedNativeBoundaries = true;
  LowIRLoopCutpoint Cut;
  Cut.OriginalAddress = Entry;
  Cut.CandidateAddress = Candidate.Residual.Entry;
  Cut.UseEntryPrefix = true;
  Cut.Rank = {NdVar::scalar(0, 8)};
  // Bit 1 is set in every admitted system-flags image, so this selector never
  // matches. Its failure still cannot justify skipping unaudited native bytes.
  Cut.OriginalGuards = {{{LowIRLoopSpace::SystemFlags, 0, 8}, 2, 0}};
  const auto R = checkBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Candidate.Residual, P.Contract, {{Cut}});
  refused(R, Status::Unsupported);
  EXPECT_NE(R.Proof.Diagnostic.find("unaudited native boundary"),
            std::string::npos);
  EXPECT_EQ(R.Proof.Operations, 0U);
}

TEST(BinaryLowIRLoopRefinement, GuardedCutsProveRepeatedNativeContexts) {
  // R10 selects two recovered copies of the same original loop. RCX remains
  // arbitrary on both paths; no specialization entry constant is a premise.
  auto P = repeatedNativeContexts();
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  std::vector<va_t> Headers;
  for (const auto &O : Recovery.Origins)
    if (O.NativeInstruction.Address == Entry + 14)
      Headers.push_back(O.ResidualAddress);
  ASSERT_EQ(Headers.size(), 2U);
  const auto Inferred =
      inferLowIRLoopRefinementPlan(Recovery.Residual, P.Contract, {}, Headers);
  ASSERT_TRUE(Inferred.inferred()) << Inferred.Diagnostic;
  ASSERT_EQ(Inferred.Plan->Cutpoints.size(), 2U);
  auto Unguarded = *Inferred.Plan;
  for (auto &C : Unguarded.Cutpoints)
    C.OriginalAddress = Entry + 14;
  refused(checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                         Recovery.Residual, P.Contract,
                                         Unguarded),
          Status::Invalid);
  unsigned Proved = 0;
  // Try both explicit selector assignments. These are untrusted proposals;
  // only the complete original/candidate checker may accept a correspondence.
  for (unsigned Reverse : {0, 1}) {
    auto Plan = Unguarded;
    for (unsigned I = 0; I != 2; ++I)
      Plan.Cutpoints[I].OriginalGuards = Plan.Cutpoints[I].CandidateGuards = {
          {{LowIRLoopSpace::Register, x86reg::R10, 8}, 1, I ^ Reverse}};
    const auto R = checkBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery.Residual, P.Contract, Plan);
    if (!R.proved())
      continue;
    ++Proved;
    EXPECT_EQ(R.Proof.LoopInitiations, 2U);
    EXPECT_GT(R.Proof.RankingChecks, 0U);
    auto Wrong = Plan;
    Wrong.Cutpoints[1].OriginalGuards = Wrong.Cutpoints[0].OriginalGuards;
    EXPECT_FALSE(checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                                Recovery.Residual, P.Contract,
                                                Wrong)
                     .proved());
  }
  EXPECT_EQ(Proved, 1U);
}

TEST(BinaryLowIRLoopInference,
     SingleChosenContextSkipsEarlierNativeOccurrences) {
  for (bool Frame : {false, true})
    for (bool Byte : {false, true})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(Frame);
        SCOPED_TRACE(Byte);
        SCOPED_TRACE(Reverse);
        SingleNativeContext Fixture(Frame, Byte, Reverse);
        auto &P = Fixture.Input;
        auto Recovery = P.recover();
        ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
        // Retain both actual occurrences of this origin as candidates. The
        // earlier initialization context is not part of the selected loop.
        std::erase_if(Recovery.Origins, [&](const auto &O) {
          return O.NativeInstruction.Address != Entry + Fixture.Shared;
        });
        ASSERT_EQ(Recovery.Origins.size(), 2U);
        const auto Check = [&](const LowIRLoopInferenceLimits &Limits) {
          return inferAndCheckBinaryLowIRLoopRefinement(
              P.Image, Entry, P.Options, Recovery, P.Contract,
              Witness::LiftedBits, {}, Limits);
        };
        const auto Good = Check({});
        ASSERT_TRUE(Good.Inference.inferred()) << Good.Inference.Diagnostic;
        ASSERT_TRUE(Good.proved()) << Good.Refinement.Proof.Diagnostic;
        ASSERT_EQ(Good.Inference.Plan->Cutpoints.size(), 1U);
        EXPECT_FALSE(Good.Inference.Plan->Cutpoints[0].OriginalGuards.empty());

        LowIRLoopInferenceLimits Exact;
        Exact.MaxCutSelectionWork = Good.Inference.CutSelectionWork;
        Exact.Execution.MaxOperations = Good.Inference.Operations;
        Exact.Execution.MaxSolverQueries = Good.Inference.SolverQueries;
        ASSERT_TRUE(Check(Exact).proved());
        for (unsigned Kind = 0; Kind != 3; ++Kind) {
          auto Short = Exact;
          if (Kind == 0)
            --Short.MaxCutSelectionWork;
          else if (Kind == 1)
            --Short.Execution.MaxOperations;
          else
            --Short.Execution.MaxSolverQueries;
          const auto R = Check(Short);
          EXPECT_EQ(R.Inference.Status,
                    LowIRLoopInferenceStatus::BudgetExceeded);
          EXPECT_FALSE(R.Inference.Plan);
          EXPECT_FALSE(R.Refinement.Certificate);
        }
        auto &Bytes = P.Image.Segments.front().Data;
        ASSERT_EQ(Bytes[Fixture.Update + 1], 0x01U);
        Bytes[Fixture.Update + 1] =
            0x29; // ADD -> SUB under the same selectors.
        const auto Bad = checkBinaryLowIRLoopRefinement(
            P.Image, Entry, P.Options, Recovery.Residual, P.Contract,
            *Good.Inference.Plan);
        EXPECT_FALSE(Bad.proved());
        EXPECT_FALSE(Bad.Certificate);
      }
}

TEST(BinaryLowIRLoopInference, NativeSelectorsProveRepeatedContexts) {
  auto P = repeatedNativeContexts();
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto R = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(R.Inference.inferred()) << R.Inference.Diagnostic;
  EXPECT_GT(R.Inference.EntailmentCacheHits, 0U);
  ASSERT_TRUE(R.proved()) << R.Refinement.Proof.Diagnostic;
  ASSERT_EQ(R.Inference.Plan->Cutpoints.size(), 2U);
  const auto &A = R.Inference.Plan->Cutpoints[0];
  const auto &B = R.Inference.Plan->Cutpoints[1];
  EXPECT_EQ(A.OriginalAddress, B.OriginalAddress);
  EXPECT_NE(A.CandidateAddress, B.CandidateAddress);
  EXPECT_FALSE(A.OriginalGuards.empty());
  EXPECT_FALSE(B.OriginalGuards.empty());
  EXPECT_GT(R.Inference.CutSelectionWork, 0U);
  EXPECT_EQ(R.Refinement.Proof.LoopInitiations, 2U);
}

TEST(BinaryLowIRLoopInference,
     NativeSelectorsGeneralizeAlternatingEntryPhases) {
  for (bool Frame : {false, true})
    for (bool Byte : {false, true})
      for (bool Reverse : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Frame << ":" << Byte << ":" << Reverse);
        auto P = alternatingNativeContexts(Frame, Byte, Reverse);
        const auto Recovery = P.recover();
        ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
        const auto R = inferAndCheckBinaryLowIRLoopRefinement(
            P.Image, Entry, P.Options, Recovery, P.Contract);
        EXPECT_TRUE(R.Inference.inferred()) << R.Inference.Diagnostic;
        EXPECT_TRUE(R.proved()) << R.Refinement.Proof.Diagnostic;
        if (!R.proved())
          continue;
        EXPECT_GT(R.Refinement.Proof.LoopTransitions, 0U);
        EXPECT_GT(R.Refinement.Proof.RankingChecks, 0U);
        EXPECT_TRUE(
            std::any_of(R.Inference.Plan->Cutpoints.begin(),
                        R.Inference.Plan->Cutpoints.end(),
                        [](const auto &C) { return C.GeneralizeEntryPrefix; }));
        if (!Frame)
          EXPECT_FALSE(
              R.Inference.Plan->Cutpoints.front().OriginalGuards.empty());
      }
}

TEST(BinaryLowIRLoopInference, NativeSelectorsGeneralizeMirroredFramePhases) {
  for (bool Byte : {false, true})
    for (bool Reverse : {false, true}) {
      SCOPED_TRACE(Byte);
      SCOPED_TRACE(Reverse);
      auto P = alternatingNativeContexts(true, Byte, Reverse, true);
      const auto Recovery = P.recover();
      ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
      const auto R = inferAndCheckBinaryLowIRLoopRefinement(
          P.Image, Entry, P.Options, Recovery, P.Contract);
      ASSERT_TRUE(R.Inference.inferred()) << R.Inference.Diagnostic;
      ASSERT_TRUE(R.proved()) << R.Refinement.Proof.Diagnostic;
      ASSERT_EQ(R.Inference.Plan->Cutpoints.size(), 1U);
      const auto &C = R.Inference.Plan->Cutpoints.front();
      EXPECT_TRUE(C.GeneralizeEntryPrefix);
      EXPECT_TRUE(std::any_of(
          C.OriginalGuards.begin(), C.OriginalGuards.end(), [](const auto &G) {
            return G.Location.Space == LowIRLoopSpace::Frame;
          }));
    }
}

TEST(BinaryLowIRLoopInference, NativeSelectorStateRetainsWholeOverlappingCut) {
  auto P = repeatedNativeFrameContexts();
  P.Contract.ObserveWrittenFrameBytes = true;
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Inferred = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(Inferred.proved()) << Inferred.Inference.Diagnostic << ": "
                                 << Inferred.Refinement.Proof.Diagnostic;
  ASSERT_EQ(Inferred.Inference.Plan->Cutpoints.size(), 2U);
  const auto Check = [&](const LowIRLoopRefinementPlan &Plan) {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                          Recovery.Residual, P.Contract, Plan);
  };
  // Compose the actual production builder with complete native proofs. These
  // explicit plans isolate overlap policy from automatic widening choices.
  for (unsigned Kind = 0; Kind != 3; ++Kind) {
    SCOPED_TRACE(Kind);
    auto Plan = *Inferred.Inference.Plan;
    for (auto &C : Plan.Cutpoints) {
      ASSERT_TRUE(C.UseEntryPrefix);
      ASSERT_FALSE(C.GeneralizeEntryPrefix);
      ASSERT_EQ(C.OriginalGuards.size(), 1U);
      ASSERT_EQ(C.CandidateGuards.size(), 1U);
      C.GeneralizeEntryPrefix = true;
    }
    auto &Overlap = Plan.Cutpoints.back();
    const LowIRLoopLocation Byte{LowIRLoopSpace::Frame, uint64_t(-7), 1};
    const auto Prefix = NdVar::tmp(0x100000, 1);
    // This is a genuine original-prefix binding. The builder must preserve
    // it; candidate-inference rebinding belongs to its separate caller.
    Overlap.Inputs.push_back({LowIRLoopSide::OriginalPrefix, Byte, Prefix});
    if (Kind == 2)
      Overlap.OriginalGuards.push_back(Overlap.OriginalGuards.front());
    else
      (Kind == 0 ? Overlap.OriginalState : Overlap.CandidateState)
          .push_back({Byte, Prefix});
    const auto Before = Plan;
    const auto Valid = Check(Before);
    ASSERT_TRUE(Valid.proved()) << Valid.Proof.Diagnostic;

    // Compute the required work from this fixture, independently of the
    // builder's reported count: one metadata visit per cut, one visit plus
    // every byte for all state/guard ranges (including the retained cut),
    // then fresh-temporary scans and four appended records per disjoint guard.
    uint64_t ExpectedWork = 17 + Before.Cutpoints.size();
    for (const auto &C : Before.Cutpoints) {
      for (const auto *State : {&C.OriginalState, &C.CandidateState})
        for (const auto &A : *State)
          ExpectedWork += 1 + A.Location.Bytes;
      for (const auto *Guards : {&C.OriginalGuards, &C.CandidateGuards})
        for (const auto &G : *Guards)
          ExpectedWork += 1 + G.Location.Bytes;
    }
    const auto &Disjoint = Before.Cutpoints.front();
    ExpectedWork +=
        Disjoint.Inputs.size() + Disjoint.Expressions.size() +
        4 * (Disjoint.OriginalGuards.size() + Disjoint.CandidateGuards.size());
    struct WorkLimit {};
    uint64_t Work = 0;
    const auto Prepare = [&](uint64_t Limit) {
      std::optional<LowIRLoopRefinementPlan> Proposed = Before;
      Work = 17; // Work already consumed by the inference caller.
      try {
        const auto Failure = detail::proposeNativeLoopSelectorStates(
            *Proposed, {}, [&](uint64_t Count) {
              if (Count > Limit - Work)
                throw WorkLimit{};
              Work += Count;
            });
        EXPECT_FALSE(Failure);
        if (Failure)
          Proposed.reset();
      } catch (const WorkLimit &) {
        // Even if an earlier cut was appended, no partial plan is consumed.
        Proposed.reset();
      }
      return Proposed;
    };
    const auto Proposed = Prepare(UINT64_MAX);
    ASSERT_TRUE(Proposed);
    EXPECT_EQ(Work, ExpectedWork);
    const auto &Kept = Proposed->Cutpoints.back();
    EXPECT_EQ(Kept.Inputs.size(), Overlap.Inputs.size());
    EXPECT_EQ(Kept.Expressions.size(), Overlap.Expressions.size());
    EXPECT_EQ(Kept.OriginalState.size(), Overlap.OriginalState.size());
    EXPECT_EQ(Kept.CandidateState.size(), Overlap.CandidateState.size());
    EXPECT_EQ(Kept.Inputs.back().Side, LowIRLoopSide::OriginalPrefix);
    const auto &Appended = Proposed->Cutpoints.front();
    const auto &Old = Before.Cutpoints.front();
    EXPECT_EQ(Appended.Inputs.size(), Old.Inputs.size() + 2);
    EXPECT_EQ(Appended.Expressions.size(), Old.Expressions.size() + 4);
    EXPECT_EQ(Appended.OriginalState.size(), Old.OriginalState.size() + 1);
    EXPECT_EQ(Appended.CandidateState.size(), Old.CandidateState.size() + 1);
    const auto Good = Check(*Proposed);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    ASSERT_TRUE(Good.Proof.Certificate);
    EXPECT_EQ(Good.Proof.Certificate->Scope,
              LowIRRefinementScope::InductiveNativeToLowIRLoops);
    EXPECT_EQ(Good.Certificate->Relation.InputDigest,
              Good.Proof.Certificate->InputDigest);
    EXPECT_GT(Good.Proof.LoopTransitions, 0U);
    EXPECT_GT(Good.Proof.RankingChecks, 0U);
    const auto Exact = Prepare(ExpectedWork);
    ASSERT_TRUE(Exact);
    EXPECT_TRUE(Check(*Exact).proved());
    EXPECT_FALSE(Prepare(ExpectedWork - 1));
  }
}

TEST(BinaryLowIRLoopInference, NativeSelectorStateRejectsTemporaryOverflow) {
  auto P = repeatedNativeFrameContexts();
  P.Contract.ObserveWrittenFrameBytes = true;
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Inferred = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(Inferred.proved()) << Inferred.Inference.Diagnostic << ": "
                                 << Inferred.Refinement.Proof.Diagnostic;
  auto Plan = *Inferred.Inference.Plan;
  auto &C = Plan.Cutpoints.front();
  C.GeneralizeEntryPrefix = true;
  // This binding itself fits. There is no room for three fresh eight-byte
  // temporary slots per guard beyond it, even though the plan is valid.
  C.Inputs.push_back({LowIRLoopSide::OriginalPrefix,
                      {LowIRLoopSpace::Frame, uint64_t(-8), 8},
                      NdVar::tmp(UINT64_MAX - 15, 8)});
  const auto Valid = checkBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery.Residual, P.Contract, Plan);
  ASSERT_TRUE(Valid.proved()) << Valid.Proof.Diagnostic;
  std::optional<LowIRLoopRefinementPlan> Proposed = Plan;
  const auto Failure =
      detail::proposeNativeLoopSelectorStates(*Proposed, {}, [](uint64_t) {});
  ASSERT_TRUE(Failure);
  EXPECT_EQ(Failure->Status, LowIRLoopInferenceStatus::Invalid);
  EXPECT_NE(Failure->Diagnostic.find("temporary offset overflow"),
            std::string::npos);
  Proposed.reset(); // Failed construction publishes no plan or certificate.
  EXPECT_FALSE(Proposed);
}

TEST(BinaryLowIRLoopInference, NativeSelectorStateMetadataHasIndependentLimit) {
  auto P = alternatingNativeContexts(false, true, false);
  std::vector<uint8_t> Prefix;
  // These are executed writes, not entry assumptions. Keep all register and
  // written-frame observations. Fixed ranges each add two guards and two
  // input/assignment pairs, isolating plan capacity from instruction visits.
  for (unsigned Reg : {2, 3, 5, 6, 8, 9, 11, 12, 13, 14, 15}) {
    if (Reg >= 8)
      Prefix.push_back(0x41);
    Prefix.insert(Prefix.end(), {uint8_t(0xb8 + (Reg & 7)), 0, 0, 0, 0});
  }
  for (int Offset = -64; Offset < 0; Offset += 8)
    Prefix.insert(Prefix.end(), {0x48, 0xc7, 0x44, 0x24,
                                 static_cast<uint8_t>(Offset), 0, 0, 0, 0});
  auto &Code = P.Image.Segments.front();
  Code.Data.insert(Code.Data.begin(), Prefix.begin(), Prefix.end());
  Code.Size = Code.FileSz = Code.Data.size();
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Check = [&](const LowIRLoopInferenceLimits &Limits) {
    return inferAndCheckBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery, P.Contract, Witness::LiftedBits,
        {}, Limits);
  };
  const auto Good = Check({});
  ASSERT_TRUE(Good.proved())
      << Good.Inference.Diagnostic << ": " << Good.Refinement.Proof.Diagnostic;
  uint64_t Metadata = 0;
  for (const auto &C : Good.Inference.Plan->Cutpoints)
    Metadata += C.Inputs.size() + C.OriginalState.size() +
                C.CandidateState.size() + C.Rank.size() +
                C.OriginalGuards.size() + C.CandidateGuards.size();
  LowIRLoopInferenceLimits Exact;
  Exact.Execution.MaxInstructions = Metadata;
  const auto Fits = Check(Exact);
  ASSERT_TRUE(Fits.proved()) << Metadata << ": " << Fits.Inference.Diagnostic;
  --Exact.Execution.MaxInstructions;
  const auto Short = Check(Exact);
  EXPECT_EQ(Short.Inference.Status, LowIRLoopInferenceStatus::BudgetExceeded);
  EXPECT_NE(Short.Inference.Diagnostic.find("native selector state metadata"),
            std::string::npos)
      << Short.Inference.Diagnostic;
  EXPECT_FALSE(Short.Inference.Plan);
  EXPECT_FALSE(Short.Refinement.Certificate);
  EXPECT_FALSE(Short.Refinement.Proof.Certificate);
}

TEST(BinaryLowIRLoopInference, NativeSelectorStatePreservesProofObligations) {
  auto P = alternatingNativeContexts(false, true, false);
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(Good.proved())
      << Good.Inference.Diagnostic << ": " << Good.Refinement.Proof.Diagnostic;
  ASSERT_EQ(Good.Inference.Plan->Cutpoints.size(), 1U);
  ASSERT_TRUE(Good.Refinement.Proof.Certificate);
  EXPECT_EQ(Good.Refinement.Proof.Certificate->Scope,
            LowIRRefinementScope::InductiveNativeToLowIRLoops);
  EXPECT_EQ(Good.Refinement.Certificate->Relation.InputDigest,
            Good.Refinement.Proof.Certificate->InputDigest);
  const auto Check = [&](const LowIRLoopRefinementPlan &Plan) {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                          Recovery.Residual, P.Contract, Plan);
  };
  for (bool Original : {true, false}) {
    SCOPED_TRACE(Original);
    auto Bad = *Good.Inference.Plan;
    auto &C = Bad.Cutpoints.front();
    auto &State = Original ? C.OriginalState : C.CandidateState;
    const auto A = std::find_if(State.begin(), State.end(), [](const auto &A) {
      return A.Location.Space == LowIRLoopSpace::Register &&
             A.Location.Offset == x86reg::R10;
    });
    ASSERT_NE(A, State.end());
    const auto Op = std::find_if(
        C.Expressions.begin(), C.Expressions.end(),
        [&](const auto &Op) { return Op.Output.Offset == A->Value.Offset; });
    ASSERT_NE(Op, C.Expressions.end());
    ASSERT_EQ(Op->Opcode, NdOp::INT_OR);
    // The selector masks only the low byte. Forcing bit eight must still
    // fail full state equality on each side, despite satisfying that guard.
    Op->Inputs[1].Offset |= uint64_t{1} << 8;
    refused(Check(Bad), Status::Different);
  }
  auto BadRank = *Good.Inference.Plan;
  for (auto &V : BadRank.Cutpoints.front().Rank)
    V = NdVar::scalar(0, V.Size);
  refused(Check(BadRank), Status::Different);

  auto MissingSelector = *Good.Inference.Plan;
  MissingSelector.Cutpoints.front().OriginalGuards.clear();
  refused(Check(MissingSelector), Status::Different);

  auto OverlappingAssignments = *Good.Inference.Plan;
  auto &State = OverlappingAssignments.Cutpoints.front().OriginalState;
  State.push_back(State.back());
  refused(Check(OverlappingAssignments), Status::Invalid);

  auto &Bytes = P.Image.Segments.front().Data;
  const uint8_t Add[] = {0x48, 0x01, 0xc8};
  const auto It =
      std::search(Bytes.begin(), Bytes.end(), std::begin(Add), std::end(Add));
  ASSERT_NE(It, Bytes.end());
  It[1] = 0x29; // ADD -> SUB while preserving the recovered candidate.
  refused(Check(*Good.Inference.Plan), Status::Different);
}

TEST(BinaryLowIRLoopInference, NativeSelectorStateConstructionWorkIsBounded) {
  auto P = alternatingNativeContexts(false, true, false);
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Check = [&](const LowIRLoopInferenceLimits &Limits) {
    return inferAndCheckBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery, P.Contract, Witness::LiftedBits,
        {}, Limits);
  };
  const auto Good = Check({});
  ASSERT_TRUE(Good.proved())
      << Good.Inference.Diagnostic << ": " << Good.Refinement.Proof.Diagnostic;
  LowIRLoopInferenceLimits Exact;
  Exact.MaxCutSelectionWork = Good.Inference.CutSelectionWork;
  ASSERT_GT(Exact.MaxCutSelectionWork, 0U);
  ASSERT_TRUE(Check(Exact).proved());
  --Exact.MaxCutSelectionWork;
  const auto Short = Check(Exact);
  EXPECT_EQ(Short.Inference.Status, LowIRLoopInferenceStatus::BudgetExceeded);
  EXPECT_NE(Short.Inference.Diagnostic.find("native selector state proposals"),
            std::string::npos)
      << Short.Inference.Diagnostic;
  EXPECT_FALSE(Short.Inference.Plan);
  EXPECT_FALSE(Short.Refinement.Certificate);
  EXPECT_FALSE(Short.Refinement.Proof.Certificate);
}

TEST(BinaryLowIRLoopInference, NativeSelectorsProveFrameContexts) {
  auto P = repeatedNativeFrameContexts();
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto R = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(R.Inference.inferred()) << R.Inference.Diagnostic;
  ASSERT_TRUE(R.proved()) << R.Refinement.Proof.Diagnostic;
  ASSERT_EQ(R.Inference.Plan->Cutpoints.size(), 2U);
  for (const auto &C : R.Inference.Plan->Cutpoints) {
    ASSERT_EQ(C.OriginalGuards.size(), 1U);
    EXPECT_EQ(C.OriginalGuards[0].Location.Space, LowIRLoopSpace::Frame);
    EXPECT_EQ(C.OriginalGuards[0].Location.Offset, uint64_t(-8));
  }
}

TEST(BinaryLowIRLoopInference, NativeSelectorsSeparateThreeDomains) {
  // The three arms set r10 to 0, 1 or 2, then share one arbitrary-count loop.
  Program P({0x83, 0xff, 0,    0x74, 13,   0x83, 0xff, 1,    0x74, 13,   0x41,
             0xba, 2,    0,    0,    0,    0xeb, 11,   0x45, 0x31, 0xd2, 0xeb,
             6,    0x41, 0xba, 1,    0,    0,    0,    0xe3, 9,    0x48, 0x01,
             0xc8, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf5, 0xc3});
  P.Options.ControlRegisters = {{x86reg::R10, 8}};
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto R = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(R.Inference.inferred()) << R.Inference.Diagnostic;
  ASSERT_TRUE(R.proved()) << R.Refinement.Proof.Diagnostic;
  ASSERT_EQ(R.Inference.Plan->Cutpoints.size(), 3U);
  EXPECT_EQ(R.Refinement.Proof.LoopInitiations, 3U);
  bool Conjunction = false;
  for (const auto &C : R.Inference.Plan->Cutpoints) {
    unsigned Bits = 0;
    for (const auto &G : C.OriginalGuards)
      Bits += std::popcount(G.Mask);
    Conjunction |= Bits > 1;
  }
  EXPECT_TRUE(Conjunction);
}

TEST(BinaryLowIRLoopInference, NativeSelectorsRefuseInseparableTemplates) {
  // Two real loop addresses split at unsigned rdi < 5. Their reconstructed
  // states have no opposite literal bits: the second interval leaves every
  // rdi bit variable and both arms normalize the arithmetic flags.
  Program P({0x48, 0x83, 0xff, 5,    0x72, 15,   0x31, 0xc0, 0xe3,
             10,   0x48, 0x8d, 4,    8,    0x48, 0x8d, 0x49, 0xff,
             0xeb, 0xf4, 0xc3, 0x31, 0xc0, 0xe3, 10,   0x48, 0x8d,
             4,    8,    0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf4, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(Good.proved())
      << Good.Inference.Diagnostic << ": " << Good.Refinement.Proof.Diagnostic;
  ASSERT_EQ(Good.Inference.Plan->Cutpoints.size(), 2U);
  const auto &A = Good.Inference.Plan->Cutpoints[0];
  const auto &B = Good.Inference.Plan->Cutpoints[1];
  for (auto &O : Recovery.Origins)
    if (O.ResidualAddress == B.CandidateAddress)
      O.NativeInstruction.Address = A.OriginalAddress;
  const auto Bad = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  EXPECT_EQ(Bad.Inference.Status, LowIRLoopInferenceStatus::Unsupported)
      << Bad.Inference.Diagnostic;
  EXPECT_NE(Bad.Inference.Diagnostic.find("no proved literal-bit selector"),
            std::string::npos);
  EXPECT_FALSE(Bad.Inference.Plan);
  EXPECT_FALSE(Bad.Refinement.Certificate);
  EXPECT_EQ(Bad.Refinement.Proof.Operations, 0U);
}

TEST(BinaryLowIRLoopInference, NativeSelectorsKeepIndependentBudgets) {
  auto P = repeatedNativeContexts();
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Check = [&](const LowIRLoopInferenceLimits &I,
                         const LowIRRefinementLimits &L = {}) {
    return inferAndCheckBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                                  Recovery, P.Contract,
                                                  Witness::LiftedBits, L, I);
  };
  const auto Good = Check({});
  ASSERT_TRUE(Good.proved())
      << Good.Inference.Diagnostic << ": " << Good.Refinement.Proof.Diagnostic;
  LowIRLoopInferenceLimits Exact;
  Exact.Execution.MaxOperations = Good.Inference.Operations;
  Exact.Execution.MaxSolverQueries = Good.Inference.SolverQueries;
  Exact.Execution.MaxPaths = Good.Inference.ScheduledPaths;
  Exact.MaxCutSelectionWork = Good.Inference.CutSelectionWork;
  ASSERT_TRUE(Check(Exact).proved());
  for (unsigned Limit = 0; Limit != 3; ++Limit) {
    auto Short = Exact;
    if (Limit == 0)
      --Short.MaxCutSelectionWork;
    else if (Limit == 1)
      --Short.Execution.MaxOperations;
    else
      --Short.Execution.MaxSolverQueries;
    const auto R = Check(Short);
    EXPECT_EQ(R.Inference.Status, LowIRLoopInferenceStatus::BudgetExceeded);
    EXPECT_FALSE(R.Inference.Plan);
    EXPECT_FALSE(R.Refinement.Certificate);
  }
  LowIRRefinementLimits ExactProof;
  ExactProof.Execution.MaxOperations = Good.Refinement.Proof.Operations;
  ExactProof.Execution.MaxSolverQueries = Good.Refinement.Proof.SolverQueries;
  ExactProof.Execution.MaxObservations = Good.Refinement.Proof.Observations;
  ExactProof.MaxTerminalPairs = Good.Refinement.Proof.TerminalPairs;
  ASSERT_TRUE(Check(Exact, ExactProof).proved());
  --ExactProof.Execution.MaxSolverQueries;
  const auto ShortProof = Check(Exact, ExactProof);
  EXPECT_TRUE(ShortProof.Inference.inferred());
  refused(ShortProof.Refinement, Status::BudgetExceeded);
}

TEST(BinaryLowIRLoopInference, NativeSelectorsDoNotTrustOriginsOrBodies) {
  auto P = repeatedNativeContexts();
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Good = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(Good.proved())
      << Good.Inference.Diagnostic << ": " << Good.Refinement.Proof.Diagnostic;
  auto Wrong = Recovery;
  const auto &Cut = Good.Inference.Plan->Cutpoints.front();
  ASSERT_NE(Cut.OriginalAddress, Entry + 3);
  for (auto &O : Wrong.Origins)
    if (O.ResidualAddress == Cut.CandidateAddress)
      O.NativeInstruction.Address = Entry + 3; // Before the entry AND.
  const auto BadOrigin = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Wrong, P.Contract);
  EXPECT_FALSE(BadOrigin.proved());
  EXPECT_FALSE(BadOrigin.Refinement.Certificate);
  // The recovered candidate and selectors cannot authorize a changed native
  // update, even though candidate-only inference still succeeds.
  P.Image.Segments.front().Data[22] = 0xfe;
  const auto BadBody = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  EXPECT_TRUE(BadBody.Inference.inferred()) << BadBody.Inference.Diagnostic;
  EXPECT_FALSE(BadBody.proved());
  EXPECT_FALSE(BadBody.Refinement.Certificate);
}

TEST(BinaryLowIRLoopInference, NativeByteCountersKeepCachedComparisons) {
  for (bool Lagged : {false, true}) {
    SCOPED_TRACE(Lagged);
    // MOVZX R8D,DIL; MOVZX R9D,SIL; two nested byte counters starting at
    // zero. Each header tests a cached SETE result. ADD EAX,EDX observes the
    // inner counter. Both input bytes are unconstrained; all 16 GPRs and seven
    // flags remain observed.
    Program P({0x44, 0x0f, 0xb6, 0xc7, 0x44, 0x0f, 0xb6, 0xce, 0x31, 0xc0, 0x31,
               0xc9, 0x44, 0x38, 0xc1, 0x41, 0x0f, 0x94, 0xc2, 0x45, 0x84, 0xd2,
               0x75, 0x26, 0x31, 0xd2, 0x44, 0x38, 0xca, 0x41, 0x0f, 0x94, 0xc3,
               0x45, 0x84, 0xdb, 0x75, 0x0d, 0x01, 0xd0, 0xfe, 0xc2, 0x44, 0x38,
               0xca, 0x41, 0x0f, 0x94, 0xc3, 0xeb, 0xee, 0xfe, 0xc1, 0x44, 0x38,
               0xc1, 0x41, 0x0f, 0x94, 0xc2, 0xeb, 0xd5, 0xc3});
    auto &Bytes = P.Image.Segments.front().Data;
    if (Lagged)
      // Move each INC after CMP/SETE. A stale comparison permits the byte
      // counter to wrap before exit, so it cannot be treated as a fresh flag.
      for (unsigned Begin : {0x28, 0x33})
        std::rotate(Bytes.begin() + Begin, Bytes.begin() + Begin + 2,
                    Bytes.begin() + Begin + 9);
    const auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    EXPECT_TRUE(P.Options.EntryConstants.empty());
    EXPECT_TRUE(P.Contract.EntryConstants.empty());
    const auto Check = [&] {
      return inferAndCheckBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                                    Recovery, P.Contract);
    };
    const auto Good = Check();
    ASSERT_TRUE(Good.Inference.inferred()) << Good.Inference.Diagnostic;
    ASSERT_TRUE(Good.proved()) << Good.Refinement.Proof.Diagnostic;
    EXPECT_GE(Good.Refinement.Proof.RankingChecks, 2U);
    // Candidate-only inference cannot authorize a changed original body.
    Bytes[0x26] = 0x29; // SUB EAX,EDX, instead of ADD EAX,EDX.
    const auto Bad = Check();
    ASSERT_TRUE(Bad.Inference.inferred()) << Bad.Inference.Diagnostic;
    refused(Bad.Refinement, Status::Different);
  }
}

TEST(BinaryLowIRLoopInference, NativeEqualityExitsUseInductiveCounterBounds) {
  // Two independent unsigned loops, both exiting on equality with an input.
  // xor eax,eax; xor ecx,ecx; outer: cmp rcx,r8; je done; xor edx,edx;
  // inner: cmp rdx,r9; je next; lea rax,[rax+rdx]; inc rdx; jmp inner;
  // next: inc rcx; jmp outer; done: ret.
  Program P({0x31, 0xc0, 0x31, 0xc9, 0x4c, 0x39, 0xc1, 0x74, 0x15, 0x31, 0xd2,
             0x4c, 0x39, 0xca, 0x74, 0x09, 0x48, 0x8d, 0x04, 0x10, 0x48, 0xff,
             0xc2, 0xeb, 0xf2, 0x48, 0xff, 0xc1, 0xeb, 0xe6, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Check = [&] {
    return inferAndCheckBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                                  Recovery, P.Contract);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.Inference.inferred()) << Good.Inference.Diagnostic;
  ASSERT_TRUE(Good.proved()) << Good.Refinement.Proof.Diagnostic;
  EXPECT_GT(Good.Inference.PredicateNodes, 0U);
  EXPECT_GE(Good.Refinement.Proof.RankingChecks, 2U);
  // A recovered plan must not certify the original after changing either
  // increasing counter to a decreasing one.
  for (unsigned Offset : {22, 27}) {
    P.Image.Segments[0].Data[Offset] += 8;
    const auto Bad = Check();
    ASSERT_TRUE(Bad.Inference.inferred()) << Bad.Inference.Diagnostic;
    refused(Bad.Refinement, Status::Different);
    P.Image.Segments[0].Data[Offset] -= 8;
  }
}

TEST(BinaryLowIRLoopInference, OriginHintsAndRecoveryStatusAreUntrusted) {
  Program P(
      {0xe3, 9, 0x48, 0x01, 0xc8, 0x48, 0x8d, 0x49, 0xff, 0xeb, 0xf5, 0xc3});
  const auto Good = P.recover();
  ASSERT_TRUE(Good.complete()) << Good.Diagnostic;
  for (unsigned Kind = 0; Kind != 4; ++Kind) {
    auto Recovery = Good;
    if (Kind == 0)
      Recovery.Status = SpecializationStatus::Unsupported;
    if (Kind == 1)
      Recovery.Origins.clear();
    if (Kind == 2)
      Recovery.Origins.insert(Recovery.Origins.end(), Good.Origins.begin(),
                              Good.Origins.end());
    if (Kind == 3)
      for (auto &Origin : Recovery.Origins)
        Origin.NativeInstruction.Address += 0x1000;
    const auto R = inferAndCheckBinaryLowIRLoopRefinement(
        P.Image, Entry, P.Options, Recovery, P.Contract);
    EXPECT_FALSE(R.proved()) << "kind=" << Kind;
    EXPECT_FALSE(R.Refinement.Certificate);
    EXPECT_FALSE(R.Refinement.Proof.Certificate);
    EXPECT_EQ(R.Inference.inferred(), Kind == 3) << R.Inference.Diagnostic;
  }
}

TEST(BinaryLowIRRefinement, MandatorySystemFlagsRejectCandidateMutation) {
  // pushfq; pop rax; push rax; popfq; ret.
  Program P({0x9c, 0x58, 0x50, 0x9d, 0xc3});
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  ASSERT_TRUE(P.check(Recovery.Residual).proved());
  P.Contract.ReturnRegisters.clear();
  P.Contract.ObserveWrittenFrameBytes = false;
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::INTRINSIC && O.NumInputs == 2) {
        O.Inputs[1] = NdVar::scalar(2, 8);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Recovery.Residual), Status::Different);
}

TEST(BinaryLowIRRefinement, NativeEntryAndImageContractsRemainMandatory) {
  Program P({0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  P.Options.X64CetDisabled = false;
  refused(P.check(Recovery.Residual), Status::Unsupported);
  P.Options.X64CetDisabled = true;
  P.Contract.X64FlagsProfile.reset();
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Contract.X64FlagsProfile = P.Options.X64FlagsProfile;
  P.Contract.EntryConstants.push_back({NdVar::reg(x86reg::RAX, 8), 0});
  refused(P.check(Recovery.Residual), Status::Invalid);
  P.Options.X64FlagsProfile.reset();
  P.Contract.X64FlagsProfile.reset();
  refused(P.check(Recovery.Residual), Status::Unsupported);
}

TEST(BinaryLowIRRefinement, OriginalAndCandidateImmutableReadsHaveEvidence) {
  // mov rax,[rip+0x2ff9]; ret. An independent read-only table begins at 0x4000.
  Program P({0x48, 0x8b, 0x05, 0xf9, 0x2f, 0, 0, 0xc3});
  Segment Table;
  Table.VA = 0x4000;
  Table.Flags = SegmentFlags::Readable;
  Table.Data = {11, 22, 33, 44, 55, 66, 77, 88, 99};
  Table.Size = Table.FileSz = Table.Data.size();
  P.Image.Segments.push_back(Table);
  auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto Folded = P.check(Recovery.Residual);
  ASSERT_TRUE(Folded.proved()) << Folded.Proof.Diagnostic;
  ASSERT_EQ(Folded.Certificate->Reads.size(), 1U);
  bool Changed = false;
  for (auto &B : Recovery.Residual.Blocks)
    for (auto &O : B.Ops)
      if (O.Output == NdVar::reg(x86reg::RAX, 8)) {
        O.Opcode = NdOp::LOAD;
        O.NumInputs = 1;
        O.Inputs[0] = NdVar::scalar(0x4000, 8);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  const auto Read = P.check(Recovery.Residual);
  ASSERT_TRUE(Read.proved()) << Read.Proof.Diagnostic;
  ASSERT_EQ(Read.Certificate->Reads.size(), 2U);
  for (const auto &R : Read.Certificate->Reads) {
    EXPECT_EQ(R.Address, 0x4000U);
    EXPECT_EQ(R.Bytes.size(), 8U);
    EXPECT_FALSE(R.Evidence.empty());
  }
  EXPECT_NE(Folded.Certificate->InputDigest, Read.Certificate->InputDigest);
  P.Image.Segments.back().Flags =
      P.Image.Segments.back().Flags | SegmentFlags::Writable;
  refused(P.check(Recovery.Residual), Status::Unsupported);
}

TEST(BinaryLowIRRefinement, TrapsAndUnauditedOriginalArmsAreNotPruned) {
  // xor eax,eax; jz done; rcl eax,1; done: ret. Ordinary recovery prunes RCL,
  // but selected-value refinement still audits the complete direct graph.
  Program P({0x31, 0xc0, 0x74, 2, 0xd1, 0xd0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  refused(P.check(Recovery.Residual), Status::Unsupported);
  Program Trap({0xcc});
  refused(Trap.check(Recovery.Residual), Status::ContractViolation);
}

TEST(BinaryLowIRRefinement, CandidateMustRestoreStackAndOriginalReturnSlot) {
  Program P({0x58, 0x50, 0xc3}); // pop rax; push rax; ret.
  // Ordinary recovery deliberately refuses writes to this slot. Supply an
  // explicit candidate from the independently audited native instructions.
  const auto Native =
      checkBinaryUndefinedIndependence(P.Image, Entry, P.Options, P.Contract);
  ASSERT_TRUE(Native.proved()) << Native.Proof.Diagnostic;
  LowFunc Candidate;
  Candidate.Entry = Entry;
  LowBlock Block;
  Block.Id = 0;
  Block.StartAddr = Entry;
  for (const auto &I : Native.Certificate->Instructions) {
    auto Boundary = I.Origin;
    Boundary.FirstOp = Block.Ops.size();
    Block.InstructionBoundaries.push_back(Boundary);
    Block.Ops.insert(Block.Ops.end(), I.Ops.begin(), I.Ops.end());
    Block.EndAddr = I.Origin.Address + I.Origin.Size;
  }
  Candidate.Blocks.push_back(std::move(Block));
  const auto Good = P.check(Candidate);
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  bool Changed = false;
  for (auto &B : Candidate.Blocks)
    for (auto &O : B.Ops)
      if (O.Opcode == NdOp::STORE) {
        O.Inputs[O.NumInputs - 1] = NdVar::scalar(0, 8);
        Changed = true;
      }
  ASSERT_TRUE(Changed);
  refused(P.check(Candidate), Status::ContractViolation);
}
Program partitionedNativeTargets(unsigned Branches) {
  Program P({});
  auto &Code = P.Image.Segments.front();
  const auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Code.Data.insert(Code.Data.end(), Bytes);
  };
  const auto Imm = [&](uint32_t V) {
    for (unsigned I = 0; I < 4; ++I)
      Code.Data.push_back(V >> (8 * I));
  };
  const auto Pad = [&](unsigned Offset) { Code.Data.resize(Offset, 0xcc); };
  const unsigned First = 0x100, Leaves = 0x400;
  Emit({0x89, 0xf8, 0x83, 0xe0, static_cast<uint8_t>(Branches - 1), 0x48, 0xc1,
        0xe0, 7, 0x48, 0x05});
  Imm(Entry + First);
  Emit({0xff, 0xe0});
  for (unsigned I = 0; I < Branches; ++I) {
    Pad(First + 128 * I);
    Emit({static_cast<uint8_t>(I == 3 ? 0x4c : 0x48), 0x89,
          static_cast<uint8_t>(I == 0   ? 0xf0
                               : I == 1 ? 0xd0
                               : I == 2 ? 0xc8
                                        : 0xc0),
          0x48, 0x8d, 0x04, 0x40});
    Emit({0x48, 0x31, 0xf8, 0x48, 0xc1, 0xc0, static_cast<uint8_t>(I + 3), 0x83,
          0xe0, 1, 0x48, 0xc1, 0xe0, 5, 0x48, 0x05});
    Imm(Entry + Leaves + 64 * I);
    Emit({0xff, 0xe0});
  }
  for (unsigned I = 0; I < 2 * Branches; ++I) {
    Pad(Leaves + 32 * I);
    Emit({0xb8, 7, 0, 0, 0, 0xc3});
  }
  Code.Size = Code.FileSz = Code.Data.size();
  P.Options.ControlRegisters = {{x86reg::RAX, 8}};
  return P;
}

TEST(BinaryLowIRRefinement, PartitionedCoverageKeepsAllNativeAndCandidateArms) {
  for (unsigned Branches : {2U, 4U}) {
    SCOPED_TRACE(Branches);
    auto P = partitionedNativeTargets(Branches);
    const auto Recovery = P.recover();
    ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
    LowIRRefinementLimits Limits;
    Limits.Execution.Solver.Blast.MaxGates = 512;
    const auto Good = P.check(Recovery.Residual, Witness::LiftedBits, Limits);
    ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
    const auto Paths = 2 * Branches;
    EXPECT_EQ(Good.Proof.OriginalPaths, Paths);
    EXPECT_EQ(Good.Proof.CandidatePaths, Paths);
    EXPECT_EQ(Good.Proof.TerminalPairs, Paths * Paths);
    EXPECT_EQ(Good.Proof.SolverQueries, Branches == 2 ? 88U : 270U);
    Limits.Execution.MaxSolverQueries = Good.Proof.SolverQueries;
    ASSERT_TRUE(
        P.check(Recovery.Residual, Witness::LiftedBits, Limits).proved());
    --Limits.Execution.MaxSolverQueries;
    refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
            Status::BudgetExceeded);
    Limits.Execution.MaxSolverQueries = Good.Proof.SolverQueries;
    Limits.MaxTerminalPairs = Good.Proof.TerminalPairs - 1;
    refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
            Status::BudgetExceeded);
    Limits.MaxTerminalPairs = Good.Proof.TerminalPairs;
    Limits.Execution.MaxIndirectTargets = 1;
    refused(P.check(Recovery.Residual, Witness::LiftedBits, Limits),
            Status::BudgetExceeded);
    Limits.Execution.MaxIndirectTargets = 8;
    // The lowest target is visited last by the native worklist. Earlier
    // successful coverage and state pairs cannot certify this changed return.
    auto &Bytes = P.Image.Segments.front().Data;
    Bytes[0x401] = 9;
    const auto Wrong = P.check(Recovery.Residual, Witness::LiftedBits, Limits);
    refused(Wrong, Status::Different);
    EXPECT_GT(Wrong.Proof.TerminalPairs, (Paths - 1) * Paths);
    Bytes[0x401] = 7;
    // A missing reached target must not shrink the certified input domain.
    Bytes.resize(0x400 + 32 * (Paths - 1));
    P.Image.Segments.front().Size = P.Image.Segments.front().FileSz =
        Bytes.size();
    const auto Missing =
        P.check(Recovery.Residual, Witness::LiftedBits, Limits);
    EXPECT_FALSE(Missing.proved());
    EXPECT_FALSE(Missing.Certificate);
    EXPECT_FALSE(Missing.Proof.Certificate);
  }
}

TEST(BinaryLowIRRefinement,
     OpaquePreservationCannotUpgradeSelectedUndefinedChoices) {
  // CMP establishes AF=0; XOR then leaves AF undefined. LiftedBits keeps the
  // old zero and ZeroBits selects zero. Another architectural choice reaches
  // the vector write through LAHF and TEST. Entry flags remain arbitrary.
  Program P({0x39, 0xc0, 0x31, 0xc0, 0x9f, 0xf6, 0xc4, 0x10, 0x75, 1, 0xc3,
             0x66, 0x0f, 0xef, 0xf6, 0xc3});
  P.Contract.RetainUnauditedNativeBoundaries = true;
  P.Contract.NativePreservedState = LowIRNativePreservationRequirement{
      LowPreservedStateSet::LegacyIntegerOpaqueV1,
      LowIRNativePreservationQuantifier::SelectedWitness};
  // Recover an independently safe candidate. Ordinary specialization also
  // visits the vector arm and cannot model its 128-bit operation. The native
  // proof must establish that this arm is unreachable for each named witness.
  auto Safe = P;
  std::fill_n(Safe.Image.Segments.front().Data.begin() + 11, 4, 0x90);
  const auto Recovery = Safe.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  for (auto W : {Witness::LiftedBits, Witness::ZeroBits}) {
    SCOPED_TRACE(static_cast<unsigned>(W));
    const auto R = P.check(Recovery.Residual, W);
    ASSERT_TRUE(R.proved()) << R.Proof.Diagnostic;
    ASSERT_TRUE(R.Certificate->Relation.NativePreservation);
    EXPECT_EQ(R.Certificate->Relation.NativePreservation->Quantifier,
              LowIRNativePreservationQuantifier::SelectedWitness);
    ASSERT_EQ(R.Certificate->Relation.NativeAuditBoundaries.size(), 1U);
    EXPECT_EQ(
        R.Certificate->Relation.NativeAuditBoundaries.front().Kind,
        LowIRNativeAuditBoundaryKind::MissingUndefinedOutputsAndPreservedState);
    P.Contract.NativePreservedState->Quantifier =
        LowIRNativePreservationQuantifier::AllUndefinedChoices;
    refused(P.check(Recovery.Residual, W), Status::Unsupported);
    P.Contract.NativePreservedState->Quantifier =
        LowIRNativePreservationQuantifier::SelectedWitness;
  }
  P.Contract.NativePreservedState->Quantifier =
      LowIRNativePreservationQuantifier::AllUndefinedChoices;
  const auto All =
      checkBinaryUndefinedIndependence(P.Image, Entry, P.Options, P.Contract);
  EXPECT_EQ(All.Proof.Status, LowIRIndependenceStatus::Dependent)
      << All.Proof.Diagnostic;
  EXPECT_FALSE(All.Certificate);
  P.Contract.NativePreservedState->Quantifier =
      LowIRNativePreservationQuantifier::SelectedWitness;
  // The safe chosen branch was essential, not permission to execute PXOR.
  P.Image.Segments.front().Data[8] = 0x74;
  refused(P.check(Recovery.Residual), Status::Unsupported);
}

TEST(BinaryLowIRLoopInference, OpaquePreservationNeedsEveryInductiveSource) {
  auto P = repeatedNativeContexts();
  P.Contract.NativePreservedState = LowIRNativePreservationRequirement{
      LowPreservedStateSet::LegacyIntegerOpaqueV1,
      LowIRNativePreservationQuantifier::SelectedWitness};
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  const auto R = inferAndCheckBinaryLowIRLoopRefinement(
      P.Image, Entry, P.Options, Recovery, P.Contract);
  ASSERT_TRUE(R.proved()) << R.Inference.Diagnostic
                          << R.Refinement.Proof.Diagnostic;
  ASSERT_GE(R.Inference.Plan->Cutpoints.size(), 2U);
  ASSERT_TRUE(R.Refinement.Certificate->Relation.NativePreservation);
  const auto &Proof = R.Refinement.Proof;
  EXPECT_GT(Proof.LoopTransitions, 1U);
  EXPECT_EQ(R.Refinement.Certificate->Relation.NativePreservation->Quantifier,
            LowIRNativePreservationQuantifier::SelectedWitness);
  EXPECT_EQ(R.Refinement.Certificate->Relation.NativePreservation->Instructions,
            R.Refinement.Certificate->Relation.OriginalInstructions.size());
  const auto Check = [&](const LowIRLoopRefinementPlan &Plan,
                         const LowIRRefinementLimits &Limits = {}) {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options,
                                          Recovery.Residual, P.Contract, Plan,
                                          Witness::LiftedBits, Limits);
  };
  const auto Again = Check(*R.Inference.Plan);
  ASSERT_TRUE(Again.proved()) << Again.Proof.Diagnostic;
  EXPECT_EQ(Again.Certificate->InputDigest,
            R.Refinement.Certificate->InputDigest);
  auto Bad = *R.Inference.Plan;
  Bad.Cutpoints.back().Rank = {NdVar::scalar(0, 8)};
  refused(Check(Bad), Status::Different);
  LowIRRefinementLimits Short;
  Short.Execution.MaxSolverQueries = Again.Proof.SolverQueries - 1;
  refused(Check(*R.Inference.Plan, Short), Status::BudgetExceeded);
  Short = {};
  Short.Execution.MaxOperations = Again.Proof.Operations - 1;
  refused(Check(*R.Inference.Plan, Short), Status::BudgetExceeded);
  P.Contract.NativePreservedState->Quantifier =
      LowIRNativePreservationQuantifier::AllUndefinedChoices;
  refused(Check(*R.Inference.Plan), Status::Unsupported);
}

TEST(BinaryLowIRLoopRefinement,
     OpaquePreservationBindsLaterSourceBytesWithIdenticalLowIR) {
  // Two explicit source segments, with no instruction executed before the
  // entry cut: NOP [RAX]; NOP [RAX]; RET. The second source alone changes
  // below.
  Program P({0x0f, 0x1f, 0x00, 0x0f, 0x1f, 0x00, 0xc3});
  P.Contract.NativePreservedState = LowIRNativePreservationRequirement{
      LowPreservedStateSet::LegacyIntegerOpaqueV1,
      LowIRNativePreservationQuantifier::SelectedWitness};
  Decoder D;
  ASSERT_TRUE(D.init(Arch::X64));
  const auto &Bytes = P.Image.Segments.front().Data;
  LowFunc Candidate;
  Candidate.Entry = Entry;
  for (size_t Offset = 0; Offset != Bytes.size();) {
    DecodedInsn I{};
    ASSERT_GT(D.decodeOneForLift(Bytes.data() + Offset, Bytes.size() - Offset,
                                 Entry + Offset, I),
              0);
    LowBlock B;
    B.Id = Candidate.Blocks.size();
    B.StartAddr = Entry + Offset;
    B.EndAddr = B.StartAddr + I.Size;
    D.liftToLow(I, B.Ops);
    LowInstructionBoundary Origin;
    Origin.Address = B.StartAddr;
    Origin.Size = I.Size;
    Origin.OpCount = B.Ops.size();
    if (D.returnsToCaller(I).value_or(false)) {
      Origin.Control = LowInstructionControl::Return;
      Origin.ControlFlags = LowInstructionControlFlag::Return;
    } else {
      B.Succs.push_back(B.Id + 1);
    }
    B.InstructionBoundaries.push_back(Origin);
    Candidate.Blocks.push_back(std::move(B));
    Offset += I.Size;
  }
  LowIRLoopRefinementPlan Plan;
  for (unsigned N = 0; N != 2; ++N) {
    LowIRLoopCutpoint Cut;
    Cut.OriginalAddress = Cut.CandidateAddress = Entry + 3 * N;
    Cut.Rank = {NdVar::scalar(1 - N, 8)};
    Plan.Cutpoints.push_back(Cut);
  }
  const auto Check = [&] {
    return checkBinaryLowIRLoopRefinement(P.Image, Entry, P.Options, Candidate,
                                          P.Contract, Plan);
  };
  const auto Good = Check();
  ASSERT_TRUE(Good.proved()) << Good.Proof.Diagnostic;
  ASSERT_TRUE(Good.Certificate->Relation.NativePreservation);
  const auto &F = *Good.Certificate->Relation.NativePreservation;
  EXPECT_EQ(F.Instructions, 3U);
  EXPECT_EQ(F.Instructions,
            Good.Certificate->Relation.OriginalInstructions.size());
  // NOP [RCX] has exactly the same LowIR as NOP [RAX], but a different native
  // byte binding. No other source segment or candidate operation changes.
  P.Image.Segments.front().Data[5] = 0x01;
  DecodedInsn Changed{};
  ASSERT_EQ(D.decodeOneForLift(Bytes.data() + 3, 3, Entry + 3, Changed), 3);
  std::vector<LowOp> ChangedOps;
  D.liftToLow(Changed, ChangedOps);
  EXPECT_EQ(lowUndefinedOperationDigest(ChangedOps),
            lowUndefinedOperationDigest(Candidate.Blocks[1].Ops));
  const auto Later = Check();
  ASSERT_TRUE(Later.proved()) << Later.Proof.Diagnostic;
  ASSERT_TRUE(Later.Certificate->Relation.NativePreservation);
  EXPECT_EQ(Later.Certificate->Relation.NativePreservation->Instructions,
            F.Instructions);
  EXPECT_NE(Later.Certificate->Relation.NativePreservation->ExecutionDigest,
            F.ExecutionDigest);
}

TEST(BinaryLowIRRefinement,
     OpaqueFactDoesNotReplaceTrueEntryScalarPreservation) {
  Program P({0x48, 0xff, 0xc3, 0xc3}); // inc rbx; ret.
  P.Contract.NativePreservedState = LowIRNativePreservationRequirement{
      LowPreservedStateSet::LegacyIntegerOpaqueV1,
      LowIRNativePreservationQuantifier::SelectedWitness};
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  ASSERT_TRUE(P.check(Recovery.Residual).proved());
  P.Contract.PreservedRegisters.push_back({x86reg::RBX, 8});
  refused(P.check(Recovery.Residual), Status::ContractViolation);
}

TEST(BinaryLowIRRefinement, StaticAPIsCannotAuthorizeOpaqueArchitecturalState) {
  Program P({0xb8, 7, 0, 0, 0, 0xc3});
  const auto Recovery = P.recover();
  ASSERT_TRUE(Recovery.complete()) << Recovery.Diagnostic;
  P.Contract.NativePreservedState.emplace();
  const auto Independence =
      checkLowIRUndefinedIndependence(Recovery.Residual, {}, P.Contract);
  EXPECT_EQ(Independence.Status, LowIRIndependenceStatus::Unsupported);
  EXPECT_FALSE(Independence.Certificate);
  EXPECT_NE(Independence.Diagnostic.find("native preservation"),
            std::string::npos);
  const auto Relation = checkLowIRRefinement(Recovery.Residual, {},
                                             Recovery.Residual, P.Contract);
  EXPECT_EQ(Relation.Status, Status::Unsupported);
  EXPECT_FALSE(Relation.Certificate);
  EXPECT_NE(Relation.Diagnostic.find("native preservation"), std::string::npos);
}

} // namespace
