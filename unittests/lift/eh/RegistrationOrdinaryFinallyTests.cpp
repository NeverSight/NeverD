//===- RegistrationOrdinaryFinallyTests.cpp - Physical finally calls -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Exercises actual stack and memory effects of ordinary finally calls.

#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/RegistrationState.h"
#include "neverd/ir/low/LowIR.h"
#include "neverd/lift/X86/X86Regs.h"

using namespace neverd;
using namespace neverd::registration_test;

TEST(RegistrationState, OrdinaryFinallyReturnsItsCheckedFrameEffects) {
  for (unsigned Mutation = 0; Mutation != 25; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeBranchingFrame();
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.Scopes = {{-1, 0, 0x1800, true}};
    F.Blocks.resize(7);
    auto &Setup = F.Blocks[4];
    Setup.Id = 4;
    Setup.StartAddr = 0x1080;
    Setup.EndAddr = 0x1088;
    emitOp(Setup, 0x1080, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(40, 4)});
    emitOp(Setup, 0x1083, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
    emitOp(Setup, 0x1086, NdOp::STORE, {},
           {NdVar::reg(x86reg::RCX, 4),
            Mutation == 3 ? NdVar::cst(0, 4) : NdVar::reg(x86reg::RSP, 4)});
    Setup.InstructionBoundaries = {
        {0x1080, 3, 0, 1}, {0x1083, 3, 1, 1}, {0x1086, 2, 2, 1}};
    if (Mutation == 15 || Mutation == 16) {
      emitOp(Setup, 0x1088, NdOp::COPY, NdVar::reg(x86reg::RBX, 4),
             {NdVar::reg(x86reg::RBP, 4)});
      Setup.InstructionBoundaries.push_back({0x1088, 2, 3, 1});
      Setup.EndAddr = 0x108a;
    }
    F.Blocks[0].Succs = {4};
    Setup.Succs = {1};
    F.Blocks[1].Succs = {5};
    auto &Call = F.Blocks[5];
    Call.Id = 5;
    Call.StartAddr = 0x1100;
    Call.EndAddr = 0x1105;
    Call.Succs = {2};
    if (Mutation == 4)
      emitOp(Call, 0x1100, NdOp::COPY, NdVar::reg(x86reg::RBP, 4),
             {NdVar::reg(x86reg::RAX, 4)});
    if (Mutation == 5 || Mutation == 24)
      emitOp(
          Call, 0x1100, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
          {NdVar::reg(x86reg::RSP, 4), NdVar::cst(Mutation == 5 ? 40 : 8, 4)});
    const auto Prefix = Call.Ops.size();
    emitOp(Call, 0x1100, NdOp::CALL, {}, {NdVar::cst(0x1800, 4)});
    Call.InstructionBoundaries = {{0x1100, 5, 0, Prefix + 1}};
    Call.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    Call.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Call;
    Call.InstructionBoundaries.back().Immediate = 0x1800;
    if (Mutation == 2)
      Call.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Conditional;
    if (Mutation == 6)
      Call.InstructionBoundaries.back().FirstOp = Prefix + 1;
    if (Mutation == 14)
      F.TruncatedPathAddresses.push_back(0x1800);

    auto &Finally = F.Blocks[6];
    Finally.Id = 6;
    Finally.StartAddr = 0x1800;
    va_t Address = Finally.StartAddr;
    const auto Emit = [&](NdOp Opcode, NdVar Output,
                          std::initializer_list<NdVar> Inputs) {
      const auto First = Finally.Ops.size();
      emitOp(Finally, Address, Opcode, Output, Inputs);
      Finally.InstructionBoundaries.push_back({Address++, 1, First, 1});
    };
    if (Mutation == 1) {
      Emit(NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(8, 4)});
      Emit(NdOp::STORE, {}, {NdVar::reg(x86reg::RSP, 4), NdVar::cst(7, 4)});
      Emit(NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(8, 4)});
    }
    Emit(NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4),
          NdVar::cst(uint32_t(Mutation == 7 ? -24 : -28), 4)});
    Emit(NdOp::STORE, {},
         {Mutation == 8   ? NdVar::reg(x86reg::RSP, 4)
          : Mutation == 9 ? NdVar::reg(x86reg::RDX, 4)
                          : NdVar::tmp(0, 4),
          NdVar::reg(Mutation == 16 ? x86reg::RBX : x86reg::RBP, 4)});
    // The effective-address temporary belongs to this same STORE instruction.
    Finally.Ops.back().Addr = Finally.Ops[Finally.Ops.size() - 2].Addr;
    Finally.InstructionBoundaries.pop_back();
    ++Finally.InstructionBoundaries.back().OpCount;
    --Address;
    if (Mutation == 10)
      Emit(NdOp::CALL, {}, {NdVar::cst(0x2200, 4)});
    if (Mutation == 11)
      Emit(NdOp::COPY, NdVar::reg(x86reg::RBP, 4), {NdVar::cst(0, 4)});
    if (Mutation == 15)
      Emit(NdOp::COPY, NdVar::reg(x86reg::RBX, 4), {NdVar::cst(0, 4)});
    Emit(NdOp::RETURN, {}, {});
    Finally.InstructionBoundaries.back().Control =
        LowInstructionControl::Return;
    Finally.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Return;
    if (Mutation == 12)
      Finally.InstructionBoundaries.back().Immediate = 4;
    if (Mutation == 13)
      Finally.InstructionBoundaries.back().ControlFlags =
          LowInstructionControlFlag::Conditional;
    Finally.EndAddr = Address;
    if (Mutation >= 17 && Mutation <= 21) {
      auto &Branch = Finally.Ops.back();
      Branch.Opcode = NdOp::COND_BR;
      Branch.addInput(NdVar::cst(Address + 16, 8));
      Branch.addInput(NdVar::reg(x86reg::ZF, 1));
      auto &Insn = Finally.InstructionBoundaries.back();
      Insn.Control = LowInstructionControl::Branch;
      Insn.ControlFlags = LowInstructionControlFlag::Branch |
                          LowInstructionControlFlag::Conditional;
      Insn.Immediate = Address + 16;
      Finally.Succs =
          Mutation == 19 ? std::vector<int>{8} : std::vector<int>{7, 8};
      for (unsigned Arm = 0; Arm != 2; ++Arm) {
        LowBlock Exit;
        Exit.Id = 7 + Arm;
        Exit.StartAddr = Address + Arm * 16;
        if (Arm && Mutation == 18) {
          emitOp(Exit, Exit.StartAddr, NdOp::STORE, {},
                 {NdVar::reg(x86reg::RSP, 4), NdVar::cst(0, 4)});
          Exit.InstructionBoundaries.push_back({Exit.StartAddr, 1, 0, 1});
        }
        if (Arm && Mutation == 20) {
          emitOp(Exit, Exit.StartAddr, NdOp::INT_ADD, NdVar::tmp(0, 4),
                 {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
          emitOp(Exit, Exit.StartAddr, NdOp::STORE, {},
                 {NdVar::tmp(0, 4), NdVar::cst(0, 4)});
          Exit.InstructionBoundaries.push_back({Exit.StartAddr, 1, 0, 2});
        }
        const auto Prefix = Exit.Ops.size();
        const va_t Return = Exit.StartAddr + (Prefix ? 1 : 0);
        emitOp(Exit, Return, NdOp::RETURN, {}, {});
        Exit.InstructionBoundaries.push_back({Return, 1, Prefix, 1});
        Exit.InstructionBoundaries.back().Control =
            LowInstructionControl::Return;
        Exit.InstructionBoundaries.back().ControlFlags =
            LowInstructionControlFlag::Return;
        if (Arm && Mutation == 21)
          Exit.InstructionBoundaries.back().Immediate = 4;
        Exit.EndAddr = Return + 1;
        F.Blocks.push_back(std::move(Exit));
      }
    }

    emitOp(F.Blocks[2], 0x1020, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
           {NdVar::reg(x86reg::RSP, 4)});
    const auto SPSeq = F.Blocks[2].Ops.back().Seq;
    emitOp(F.Blocks[2], 0x1020, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
    emitOp(F.Blocks[2], 0x1020, NdOp::LOAD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::tmp(0, 4)});
    const auto CellSeq = F.Blocks[2].Ops.back().Seq;
    emitOp(F.Blocks[2], 0x1020, NdOp::COPY, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RBX, 4)});
    const auto BXSeq = F.Blocks[2].Ops.back().Seq;
    const std::vector<RegistrationCalleeStackContract> Stacks{
        {0x1800, Mutation == 23 ? 4u : 0u, false}};
    const auto A = analyzeRegistrationStates(
        F, 0, 0, nullptr, nullptr, Mutation == 22 ? nullptr : &Stacks);
    bool RestoredSP = false, AppliedWrite = false, PreservedBX = false;
    for (const auto &Value : A.FrameValues) {
      if (Value.Address == 0x1020 && Value.OpSeq == SPSeq)
        RestoredSP |=
            Value.EstablishedFrameOffset == (Mutation == 24 ? -48 : -56);
      if (Value.Address == 0x1020 && Value.OpSeq == CellSeq)
        AppliedWrite |= Value.EstablishedFrameOffset == 0;
      if (Value.Address == 0x1020 && Value.OpSeq == BXSeq)
        PreservedBX |= Value.EstablishedFrameOffset == 0;
    }
    // A normal CALL/RET owns the actual ESP independently of SavedESP.
    // Releasing storage up to the registration administration still refuses.
    const bool Valid = Mutation < 2 || Mutation == 3 || Mutation == 15 ||
                       Mutation == 16 || Mutation == 17 || Mutation == 20 ||
                       Mutation == 24;
    EXPECT_EQ(RestoredSP, Valid);
    EXPECT_EQ(AppliedWrite, Valid && Mutation != 20);
    if (Mutation == 15 || Mutation == 16)
      EXPECT_EQ(PreservedBX, Mutation == 16);
    if (Valid)
      EXPECT_TRUE(A.Complete);
    // Inspection establishes state flow only. A native call still needs its
    // independent callable ABI and complete frame-effect contract.
    EXPECT_FALSE(A.CallFrameEffectsComplete);
    EXPECT_TRUE(A.CallFrameEffects.empty());
  }
}
