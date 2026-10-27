//===- RegistrationSynchronizationStateTests.cpp - Runtime scalar inputs -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Distinguish initialized opaque arguments from stale bytes and frame borrows.
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/lift/X86Regs.h"

#include <cstdint>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::registration_state;
using namespace neverd::registration_test;

enum class Change {
  None,
  UnknownScalar,
  HalfWords,
  MissingWord,
  PartialWord,
  StaleAllocation,
  FrameScalar,
  PartialFrameScalar,
  OwnImportWrite,
  OtherImportWrite,
  UnrelatedImageWrite,
  UnknownStore,
  WrongCall,
  UnknownCall,
  IntegerDestination,
  NullDestination,
  UnalignedDestination,
  FrameDestination,
  CookieDestination,
  ScopeDestination,
  ReadOnlyDestination,
  Count
};

LowFunc synchronizationFrame(bool Atomic, unsigned Protocol, Change Mutation) {
  auto F = makeJumpFrame();
  if (Protocol)
    initializeEH4JumpFrame(F, Protocol == 2);
  auto &Setup = F.Blocks[1];
  const unsigned Count = Atomic ? 3 : 1;
  const unsigned Scalar = Atomic ? 1 : 0;
  if (Mutation == Change::UnknownScalar || Mutation == Change::UnknownStore)
    emitOp(Setup, 0x1086, NdOp::LOAD, NdVar::tmp(99, 4),
           {NdVar::cst(0x5004, 4)});
  for (unsigned I = 0; I != Count; ++I) {
    if (Mutation == Change::MissingWord && I == Scalar)
      continue;
    emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(8, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(I * 4, 4)});
    auto Value = NdVar::cst(I == Scalar ? 1000 : 0, 4);
    if (Atomic && I == 0) {
      Value = NdVar::cst(Mutation == Change::NullDestination        ? 0
                         : Mutation == Change::UnalignedDestination ? 0x5001
                         : Mutation == Change::CookieDestination    ? 0x4000
                         : Mutation == Change::ScopeDestination
                             ? F.ExceptionMetadata->Registration->ScopeTableVA
                             : 0x5000,
                         4);
      if (Mutation != Change::IntegerDestination)
        Value.Provenance = ConstantAddressProvenance::DataAddress;
      if (Mutation == Change::FrameDestination)
        Value = NdVar::reg(x86reg::RSP, 4);
    }
    if (I == Scalar) {
      if (Mutation == Change::UnknownScalar)
        Value = NdVar::tmp(99, 4);
      if (Mutation == Change::PartialWord)
        Value.Size = 2;
      if (Mutation == Change::FrameScalar ||
          Mutation == Change::PartialFrameScalar)
        Value = NdVar::reg(x86reg::RSP, 4);
    }
    emitOp(Setup, 0x1086, NdOp::STORE, {}, {NdVar::tmp(8, 4), Value});
    if (I == Scalar && Mutation == Change::PartialFrameScalar)
      emitOp(Setup, 0x1086, NdOp::STORE, {},
             {NdVar::tmp(8, 4), NdVar::cst(0, 2)});
    if (I == Scalar && Mutation == Change::HalfWords) {
      Setup.Ops.back().Inputs[1].Size = 2;
      emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(8, 4),
             {NdVar::tmp(8, 4), NdVar::cst(2, 4)});
      emitOp(Setup, 0x1086, NdOp::STORE, {},
             {NdVar::tmp(8, 4), NdVar::cst(0, 2)});
    }
  }
  if (Mutation == Change::StaleAllocation) {
    emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(Count * 4, 4)});
    emitOp(Setup, 0x1086, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(Count * 4, 4)});
  }
  if (Mutation == Change::OwnImportWrite ||
      Mutation == Change::OtherImportWrite ||
      Mutation == Change::UnrelatedImageWrite ||
      Mutation == Change::UnknownStore)
    emitOp(Setup, 0x1086, NdOp::STORE, {},
           {Mutation == Change::UnknownStore
                ? NdVar::tmp(99, 4)
                : NdVar::cst(Mutation == Change::OwnImportWrite     ? 0x4200
                             : Mutation == Change::OtherImportWrite ? 0x4300
                                                                    : 0x4400,
                             4),
            NdVar::cst(0, 4)});
  if (Mutation == Change::UnknownCall)
    emitOp(Setup, 0x1086, NdOp::CALL, {}, {NdVar::cst(0x6000, 4)});
  Setup.InstructionBoundaries.back().OpCount = Setup.Ops.size() - 3;
  auto &Call = F.Blocks[2];
  Call.EndAddr = 0x1106;
  Call.Ops[0].Opcode = NdOp::INDIR_CALL;
  Call.Ops[0].Inputs[0] = NdVar::cst(0x4200, 4);
  Call.InstructionBoundaries[0].Size = 6;
  Call.InstructionBoundaries[0].Immediate = 0x4200;
  Call.InstructionBoundaries[0].ControlFlags |=
      LowInstructionControlFlag::Indirect;
  auto &Resume = F.Blocks[3];
  Resume.StartAddr = 0x1108;
  Resume.EndAddr = 0x110f;
  Resume.Ops.clear();
  Resume.Succs = {5};
  emitOp(Resume, 0x1108, NdOp::STORE, {},
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(37, 4)});
  Resume.InstructionBoundaries = {{0x1108, 7, 0, 1}};
  return F;
}

RegistrationStateAnalysis analyze(const LowFunc &F, bool Atomic,
                                  unsigned Protocol, Change Mutation,
                                  size_t Work = 0) {
  SEHJumpRuntime Runtime;
  Runtime.ImportSlots = {0x4200, 0x4300};
  auto Check = [Mutation](const LowOp &Op, size_t &) {
    return Mutation != Change::WrongCall && Op.Opcode == NdOp::INDIR_CALL &&
           Op.Addr == 0x1100;
  };
  if (Atomic)
    Runtime.Interlocked = Check;
  else
    Runtime.Sleep = Check;
  const auto Scope = F.ExceptionMetadata->Registration->ScopeTableVA;
  Runtime.ImageMemory = [Mutation, Scope](va_t Address, uint16_t Width,
                                          bool Write, size_t &) {
    return Mutation != Change::ReadOnlyDestination &&
           (Address == 0x5000 || Address == 0x4000 || Address == Scope) &&
           Width == 4 && Write;
  };
  const std::vector<RegistrationCalleeStackContract> Stacks = {
      {0x4200, Atomic ? 12u : 4u, true}, {0x4300, 4, true}};
  RegistrationStateSolver Solver(F, Protocol ? 0x4000 : 0, 0, nullptr, nullptr,
                                 &Stacks, nullptr, nullptr, nullptr, nullptr,
                                 &Runtime);
  return Solver.run(Work);
}

TEST(RegistrationSynchronization, ActualInitializedWordsAndOriginalStorage) {
  for (bool Atomic : {false, true})
    for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
      for (unsigned I = 0; I != unsigned(Change::Count); ++I) {
        if (!Atomic && I >= unsigned(Change::IntegerDestination))
          continue;
        const auto Mutation = Change(I);
        if (!Protocol && Mutation == Change::CookieDestination)
          continue;
        SCOPED_TRACE(::testing::Message()
                     << Atomic << ' ' << Protocol << ' ' << I);
        auto F = synchronizationFrame(Atomic, Protocol, Mutation);
        const auto Result = analyze(F, Atomic, Protocol, Mutation);
        const bool Expected = Mutation == Change::None ||
                              Mutation == Change::UnknownScalar ||
                              Mutation == Change::HalfWords ||
                              Mutation == Change::UnrelatedImageWrite;
        ASSERT_EQ(Result.SEHBoundCalls.size(), Expected ? 1u : 0u);
        if (!Expected)
          continue;
        EXPECT_TRUE(Result.Complete);
        const auto &Call = Result.SEHBoundCalls.front();
        EXPECT_EQ(
            Call.CalleeKind,
            Atomic ? RegistrationSEHBoundCall::Kind::InterlockedCompareExchange
                   : RegistrationSEHBoundCall::Kind::Sleep);
        EXPECT_FALSE(Call.ConstantReturn);
        EXPECT_EQ(Call.StackOffset, -192);
        ASSERT_EQ(Call.Arguments.size(), Atomic ? 3u : 1u);
        for (unsigned Index = 0; Index != Call.Arguments.size(); ++Index) {
          const auto &Arg = Call.Arguments[Index];
          if (Atomic && !Index) {
            EXPECT_EQ(Arg.ArgumentKind,
                      RegistrationSEHBoundArgument::Kind::ImagePointer);
            EXPECT_EQ(Arg.ImageVA, 0x5000u);
          } else {
            EXPECT_EQ(Arg.ArgumentKind,
                      RegistrationSEHBoundArgument::Kind::Scalar);
            EXPECT_EQ(Arg.Offset, -192 + int32_t(Index * 4));
          }
        }
        EXPECT_TRUE(analyze(F, Atomic, Protocol, Mutation,
                            limits::kMaxRegistrationEHStateWork)
                        .SEHBoundCalls.empty());
      }
}

TEST(RegistrationSynchronization, EveryIncomingPathMustInitializeTheArguments) {
  for (bool Atomic : {false, true})
    for (bool Missing : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Atomic << ' ' << Missing);
      auto F = synchronizationFrame(Atomic, 0, Change::UnknownScalar);
      auto Other = synchronizationFrame(
          Atomic, 0, Missing ? Change::MissingWord : Change::UnknownScalar);
      F.Blocks[4] = Other.Blocks[1];
      auto &Alternative = F.Blocks[4];
      Alternative.Id = 4;
      Alternative.StartAddr += 0x10;
      Alternative.EndAddr += 0x10;
      for (auto &Op : Alternative.Ops)
        Op.Addr += 0x10;
      for (auto &Boundary : Alternative.InstructionBoundaries)
        Boundary.Address += 0x10;
      F.Blocks[0].Succs = {1, 4};
      const auto Result = analyze(F, Atomic, 0, Change::UnknownScalar);
      EXPECT_EQ(Result.SEHBoundCalls.size(), Missing ? 0u : 1u);
    }
}
TEST(RegistrationSynchronization, ReturningCallRetiresItsOutgoingWords) {
  for (bool Atomic : {false, true}) {
    auto F = synchronizationFrame(Atomic, 0, Change::UnknownScalar);
    auto &Resume = F.Blocks[3];
    Resume.Ops.clear();
    emitOp(Resume, 0x1108, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(Atomic ? 12 : 4, 4)});
    emitOp(Resume, 0x110b, NdOp::COND_BR, {},
           {NdVar::cst(0x1100, 8), NdVar::tmp(99, 4)});
    Resume.InstructionBoundaries = {{0x1108, 3, 0, 1}, {0x110b, 4, 1, 1}};
    auto &Branch = Resume.InstructionBoundaries.back();
    Branch.Control = LowInstructionControl::Branch;
    Branch.ControlFlags = LowInstructionControlFlag::Branch |
                          LowInstructionControlFlag::Conditional;
    Resume.Succs = {2, 5};
    EXPECT_TRUE(
        analyze(F, Atomic, 0, Change::UnknownScalar).SEHBoundCalls.empty());
  }
}

TEST(RegistrationSynchronization,
     AtomicResultIsAnOpaqueScalarWithoutAFrameCoordinate) {
  for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = synchronizationFrame(true, 0, Change::None);
    auto &Resume = F.Blocks[3];
    Resume.Ops.clear();
    auto Value = NdVar::reg(x86reg::RAX, 4);
    if (Mutation == 1) {
      emitOp(Resume, 0x1108, NdOp::STORE, {},
             {NdVar::reg(x86reg::RSP, 4), Value});
      emitOp(Resume, 0x1108, NdOp::LOAD, NdVar::tmp(9, 4),
             {NdVar::reg(x86reg::RSP, 4)});
      Value = NdVar::tmp(9, 4);
    }
    if (Mutation == 2) {
      emitOp(Resume, 0x1108, NdOp::INT_ADD, NdVar::tmp(9, 4),
             {Value, NdVar::reg(x86reg::RBP, 4)});
      Value = NdVar::tmp(9, 4);
    }
    emitOp(Resume, 0x1108, NdOp::STORE, {},
           {Mutation == 3 ? Value : NdVar::cst(0x5004, 4),
            Mutation == 3 ? NdVar::cst(0, 4) : Value});
    Resume.InstructionBoundaries.back().OpCount = Resume.Ops.size();
    const auto Result = analyze(F, true, 0, Change::None);
    EXPECT_EQ(Result.Complete, Mutation < 2);
    EXPECT_EQ(Result.SEHBoundCalls.size(), Mutation < 2 ? 1u : 0u);
  }
}

} // namespace
