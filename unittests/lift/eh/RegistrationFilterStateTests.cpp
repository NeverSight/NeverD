//===- RegistrationFilterStateTests.cpp - Correlated SEH filter effects ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks search dispositions, written states and suspended finally frames.

#include "RegistrationFilterTestUtils.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

#include <utility>

using namespace neverd;
using namespace neverd::registration_filter_test;
using namespace neverd::registration_test;

TEST(RegistrationFilterState, CallbackActivatedScopeHasIndependentRoots) {
  const auto Result =
      analyze(callbackActivatedFrame(), Variant::TwoResumeMappings);
  ASSERT_TRUE(Result.Complete);
  ASSERT_GE(Result.Blocks.size(), 2u);
  const auto &Filter = Result.Blocks[Result.Blocks.size() - 2];
  const auto &Handler = Result.Blocks.back();
  EXPECT_TRUE(Filter.Reached);
  EXPECT_TRUE(Filter.CallbackOnly);
  EXPECT_EQ(Filter.levels(), (std::vector<int32_t>{2}));
  EXPECT_TRUE(Handler.Reached);
  EXPECT_FALSE(Handler.CallbackOnly);
  EXPECT_EQ(Handler.levels(), (std::vector<int32_t>{1}));
}

TEST(RegistrationFilterState, UnknownSearchCannotPublishCompleteRootProofs) {
  auto F = callbackActivatedFrame();
  F.Blocks[15].Ops.front().Inputs[0] = NdVar::reg(x86reg::RCX, 4);
  const auto Result = analyze(std::move(F), Variant::TwoResumeMappings);
  EXPECT_FALSE(Result.Complete);
  ASSERT_GE(Result.Blocks.size(), 2u);
  const auto &Filter = Result.Blocks[Result.Blocks.size() - 2];
  const auto &Handler = Result.Blocks.back();
  EXPECT_TRUE(!Filter.Reached || Filter.Unknown);
  EXPECT_TRUE(!Handler.Reached || Handler.Unknown);
}

TEST(RegistrationFilterState,
     PreservesDispositionAndWrittenStateAtSharedReturn) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (auto V :
         {Variant::Correlated, Variant::SameState, Variant::BothContinue,
          Variant::CapturedCursor, Variant::StoresAroundRaise,
          Variant::ReversedEdges, Variant::NegativeDisposition,
          Variant::SkipFilterBeforeResume, Variant::SkipTwoFiltersBeforeResume,
          Variant::TwoResumeMappings}) {
      SCOPED_TRACE(unsigned(V));
      SCOPED_TRACE(Protocol);
      const auto Result = analyze(V, Protocol);
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
      EXPECT_TRUE(Result.Complete);
      EXPECT_TRUE(Result.SEHStateStoresComplete);
      EXPECT_TRUE(Result.SEHJumpResumptionsComplete);
      EXPECT_EQ(Result.SEHJumpResumptions.size(), 1u);
      if (!Result.SEHJumpResumptions.empty()) {
        const auto &Resume = Result.SEHJumpResumptions.front();
        const int32_t Outside = Protocol ? -2 : -1;
        const std::vector<int32_t> Expected =
            V == Variant::SameState || V == Variant::StoresAroundRaise
                ? std::vector<int32_t>{0}
            : V == Variant::TwoResumeMappings ? std::vector<int32_t>{0, 1}
            : V == Variant::CapturedCursor ? std::vector<int32_t>{Outside}
                                           : std::vector<int32_t>{Outside, 0};
        EXPECT_EQ(Resume.ResumedLevels, Expected);
        std::vector<RegistrationSEHJumpState> Restores;
        for (int32_t Level : Expected) {
          const int32_t Frame =
              Level == 0 && V == Variant::SkipFilterBeforeResume       ? 1
              : Level == 0 && V == Variant::SkipTwoFiltersBeforeResume ? 3
                                                                       : Level;
          Restores.push_back(
              {Frame, Level, V == Variant::TwoResumeMappings ? 1 : 0});
        }
        if (V == Variant::TwoResumeMappings)
          Restores = {{2, 1, 1}, {3, 0, 1}};
        EXPECT_EQ(Resume.StateRestores, Restores);
      }
    }
}

TEST(RegistrationFilterState, SkippingAnotherScopeCannotReuseTheCallbackState) {
  const auto Result = analyze(Variant::SkipFilterAfterCallback);
  ASSERT_TRUE(Result.Complete);
  ASSERT_EQ(Result.SEHJumpResumptions.size(), 1u);
  const auto &Resume = Result.SEHJumpResumptions.front();
  EXPECT_EQ(Resume.ResumedLevels, (std::vector<int32_t>{-1}));
  EXPECT_EQ(Resume.StateRestores,
            (std::vector<RegistrationSEHJumpState>{{-1, -1}, {0, -1}}));
}

TEST(RegistrationFilterState, EveryFilterPathMustCloseTheInvocation) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (auto V :
         {Variant::RecurrentSelection, Variant::UnknownDisposition,
          Variant::EscapingSearch, Variant::BrokenStack, Variant::RecurrentArm,
          Variant::WrongStoreWidth, Variant::IncompleteCallInstruction}) {
      SCOPED_TRACE(unsigned(V));
      SCOPED_TRACE(Protocol);
      const auto Result = analyze(V, Protocol);
      EXPECT_FALSE(Result.Complete);
      EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
      EXPECT_TRUE(Result.SEHJumpResumptions.empty());
    }
}

TEST(RegistrationFilterState, EH4SearchKeepsBothCookiesIndependentOfState) {
  for (unsigned Protocol : {1u, 2u}) {
    SCOPED_TRACE(Protocol);
    for (va_t Cookie : {va_t(0), va_t(0x4004)}) {
      const auto Result = analyze(filterFrame(Variant::Correlated, Protocol),
                                  Variant::Correlated, Cookie);
      EXPECT_FALSE(Result.SecurityCookiesComplete);
      EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
    }
    for (int32_t Offset : {-40, -28, -29, -32, -33}) {
      SCOPED_TRACE(Offset);
      auto F = filterFrame(Variant::Correlated, Protocol);
      LowBlock Store;
      Store.Id = F.Blocks.size();
      Store.StartAddr = 0x1a00;
      Store.EndAddr = 0x1a07;
      Store.Succs = {7};
      emitOp(Store, 0x1a00, NdOp::INT_ADD, NdVar::tmp(0, 4),
             {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Offset), 4)});
      emitOp(Store, 0x1a00, NdOp::STORE, {},
             {NdVar::tmp(0, 4), NdVar::cst(0, 4)});
      Store.InstructionBoundaries = {{0x1a00, 7, 0, 2}};
      F.ExceptionMetadata->Registration->Scopes[1].FilterVA = 0x1a00;
      F.Blocks.push_back(std::move(Store));
      const auto Result = analyze(std::move(F), Variant::Correlated);
      const bool Valid = Offset == -40 || (Protocol == 1 && Offset <= -32);
      EXPECT_EQ(Result.Complete, Valid);
      EXPECT_EQ(Result.SEHJumpResumptionsComplete, Valid);
      EXPECT_EQ(Result.SEHJumpResumptions.empty(), !Valid);
    }
  }
}

TEST(RegistrationFilterState, HandlerSelectionAbandonsOnlyItsSuspendedPath) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (auto V : {Variant::SelectedHandler, Variant::OuterHandler,
                   Variant::HandlerOnly, Variant::HandlerCleanup}) {
      SCOPED_TRACE(Protocol);
      SCOPED_TRACE(unsigned(V));
      const auto Result = analyze(V, Protocol);
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
      ASSERT_TRUE(Result.Complete);
      ASSERT_TRUE(Result.SEHJumpResumptionsComplete);
      ASSERT_EQ(Result.SEHJumpResumptions.size(), 1u);
      const auto &Resume = Result.SEHJumpResumptions.front();
      EXPECT_EQ(Resume.SelectedScopes,
                (std::vector<uint32_t>{V == Variant::OuterHandler ? 0u : 1u}));
      const bool HandlerOnly =
          V == Variant::HandlerOnly || V == Variant::HandlerCleanup;
      const int32_t Outside = Protocol ? -2 : -1;
      EXPECT_EQ(Resume.ResumedLevels, HandlerOnly
                                          ? std::vector<int32_t>{}
                                          : std::vector<int32_t>{Outside});
      const std::vector<RegistrationSEHJumpState> Restored =
          HandlerOnly
              ? std::vector<RegistrationSEHJumpState>{}
              : std::vector<RegistrationSEHJumpState>{{Outside, Outside, 0}};
      EXPECT_EQ(Resume.StateRestores, Restored);
      if (V == Variant::HandlerCleanup) {
        const auto &Cleanup = Result.Blocks[Result.Blocks.size() - 1];
        EXPECT_TRUE(Cleanup.Reached);
        EXPECT_TRUE(Cleanup.CallbackOnly);
      }
    }
}

TEST(RegistrationFilterState,
     SelectedHandlerRequiresACompleteRaiseInstruction) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol) {
    SCOPED_TRACE(Protocol);
    auto F = filterFrame(Variant::HandlerOnly, Protocol);
    auto &Block = F.Blocks[9];
    const auto At = Block.Ops.end() - 1;
    LowOp Extra;
    Extra.Addr = 0x1850;
    Extra.Opcode = NdOp::COPY;
    Extra.Output = NdVar::reg(x86reg::RAX, 4);
    Extra.NumInputs = 1;
    Extra.Inputs[0] = NdVar::cst(0, 4);
    Block.Ops.insert(At, Extra);
    ++Block.InstructionBoundaries[1].OpCount;
    ++Block.InstructionBoundaries[2].FirstOp;
    for (unsigned I = 0; I != Block.Ops.size(); ++I)
      Block.Ops[I].Seq = I;
    const auto Result = analyze(std::move(F), Variant::HandlerOnly);
    EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
    EXPECT_TRUE(Result.SEHJumpResumptions.empty());
  }
}

TEST(RegistrationFilterState, CleanupJumpRetiresThePendingHandler) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (unsigned Mutation = 0; Mutation != 14; ++Mutation) {
      SCOPED_TRACE(Protocol);
      SCOPED_TRACE(Mutation);
      auto F = filterFrame(Variant::HandlerCleanup, Protocol);
      if (Mutation == 7)
        F.ExceptionMetadata->CodeRange.End = 0x1a00;
      if (Mutation == 8)
        F.ExceptionMetadata->CodeRange.End = 0x115b;
      auto Cleanup = F.Blocks[4];
      Cleanup.Id = F.Blocks.back().Id;
      const va_t Delta = 0x1a00 - Cleanup.StartAddr;
      Cleanup.StartAddr += Delta;
      Cleanup.EndAddr += Delta;
      Cleanup.Succs.clear();
      for (auto &Op : Cleanup.Ops)
        Op.Addr += Delta;
      for (auto &Boundary : Cleanup.InstructionBoundaries)
        Boundary.Address += Delta;
      if (Mutation == 1 || Mutation == 2)
        Cleanup.Ops[4].Inputs[1] = NdVar::cst(Mutation, 4);
      if (Mutation == 9)
        Cleanup.Ops[4].Inputs[1] = NdVar::reg(x86reg::RBP, 4);
      if (Mutation >= 10)
        Cleanup.Ops[4].Inputs[1] = NdVar::reg(x86reg::RAX, 4);
      if (Mutation == 3)
        Cleanup.Ops[1].Inputs[1] = NdVar::cst(uint32_t(-140), 4);
      if (Mutation == 4) {
        Cleanup.Ops[4].Inputs[0] = NdVar::reg(x86reg::RBP, 4);
      } else if (Mutation == 5) {
        emitOp(Cleanup, 0x1a0b, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
               {NdVar::cst(0, 4)});
        ++Cleanup.InstructionBoundaries.back().OpCount;
      } else if (Mutation == 6) {
        Cleanup.InstructionBoundaries.back().ControlFlags =
            LowInstructionControlFlag::Call;
      }
      for (unsigned I = 0; I != Cleanup.Ops.size(); ++I)
        Cleanup.Ops[I].Seq = I;
      F.Blocks.back() = std::move(Cleanup);
      if (Mutation >= 10) {
        LowBlock Scalar;
        Scalar.Id = F.Blocks.size();
        Scalar.StartAddr = Mutation >= 12 ? 0x19f8 : 0x19fb;
        Scalar.EndAddr = 0x1a00;
        Scalar.Succs = {F.Blocks.back().Id};
        emitOp(Scalar, Scalar.StartAddr, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
               {NdVar::cst(Mutation == 11 ? 0x4404 : 0x4400, 4)});
        Scalar.InstructionBoundaries = {{Scalar.StartAddr, 5, 0, 1}};
        if (Mutation >= 12) {
          // Scalar use retains alias taint. Neither dereferencing these bits
          // nor mixing in the physical frame receives jump-value authority.
          if (Mutation == 12)
            emitOp(Scalar, 0x19fd, NdOp::LOAD, NdVar::reg(x86reg::RAX, 4),
                   {NdVar::reg(x86reg::RAX, 4)});
          else
            emitOp(Scalar, 0x19fd, NdOp::INT_ADD, NdVar::reg(x86reg::RAX, 4),
                   {NdVar::reg(x86reg::RAX, 4), NdVar::reg(x86reg::RBP, 4)});
          Scalar.InstructionBoundaries.push_back({0x19fd, 3, 1, 1});
        }
        F.ExceptionMetadata->Registration->Scopes.back().HandlerVA =
            Scalar.StartAddr;
        F.Blocks.push_back(std::move(Scalar));
      }
      const auto Result =
          analyze(std::move(F), Variant::HandlerCleanup, 0x4000, true);
      if (Mutation >= 3 && Mutation != 7 && Mutation != 10) {
        EXPECT_FALSE(Result.Complete);
        EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
        continue;
      }
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
      ASSERT_TRUE(Result.Complete);
      ASSERT_TRUE(Result.SEHJumpResumptionsComplete);
      const auto Outer =
          llvm::find_if(Result.SEHJumpResumptions, [](const auto &Resume) {
            return Resume.Address == 0x115b;
          });
      ASSERT_NE(Outer, Result.SEHJumpResumptions.end());
      EXPECT_TRUE(Outer->SelectedScopes.empty());
      EXPECT_EQ(Outer->ResumedLevels, (std::vector<int32_t>{0}));
      EXPECT_EQ(Outer->StateRestores,
                (std::vector<RegistrationSEHJumpState>{{1, 0, 0}}));
    }
}
