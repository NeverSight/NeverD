//===- RegistrationFilterJumpTests.cpp - Nonlocal search exits -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keeps filter jumps separate from returning search and finally effects.

#include "RegistrationFilterTestUtils.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/lift/X86/X86Regs.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <utility>

using namespace neverd;
using namespace neverd::registration_test;
using namespace neverd::registration_filter_test;

TEST(RegistrationFilterJump, SearchCanAbandonTheSuspendedFinally) {
  for (unsigned Protocol = 0; Protocol != 3; ++Protocol)
    for (unsigned Mixed = 0; Mixed != 3; ++Mixed)
      for (unsigned Mutation = 0; Mutation != 9; ++Mutation) {
        SCOPED_TRACE(Protocol);
        SCOPED_TRACE(Mixed);
        SCOPED_TRACE(Mutation);
        auto F = filterFrame(Variant::Correlated, Protocol);
        auto &Chain = *F.ExceptionMetadata->Registration;
        if (Mixed == 2)
          F.Blocks[15].Ops.front().Inputs[0] = NdVar::cst(1, 4);
        const unsigned StateIndex = Mixed ? 11 : 7;
        const va_t StateAddress = F.Blocks[StateIndex].StartAddr;
        std::erase_if(Chain.TryLevelStores, [&](const auto &Store) {
          return Store.StoreVA == StateAddress;
        });
        auto &State = F.Blocks[StateIndex];
        State = {};
        State.Id = StateIndex;
        State.StartAddr = StateAddress;
        State.EndAddr = StateAddress + 7;
        State.Succs = {int(F.Blocks.size())};
        addStateStore(State, StateAddress, 0, Chain);
        auto Jump = F.Blocks[4];
        Jump.Id = F.Blocks.size();
        const va_t Delta = 0x1a00 - Jump.StartAddr;
        Jump.StartAddr += Delta;
        Jump.EndAddr += Delta;
        Jump.Succs.clear();
        for (auto &Op : Jump.Ops)
          Op.Addr += Delta;
        for (auto &Boundary : Jump.InstructionBoundaries)
          Boundary.Address += Delta;
        Jump.Ops[4].Inputs[1] =
            Mutation == 2 ? NdVar::reg(x86reg::RBP, 4) : NdVar::cst(43, 4);
        if (Mutation == 1)
          Jump.Ops[1].Inputs[1] = NdVar::cst(uint32_t(-140), 4);
        if (Mutation == 3)
          Jump.InstructionBoundaries.back().ControlFlags =
              LowInstructionControlFlag::Call;
        if (Mutation == 4) {
          emitOp(Jump, 0x1a0b, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
                 {NdVar::cst(0, 4)});
          ++Jump.InstructionBoundaries.back().OpCount;
        }
        if (Mutation == 8)
          Jump.Ops.back().MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        F.Blocks.push_back(std::move(Jump));
        if (Mutation == 5 || Mutation == 6) {
          LowBlock Write;
          Write.Id = F.Blocks.size();
          Write.StartAddr = 0x19f9;
          Write.EndAddr = 0x1a00;
          Write.Succs = {int(F.Blocks.size()) - 1};
          emitOp(Write, Write.StartAddr, NdOp::INT_ADD, NdVar::tmp(0, 4),
                 {NdVar::reg(x86reg::RBP, 4),
                  NdVar::cst(uint32_t(Mutation == 5 ? -143 : -72), 4)});
          emitOp(Write, Write.StartAddr, NdOp::STORE, {},
                 {NdVar::tmp(0, 4), NdVar::cst(0, 1)});
          Write.InstructionBoundaries = {{Write.StartAddr, 7, 0, 2}};
          F.Blocks[StateIndex].Succs = {Write.Id};
          F.Blocks.push_back(std::move(Write));
        }
        for (auto &Block : F.Blocks)
          for (unsigned I = 0; I != Block.Ops.size(); ++I)
            Block.Ops[I].Seq = I;
        if (Mutation == 7)
          std::reverse(F.Blocks.begin(), F.Blocks.end());
        const auto Result = analyze(std::move(F), Variant::Correlated, 0x4000,
                                    /*UnwindJump=*/true);
        if (Mutation && Mutation != 6 && Mutation != 7) {
          EXPECT_FALSE(Result.Complete);
          EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
          continue;
        }
        for (const auto &Diagnostic : Result.Diagnostics)
          ADD_FAILURE() << Diagnostic;
        ASSERT_TRUE(Result.Complete);
        ASSERT_TRUE(Result.SEHJumpResumptionsComplete);
        ASSERT_EQ(Result.SEHJumpResumptions.size(), 2u);
        const auto Filter =
            llvm::find_if(Result.SEHJumpResumptions, [](const auto &Resume) {
              return Resume.Address == 0x1a0b;
            });
        ASSERT_NE(Filter, Result.SEHJumpResumptions.end());
        EXPECT_EQ(Filter->EntryVA, 0x1800u);
        EXPECT_EQ(Filter->CaptureCall, 0x1100u);
        EXPECT_EQ(Filter->ResumedLevels, (std::vector<int32_t>{0}));
        EXPECT_TRUE(Filter->SelectedScopes.empty());
        EXPECT_TRUE(Filter->Replacements.empty());
      }
}
