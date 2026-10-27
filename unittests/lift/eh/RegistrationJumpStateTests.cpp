//===- RegistrationJumpStateTests.cpp - EH3 buffer lifetime and resumption ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Checks EH3 captures, callback effects and nonlocal state restoration.

#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"
#include "gtest/gtest.h"

#include "neverd/ir/low/LowScalarConstant.h"
#include "neverd/lift/X86Regs.h"

using namespace neverd;
using namespace neverd::registration_state;
using namespace neverd::registration_test;

TEST(RegistrationJumpState, CaptureSurvivesOnlyCheckedStorageAndCalls) {
  for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
    SCOPED_TRACE(Mutation);
    const auto F = makeJumpFrame(Mutation);
    SEHJumpRuntime Runtime;
    Runtime.ImportSlots.insert(0x4100);
    Runtime.Capture = [&](const LowOp &Op, const FrameState &Frame,
                          bool Installed,
                          size_t &) -> std::optional<SEHJumpCapture> {
      if (Op.Addr != 0x1100 || !Installed || Mutation == 4)
        return std::nullopt;
      EXPECT_EQ(Frame.Registers[x86reg::RSP / x86reg::GeneralRegStride].Offset,
                -192);
      SEHJumpCapture Capture;
      Capture.Call = Op.Addr;
      Capture.ReturnPC = Mutation == 5 ? 0x1106 : 0x1105;
      Capture.BufferOffset = -144;
      Capture.StackOffset = -192;
      Capture.UnwindTarget = 0x3300;
      constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                        x86reg::RSI};
      for (unsigned I = 0; I != 4; ++I)
        Capture.Nonvolatile[I] =
            Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
      return Capture;
    };
    Runtime.Resume = [](const LowOp &Op, size_t &) {
      return Op.Addr == 0x111a;
    };
    RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, &Runtime);
    const auto Result = Solver.run();
    if (Mutation == 0) {
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
      EXPECT_TRUE(Result.Complete);
      EXPECT_TRUE(Result.RegistrationLifetimeComplete);
      EXPECT_TRUE(Result.HasSEHJumpCaptures);
      EXPECT_TRUE(Result.SEHJumpCapturesComplete);
      ASSERT_EQ(Result.SEHJumpCaptures.size(), 1u);
      const auto &Capture = Result.SEHJumpCaptures.front();
      EXPECT_EQ(Capture.Address, 0x1100u);
      EXPECT_EQ(Capture.EndAddress, 0x1105u);
      EXPECT_EQ(Capture.TargetVA, 0x3000u);
      EXPECT_EQ(Capture.UnwindVA, 0x3300u);
      EXPECT_EQ(Capture.BufferOffset, -144);
      EXPECT_EQ(Capture.StackOffset, -192);
      EXPECT_EQ(Capture.Levels, (std::vector<int32_t>{-1}));
      EXPECT_TRUE(Result.SEHJumpResumptionsComplete);
      ASSERT_EQ(Result.SEHJumpResumptions.size(), 1u);
      const auto &Resume = Result.SEHJumpResumptions.front();
      EXPECT_EQ(Resume.Address, 0x111au);
      EXPECT_EQ(Resume.CaptureCall, 0x1100u);
      EXPECT_EQ(Resume.TargetVA, 0x1105u);
      EXPECT_EQ(Resume.StackOffset, -192);
      // Capture reserves 16 bytes; its successor releases those arguments,
      // then longjmp reserves eight bytes and writes its value at ESP + 4.
      EXPECT_EQ(Resume.ValueOffset, -192 + 16 - 8 + 4);
    } else {
      EXPECT_FALSE(Result.SEHJumpCapturesComplete);
      EXPECT_TRUE(Result.SEHJumpCaptures.empty());
      EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
      EXPECT_TRUE(Result.SEHJumpResumptions.empty());
    }
  }
}

TEST(RegistrationJumpState, CaptureWritesOnlyTheSpecifiedRuntimeFields) {
  SEHJumpCapture Capture;
  Capture.BufferOffset = -144;
  Capture.StackOffset = -192;
  Capture.ReturnPC = 0x1105;
  Capture.UnwindTarget = 0x3300;
  Capture.Nonvolatile[0] = FrameValue::frame(0);
  for (unsigned I = 1; I != Capture.Nonvolatile.size(); ++I)
    Capture.Nonvolatile[I] = FrameValue::constant(0x11223344 + I);
  for (unsigned Mutation = 0; Mutation != 42; ++Mutation) {
    FrameState Frame;
    Frame.store(-104, 4, FrameValue::constant(7));
    storeSEHJumpCapture(Frame, Capture);
    EXPECT_EQ(Frame.load(-104, 4).Constant, 7u);
    EXPECT_TRUE(hasSEHJumpCapture(Frame, Capture));
    if (Mutation < 40)
      Frame.store(-144 + Mutation, 1, FrameValue::constant(0));
    else if (Mutation == 40)
      Frame.forgetCellValues();
    else
      Frame.merge(FrameState{});
    EXPECT_FALSE(hasSEHJumpCapture(Frame, Capture));
  }
}

TEST(RegistrationJumpState, EH4InitializesAndProtectsItsCookieAddress) {
  SEHJumpCapture Capture;
  Capture.Protocol = RegistrationSEHJumpCapture::Kind::EH4;
  Capture.SecurityCookieVA = 0x405000;
  Capture.Nonvolatile[0] = FrameValue::frame(0);
  for (unsigned I = 1; I != Capture.Nonvolatile.size(); ++I)
    Capture.Nonvolatile[I] = FrameValue::constant(0x11223344 + I);
  Capture.BufferOffset = -144;
  Capture.StackOffset = -192;
  Capture.Level = -2;
  Capture.ReturnPC = 0x1105;
  Capture.UnwindTarget = 0x3300;
  for (unsigned I = 0; I != 44; ++I) {
    SCOPED_TRACE(I);
    FrameState Frame;
    Frame.store(-100, 4, FrameValue::constant(7));
    storeSEHJumpCapture(Frame, Capture);
    EXPECT_EQ(Frame.load(-104, 4).ImageAddress, 0x405000u);
    EXPECT_FALSE(Frame.load(-104, 4).Constant);
    EXPECT_EQ(Frame.load(-100, 4).Constant, 7u);
    EXPECT_TRUE(hasSEHJumpCapture(Frame, Capture));
    Frame.store(-144 + I, 1, {});
    EXPECT_FALSE(hasSEHJumpCapture(Frame, Capture));
  }
}

TEST(RegistrationJumpState, RuntimeCallbacksResumeOnlyFromPrivateArguments) {
  for (bool Filter : {false, true}) {
    SCOPED_TRACE(Filter);
    for (unsigned Mutation = 0; Mutation != 4; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto F = makeJumpFrame(0);
      F.Blocks.resize(8);
      auto &Chain = *F.ExceptionMetadata->Registration;
      Chain.Scopes =
          Filter ? std::vector<RegistrationScopeRecord>{{-1, 0x110f, 0x1120,
                                                         false}}
                 : std::vector<RegistrationScopeRecord>{{-1, 0, 0x110f, true}};
      if (Mutation == 3)
        Chain.SeededTryLevel = 0;
      Chain.TryLevelStores = {{0x1120, 0x1127, -1}, {0x1140, 0x1147, 0}};
      auto &Resume = F.Blocks[3];
      Resume.Ops.clear();
      Resume.Succs = {7};
      Resume.EndAddr = 0x1108;
      emitOp(Resume, 0x1105, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
      Resume.InstructionBoundaries = {{0x1105, 3, 0, 1}};
      auto &Active = F.Blocks[7];
      Active.Id = 7;
      Active.StartAddr = 0x1140;
      Active.EndAddr = 0x1147;
      Active.Succs = {5};
      addSlotStore(Active, 0);
      for (unsigned I = 0; I != Active.Ops.size(); ++I)
        Active.Ops[I].Seq = I;
      Active.InstructionBoundaries = {{0x1140, 7, 0, 2}};
      if (Mutation == 1 || Mutation == 2) {
        auto &Jump = F.Blocks[4];
        LowOp Access;
        Access.Addr = 0x110f;
        Access.Opcode = Mutation == 1 ? NdOp::STORE : NdOp::LOAD;
        Access.addInput(NdVar::reg(x86reg::RSP, 4));
        if (Mutation == 1)
          Access.addInput(NdVar::cst(0, 1));
        else
          Access.Output = NdVar::reg(x86reg::RCX, 1);
        Jump.Ops.insert(Jump.Ops.begin(), Access);
        ++Jump.InstructionBoundaries.front().OpCount;
        for (unsigned I = 1; I != Jump.InstructionBoundaries.size(); ++I)
          ++Jump.InstructionBoundaries[I].FirstOp;
        for (unsigned I = 0; I != Jump.Ops.size(); ++I)
          Jump.Ops[I].Seq = I;
      }
      SEHJumpRuntime Runtime;
      Runtime.Capture = [](const LowOp &Op, const FrameState &Frame, bool,
                           size_t &) -> std::optional<SEHJumpCapture> {
        if (Op.Addr != 0x1100)
          return std::nullopt;
        SEHJumpCapture Capture;
        Capture.Call = Op.Addr;
        Capture.ReturnPC = 0x1105;
        Capture.BufferOffset = -144;
        Capture.StackOffset = -192;
        Capture.UnwindTarget = 0x3300;
        constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                          x86reg::RSI};
        for (unsigned I = 0; I != 4; ++I)
          Capture.Nonvolatile[I] =
              Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
        return Capture;
      };
      Runtime.Resume = [](const LowOp &Op, size_t &) {
        return Op.Addr == 0x111a;
      };
      std::vector<RegistrationSEHControlContract> Controls;
      RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr,
                                     nullptr, &Controls, nullptr, nullptr,
                                     &Runtime);
      const auto Result = Solver.run();
      if (!Mutation) {
        for (const auto &Diagnostic : Result.Diagnostics)
          ADD_FAILURE() << Diagnostic;
        EXPECT_TRUE(Result.Complete);
        ASSERT_EQ(Result.SEHJumpResumptions.size(), 1u);
        EXPECT_EQ(Result.SEHJumpResumptions.front().EntryVA, 0x110fu);
        EXPECT_EQ(Result.SEHJumpResumptions.front().TargetVA, 0x1105u);
        EXPECT_EQ(Result.SEHJumpResumptions.front().ValueOffset, -4);
        EXPECT_EQ(Result.SEHJumpResumptions.front().ResumedLevels,
                  std::vector<int32_t>{-1});
      } else {
        EXPECT_FALSE(Result.Complete);
        EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
        EXPECT_TRUE(Result.SEHJumpResumptions.empty());
      }
    }
  }
}

TEST(RegistrationJumpState, ExactFlagsSurviveOnlyTheirReachingDefinition) {
  FrameState State;
  State.OtherRegistersMayBeFrame = true;
  State.Registers[x86reg::RAX / x86reg::GeneralRegStride] =
      FrameValue::constant(1);
  FrameTransfer Transfer(State, -16);
  LowOp Equal;
  Equal.Opcode = NdOp::INT_EQUAL;
  Equal.Output = NdVar::reg(x86reg::ZF, 1);
  Equal.addInput(NdVar::reg(x86reg::RAX, 4));
  Equal.addInput(NdVar::cst(2, 4));
  Transfer.write(Equal, Transfer.evaluate(Equal, true));
  EXPECT_EQ(Transfer.read(Equal.Output).Constant, 0u);
  Equal.Inputs[1] = NdVar::cst(1, 4);
  Transfer.write(Equal, Transfer.evaluate(Equal, true));
  EXPECT_EQ(Transfer.read(Equal.Output).Constant, 1u);
  auto Other = State;
  Other.OtherRegisterBytes[x86reg::ZF] = FrameValue::constant(0);
  State.merge(Other);
  EXPECT_FALSE(Transfer.read(Equal.Output).Constant);
  Transfer.write(Equal, Transfer.evaluate(Equal, true));
  LowOp Call;
  Call.Opcode = NdOp::CALL;
  Transfer.write(Call, {});
  EXPECT_FALSE(Transfer.read(Equal.Output).Constant);
  EXPECT_TRUE(Transfer.read(Equal.Output).MayBeFrame);
}

TEST(RegistrationJumpState, AsyncFilterWritesRevokeAParentResumption) {
  for (bool Overlap : {false, true}) {
    SCOPED_TRACE(Overlap);
    auto F = makeJumpFrame(0);
    F.Blocks.resize(9);
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.TryLevelStores = {{0x110f, 0x1116, 0}, {0x1120, 0x1127, -1}};
    auto &Resume = F.Blocks[3];
    Resume.Succs = {8, 5};
    auto &Active = F.Blocks[8];
    Active.Id = 8;
    Active.StartAddr = 0x110f;
    Active.EndAddr = 0x1116;
    Active.Succs = {4};
    addSlotStore(Active, 0);
    for (unsigned I = 0; I != Active.Ops.size(); ++I)
      Active.Ops[I].Seq = I;
    Active.InstructionBoundaries = {{0x110f, 7, 0, 2}};
    auto &Jump = F.Blocks[4];
    Jump.StartAddr += 0x41;
    Jump.EndAddr += 0x41;
    for (auto &Op : Jump.Ops)
      Op.Addr += 0x41;
    for (auto &Boundary : Jump.InstructionBoundaries)
      Boundary.Address += 0x41;
    auto &Filter = F.Blocks[7];
    Filter.Id = 7;
    Filter.StartAddr = 0x1800;
    Filter.EndAddr = 0x1806;
    emitOp(Filter, 0x1800, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4),
            NdVar::cst(uint32_t(Overlap ? -144 : -28), 4)});
    emitOp(Filter, 0x1800, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::cst(1, 4)});
    emitOp(Filter, 0x1805, NdOp::RETURN, {}, {});
    Filter.InstructionBoundaries = {{0x1800, 5, 0, 2}, {0x1805, 1, 2, 1}};
    Filter.InstructionBoundaries.back().Control = LowInstructionControl::Return;
    Filter.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Return;
    SEHJumpRuntime Runtime;
    Runtime.Capture = [](const LowOp &Op, const FrameState &Frame, bool,
                         size_t &) -> std::optional<SEHJumpCapture> {
      if (Op.Addr != 0x1100)
        return std::nullopt;
      SEHJumpCapture Capture;
      Capture.Call = Op.Addr;
      Capture.ReturnPC = 0x1105;
      Capture.BufferOffset = -144;
      Capture.StackOffset = -192;
      Capture.UnwindTarget = 0x3300;
      constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                        x86reg::RSI};
      for (unsigned I = 0; I != 4; ++I)
        Capture.Nonvolatile[I] =
            Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
      return Capture;
    };
    Runtime.Resume = [](const LowOp &Op, size_t &) {
      return Op.Addr == 0x115b;
    };
    RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, &Runtime);
    const auto Result = Solver.run();
    EXPECT_EQ(Result.Complete, !Overlap);
    if (!Overlap && !Result.Complete)
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
    EXPECT_EQ(Result.SEHJumpResumptionsComplete, !Overlap);
    EXPECT_EQ(Result.SEHJumpResumptions.size(), Overlap ? 0u : 1u);
  }
}

TEST(RegistrationJumpState, FinallyRaiseUsesClosedFilterFrameEffects) {
  for (unsigned Mutation = 0; Mutation != 15; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeJumpFrame(0);
    F.Blocks.resize(10);
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.Scopes = {{-1, 0x1800, 0x1120, false}, {0, 0, 0x1840, true}};
    Chain.TryLevelStores = {{0x1140, 0x1147, 1}, {0x1120, 0x1127, -1}};
    F.Blocks[3].Succs = {8, 5};
    auto &Active = F.Blocks[8];
    Active.Id = 8;
    Active.StartAddr = 0x1140;
    Active.EndAddr = 0x1147;
    Active.Succs = {4};
    addSlotStore(Active, 1);
    for (unsigned I = 0; I != Active.Ops.size(); ++I)
      Active.Ops[I].Seq = I;
    Active.InstructionBoundaries = {{0x1140, 7, 0, 2}};
    auto &Jump = F.Blocks[4];
    Jump.StartAddr += 0x41;
    Jump.EndAddr += 0x41;
    for (auto &Op : Jump.Ops)
      Op.Addr += 0x41;
    for (auto &Boundary : Jump.InstructionBoundaries)
      Boundary.Address += 0x41;
    auto &Filter = F.Blocks[7];
    Filter.Id = 7;
    Filter.StartAddr = 0x1800;
    Filter.EndAddr = 0x180b;
    const int32_t Offset = Mutation == 1   ? -144
                           : Mutation == 2 ? -4
                           : Mutation == 3 ? -24
                                           : -28;
    emitOp(Filter, 0x1800, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Offset), 4)});
    emitOp(Filter, 0x1800, NdOp::STORE, {},
           {(Mutation >= 10 && Mutation <= 12) ? NdVar::cst(0x4400, 4)
                                               : NdVar::tmp(0, 4),
            Mutation == 12 ? NdVar::reg(x86reg::RBP, 4) : NdVar::cst(2, 4)});
    emitOp(Filter, 0x1805, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
           {Mutation == 7 ? NdVar::reg(x86reg::RCX, 4)
                          : NdVar::cst(Mutation == 5   ? 0
                                       : Mutation == 6 ? 1
                                                       : UINT32_MAX,
                                       4)});
    Filter.InstructionBoundaries = {{0x1800, 5, 0, 2}, {0x1805, 5, 2, 1}};
    if (Mutation == 4 || Mutation == 8) {
      emitOp(Filter, 0x1805, Mutation == 4 ? NdOp::INT_SUB : NdOp::CALL,
             Mutation == 4 ? NdVar::reg(x86reg::RSP, 4) : NdVar{},
             Mutation == 4
                 ? std::initializer_list<NdVar>{NdVar::reg(x86reg::RSP, 4),
                                                NdVar::cst(4, 4)}
                 : std::initializer_list<NdVar>{NdVar::cst(0x3500, 4)});
      ++Filter.InstructionBoundaries.back().OpCount;
    }
    const auto Return = [&](LowBlock &Block, va_t Address) {
      const auto First = Block.Ops.size();
      emitOp(Block, Address, NdOp::RETURN, {}, {});
      Block.InstructionBoundaries.push_back({Address, 1, First, 1});
      auto &Boundary = Block.InstructionBoundaries.back();
      Boundary.Control = LowInstructionControl::Return;
      Boundary.ControlFlags = LowInstructionControlFlag::Return;
    };
    Return(Filter, 0x180a);
    auto &Finally = F.Blocks[9];
    Finally.Id = 9;
    Finally.StartAddr = 0x1840;
    Finally.EndAddr = 0x1857;
    emitOp(Finally, 0x1840, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
    for (unsigned I = 0; I != 4; ++I) {
      emitOp(Finally, 0x1840, NdOp::INT_ADD, NdVar::tmp(0, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(I * 4, 4)});
      emitOp(
          Finally, 0x1840, NdOp::STORE, {},
          {NdVar::tmp(0, 4), NdVar::cst(Mutation == 9 && I == 1 ? 1 : 0, 4)});
    }
    Finally.InstructionBoundaries = {{0x1840, 16, 0, Finally.Ops.size()}};
    const auto Call = Finally.Ops.size();
    emitOp(Finally, 0x1850, NdOp::INDIR_CALL, {}, {NdVar::cst(0x4100, 4)});
    Finally.InstructionBoundaries.push_back({0x1850, 6, Call, 1});
    Finally.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    Finally.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Call;
    Return(Finally, 0x1856);
    if (Mutation >= 13) {
      // A continuing filter can change the frame state. Search still follows
      // its captured scope cursor, whereas the suspended unwind reloads the
      // changed frame state after the finally returns.
      F.Blocks.resize(12);
      auto &StateStore = F.Blocks[7];
      StateStore.Ops[0].Inputs[1] = NdVar::cst(uint32_t(-4), 4);
      StateStore.Ops[1].Inputs[1] = NdVar::cst(UINT32_MAX, 4);
      auto &Disposition = F.Blocks[10];
      Disposition.Id = 10;
      Disposition.StartAddr = 0x1805;
      Disposition.EndAddr = 0x180b;
      Disposition.Ops.assign(StateStore.Ops.begin() + 2, StateStore.Ops.end());
      Disposition.InstructionBoundaries.assign(
          StateStore.InstructionBoundaries.begin() + 1,
          StateStore.InstructionBoundaries.end());
      for (unsigned I = 0; I != Disposition.Ops.size(); ++I)
        Disposition.Ops[I].Seq = I;
      for (auto &Boundary : Disposition.InstructionBoundaries)
        Boundary.FirstOp -= 2;
      StateStore.Ops.resize(2);
      StateStore.InstructionBoundaries.resize(1);
      StateStore.EndAddr = 0x1805;
      StateStore.Succs = {10};
      Chain.TryLevelStores.push_back({0x1800, 0x1805, -1});
      auto &Continue = F.Blocks[11];
      Continue.Id = 11;
      Continue.StartAddr = 0x1880;
      Continue.EndAddr = 0x1886;
      emitOp(Continue, 0x1880, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
             {NdVar::cst(UINT32_MAX, 4)});
      Continue.InstructionBoundaries = {{0x1880, 5, 0, 1}};
      Return(Continue, 0x1885);
      if (Mutation == 14) {
        Chain.Scopes = {{-1, 0x1880, 0x1120, false},
                        {0, 0x1800, 0x1120, false},
                        {1, 0, 0x1840, true}};
        Chain.TryLevelStores.front().Level = 2;
        F.Blocks[8].Ops.back().Inputs[1] = NdVar::cst(2, 4);
        Disposition.Ops.front().Inputs[0] = NdVar::cst(0, 4);
      }
    }
    SEHJumpRuntime Runtime;
    Runtime.ImportSlots.insert(0x4100);
    Runtime.Capture = [](const LowOp &Op, const FrameState &Frame, bool,
                         size_t &) -> std::optional<SEHJumpCapture> {
      if (Op.Addr != 0x1100)
        return std::nullopt;
      SEHJumpCapture Capture;
      Capture.Call = Op.Addr;
      Capture.ReturnPC = 0x1105;
      Capture.BufferOffset = -144;
      Capture.StackOffset = -192;
      Capture.UnwindTarget = 0x3300;
      constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                        x86reg::RSI};
      for (unsigned I = 0; I != 4; ++I)
        Capture.Nonvolatile[I] =
            Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
      return Capture;
    };
    Runtime.Resume = [](const LowOp &Op, size_t &) {
      return Op.Addr == 0x115b;
    };
    Runtime.Raise = [](const LowOp &Op, const FrameState &, size_t &) {
      return Op.Addr == 0x1850;
    };
    Runtime.ImageMemory = [&](va_t Address, uint16_t Width, bool Write,
                              size_t &) {
      return Mutation != 11 && Address == 0x4400 && Width == 4 && Write;
    };
    RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, &Runtime);
    const auto Result = Solver.run();
    const bool Accepted =
        Mutation == 0 || Mutation == 6 || Mutation == 10 || Mutation >= 13;
    if (Accepted && !Result.Complete)
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
    EXPECT_EQ(Result.Complete, Accepted);
    EXPECT_EQ(Result.SEHJumpResumptionsComplete, Accepted);
    EXPECT_EQ(Result.SEHJumpResumptions.size(), Accepted ? 1u : 0u);
    if (Mutation == 6 && !Result.SEHJumpResumptions.empty()) {
      const auto &Resume = Result.SEHJumpResumptions.front();
      EXPECT_EQ(Resume.SelectedScopes, (std::vector<uint32_t>{0}));
      EXPECT_TRUE(Resume.ResumedLevels.empty());
      EXPECT_TRUE(Resume.StateRestores.empty());
    }
  }
}

TEST(RegistrationJumpState, UnwindFinallyKeepsNestedPrivateStacksAndEffects) {
  for (unsigned Mutation = 0; Mutation != 21; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeJumpFrame(0);
    F.Blocks.resize(13);
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.Scopes = {{-1, 0x1800, 0x1120, false},
                    {0, 0, 0x1840, true},
                    {1, 0, 0x1880, true}};
    Chain.TryLevelStores = {{0x1140, 0x1147, 1}, {0x1120, 0x1127, -1}};
    F.Blocks[3].Succs = {8, 5};
    auto &Active = F.Blocks[8];
    Active.Id = 8;
    Active.StartAddr = 0x1140;
    Active.EndAddr = 0x1147;
    Active.Succs = {4};
    addSlotStore(Active, 1);
    for (unsigned I = 0; I != Active.Ops.size(); ++I)
      Active.Ops[I].Seq = I;
    Active.InstructionBoundaries = {{0x1140, 7, 0, 2}};
    auto &Jump = F.Blocks[4];
    Jump.StartAddr += 0x38;
    Jump.EndAddr += 0x38;
    for (auto &Op : Jump.Ops)
      Op.Addr += 0x38;
    for (auto &Boundary : Jump.InstructionBoundaries)
      Boundary.Address += 0x38;
    auto SetReturn = [&](LowBlock &Block, va_t Address) {
      const auto First = Block.Ops.size();
      emitOp(Block, Address, NdOp::RETURN, {}, {});
      Block.EndAddr = Address + 1;
      Block.InstructionBoundaries.push_back({Address, 1, First, 1});
      auto &Boundary = Block.InstructionBoundaries.back();
      Boundary.Control = LowInstructionControl::Return;
      Boundary.ControlFlags = LowInstructionControlFlag::Return;
    };
    auto &Filter = F.Blocks[7];
    Filter.Id = 7;
    Filter.StartAddr = 0x1800;
    emitOp(Filter, 0x1800, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
           {NdVar::cst(UINT32_MAX, 4)});
    Filter.InstructionBoundaries = {{0x1800, 5, 0, 1}};
    SetReturn(Filter, 0x1805);
    auto &Outer = F.Blocks[9];
    Outer.Id = 9;
    Outer.StartAddr = 0x1840;
    Outer.EndAddr = 0x1847;
    Outer.Succs = {Mutation == 9 ? 5 : 10};
    emitOp(Outer, 0x1840, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Outer, 0x1840, NdOp::STORE, {},
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(7, 4)});
    emitOp(Outer, 0x1842, NdOp::CALL, {}, {NdVar::cst(0x1880, 4)});
    if (Mutation == 10)
      Outer.Ops.back().MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    Outer.InstructionBoundaries = {{0x1840, 2, 0, 2}, {0x1842, 5, 2, 1}};
    Outer.InstructionBoundaries.back().Control = LowInstructionControl::Call;
    Outer.InstructionBoundaries.back().ControlFlags =
        LowInstructionControlFlag::Call;
    Outer.InstructionBoundaries.back().Immediate = 0x1880;
    auto &Resume = F.Blocks[10];
    Resume.Id = 10;
    Resume.StartAddr = 0x1847;
    emitOp(Resume, 0x1847, NdOp::LOAD, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RSP, 4)});
    emitOp(Resume, 0x1847, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    // The parent write is provable only if the outer spill survives the inner
    // private stack and the inner's actual EBX frame value is retained.
    emitOp(Resume, 0x1848, NdOp::INT_ADD, NdVar::reg(x86reg::RCX, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-35), 4)});
    emitOp(Resume, 0x184b, NdOp::INT_ADD, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RCX, 4), NdVar::reg(x86reg::RDX, 4)});
    emitOp(Resume, 0x184d, NdOp::STORE, {},
           {NdVar::reg(x86reg::RDX, 4), NdVar::reg(x86reg::RBX, 4)});
    Resume.InstructionBoundaries = {{0x1847, 1, 0, 2},
                                    {0x1848, 3, 2, 1},
                                    {0x184b, 2, 3, 1},
                                    {0x184d, 2, 4, 1}};
    SetReturn(Resume, 0x184f);
    auto &Inner = F.Blocks[11];
    Inner.Id = 11;
    Inner.StartAddr = 0x1880;
    if (Mutation == 4)
      emitOp(Inner, 0x1880, NdOp::STORE, {},
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(9, 4)});
    emitOp(Inner, 0x1880, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Inner, 0x1880, NdOp::STORE, {},
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(9, 4)});
    const auto Prefix = Inner.Ops.size();
    emitOp(Inner, 0x1882, NdOp::COPY, NdVar::reg(x86reg::RBX, 4),
           {NdVar::reg(x86reg::RBP, 4)});
    emitOp(
        Inner, 0x1884, NdOp::INT_ADD, NdVar::reg(x86reg::RDX, 4),
        {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Mutation == 5   ? -24
                                                         : Mutation == 6 ? -144
                                                                         : -32),
                                                4)});
    emitOp(Inner, 0x1887, NdOp::STORE, {},
           {NdVar::reg(x86reg::RDX, 4), NdVar::reg(x86reg::RBX, 4)});
    emitOp(Inner, 0x1889, NdOp::LOAD, NdVar::reg(x86reg::RDX, 4),
           {NdVar::reg(x86reg::RSP, 4)});
    if (Mutation != 7)
      emitOp(Inner, 0x1889, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    Inner.InstructionBoundaries = {
        {0x1880, 2, 0, Prefix},
        {0x1882, 2, Prefix, 1},
        {0x1884, 3, Prefix + 1, 1},
        {0x1887, 2, Prefix + 2, 1},
        {0x1889, 1, Prefix + 3, Mutation == 7 ? 1u : 2u}};
    SetReturn(Inner, 0x188a);
    if (Mutation == 3)
      Inner.InstructionBoundaries.back().Immediate = 4;
    auto &RecursiveReturn = F.Blocks[12];
    RecursiveReturn.Id = 12;
    RecursiveReturn.StartAddr = 0x188f;
    SetReturn(RecursiveReturn, 0x188f);
    std::vector<RegistrationSEHControlContract> Controls{
        {RegistrationSEHControlContract::Kind::NearCall, 0x1842, 0x1847,
         Mutation == 1 ? 0x1881u : 0x1880u},
        {RegistrationSEHControlContract::Kind::NearReturn, 0x1805, 0x1806,
         InvalidVA},
        {RegistrationSEHControlContract::Kind::NearReturn, 0x184f, 0x1850,
         InvalidVA},
        {RegistrationSEHControlContract::Kind::NearReturn, 0x188a, 0x188b,
         InvalidVA},
        {RegistrationSEHControlContract::Kind::NearReturn, 0x188f, 0x1890,
         InvalidVA}};
    if (Mutation == 2)
      Controls.erase(Controls.begin());
    if (Mutation == 8) {
      Inner.Ops.back().Opcode = NdOp::CALL;
      Inner.Ops.back().addInput(NdVar::cst(0x1880, 4));
      auto &Boundary = Inner.InstructionBoundaries.back();
      Boundary.Size = 5;
      Boundary.Control = LowInstructionControl::Call;
      Boundary.ControlFlags = LowInstructionControlFlag::Call;
      Boundary.Immediate = 0x1880;
      Inner.EndAddr = 0x188f;
      Inner.Succs = {12};
      Controls[3] = {RegistrationSEHControlContract::Kind::NearCall, 0x188a,
                     0x188f, 0x1880};
    }
    if (Mutation >= 11 && Mutation < 20)
      addFinallyTailJump(Outer, Resume, Controls, Mutation - 11);
    if (Mutation == 20)
      addFinallyCallTrailingEffect(Outer);
    SEHJumpRuntime Runtime;
    Runtime.Capture = [](const LowOp &Op, const FrameState &Frame, bool,
                         size_t &) -> std::optional<SEHJumpCapture> {
      if (Op.Addr != 0x1100)
        return std::nullopt;
      SEHJumpCapture Capture;
      Capture.Call = Op.Addr;
      Capture.ReturnPC = 0x1105;
      Capture.BufferOffset = -144;
      Capture.StackOffset = -192;
      Capture.UnwindTarget = 0x3300;
      constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                        x86reg::RSI};
      for (unsigned I = 0; I != 4; ++I)
        Capture.Nonvolatile[I] =
            Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
      return Capture;
    };
    Runtime.Resume = [](const LowOp &Op, size_t &) {
      return Op.Addr == 0x1152;
    };
    RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                   &Controls, nullptr, nullptr, &Runtime);
    const auto Result = Solver.run();
    const bool Valid = Mutation == 0 || Mutation == 11;
    if (Valid && !Result.Complete)
      for (const auto &Diagnostic : Result.Diagnostics)
        ADD_FAILURE() << Diagnostic;
    EXPECT_EQ(Result.Complete, Valid);
    EXPECT_EQ(Result.SEHJumpResumptionsComplete, Valid);
    EXPECT_EQ(Result.SEHJumpResumptions.size(), Valid ? 1u : 0u);
    if (Valid) {
      ASSERT_EQ(Result.SEHInvocationExits.size(), 1u);
      EXPECT_EQ(Result.SEHInvocationExits.front().CallVA, 0x1842u);
      EXPECT_EQ(Result.SEHInvocationExits.front().StackOffset, -4);
    }
  }
}

TEST(RegistrationCRTCall, ControlFPWritesOnlyADisjointScalarWord) {
  for (unsigned Mutation = 0; Mutation != 6; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto F = makeJumpFrame(0);
    auto &Setup = F.Blocks[1];
    emitOp(
        Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(0, 4),
        {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Mutation == 1   ? -24
                                                         : Mutation == 2 ? -192
                                                                         : -48),
                                                4)});
    emitOp(Setup, 0x1086, NdOp::STORE, {},
           {NdVar::reg(x86reg::RSP, 4), NdVar::tmp(0, 4)});
    emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    emitOp(Setup, 0x1086, NdOp::STORE, {},
           {NdVar::tmp(0, 4),
            Mutation == 3 ? NdVar::reg(x86reg::RBP, 4) : NdVar::cst(0, 4)});
    emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(8, 4)});
    emitOp(
        Setup, 0x1086, NdOp::STORE, {},
        {NdVar::tmp(0, 4), NdVar::cst(Mutation == 4 ? 0x30000 : 0x8001f, 4)});
    Setup.InstructionBoundaries.back().OpCount = 7;
    auto &Call = F.Blocks[2];
    Call.Ops[0].Opcode = NdOp::INDIR_CALL;
    Call.Ops[0].Inputs[0] = NdVar::cst(0x4200, 4);
    Call.InstructionBoundaries[0].Immediate = 0x4200;
    Call.InstructionBoundaries[0].ControlFlags |=
        LowInstructionControlFlag::Indirect;
    auto &Resume = F.Blocks[3];
    Resume.Ops.erase(Resume.Ops.begin());
    Resume.Ops.pop_back();
    Resume.Succs = {5};
    Resume.EndAddr = 0x110b;
    Resume.InstructionBoundaries = {{0x1108, 3, 0, 1}};
    SEHJumpRuntime Runtime;
    Runtime.ControlFP = [Mutation](const LowOp &Op, size_t &) {
      return Mutation != 5 && Op.Opcode == NdOp::INDIR_CALL &&
             Op.Addr == 0x1100;
    };
    RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, &Runtime);
    const auto Result = Solver.run();
    EXPECT_EQ(Result.HasBoundSEHCallees, Mutation == 0);
    if (!Mutation)
      EXPECT_TRUE(Result.Complete);
  }
}

TEST(RegistrationCRTCall, NullControlFPHasNoOutputBorrow) {
  struct Setting {
    uint32_t Control;
    uint32_t Mask;
    bool Accepted;
  };
  for (const auto &Setting :
       {Setting{0, 0, true}, Setting{0, 0x8001f, true},
        Setting{0x10000, 0x30000, true}, Setting{0x20000, 0x30000, true},
        Setting{0, 0x30000, true}, Setting{0x30000, 0x30000, false},
        Setting{0x10000, 0x10000, false}, Setting{0, 0x70000, false}})
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << Setting.Control << ':'
                                        << Setting.Mask << ':' << Mutation);
      auto F = makeJumpFrame(0);
      auto &Setup = F.Blocks[1];
      auto Output = NdVar::cst(0, 4);
      if (Mutation == 1)
        Output = NdVar::reg(x86reg::RBP, 4);
      if (Mutation == 2)
        Output.Provenance = ConstantAddressProvenance::DataAddress;
      emitOp(Setup, 0x1086, NdOp::STORE, {},
             {NdVar::reg(x86reg::RSP, 4), Output});
      for (unsigned I = 1; I != 3; ++I) {
        emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(0, 4),
               {NdVar::reg(x86reg::RSP, 4), NdVar::cst(I * 4, 4)});
        emitOp(Setup, 0x1086, NdOp::STORE, {},
               {NdVar::tmp(0, 4),
                Mutation == 3 && I == 1
                    ? NdVar::reg(x86reg::RBP, 4)
                    : NdVar::cst(I == 1 ? Setting.Control : Setting.Mask, 4)});
      }
      Setup.InstructionBoundaries.back().OpCount = 5;
      auto &Call = F.Blocks[2];
      Call.Ops[0].Opcode = NdOp::INDIR_CALL;
      Call.Ops[0].Inputs[0] = NdVar::cst(0x4200, 4);
      Call.InstructionBoundaries[0].Immediate = 0x4200;
      Call.InstructionBoundaries[0].ControlFlags |=
          LowInstructionControlFlag::Indirect;
      auto &Resume = F.Blocks[3];
      Resume.Ops.erase(Resume.Ops.begin());
      Resume.Ops.pop_back();
      Resume.Succs = {5};
      Resume.EndAddr = 0x110b;
      Resume.InstructionBoundaries = {{0x1108, 3, 0, 1}};
      SEHJumpRuntime Runtime;
      Runtime.ControlFP = [Mutation](const LowOp &Op, size_t &) {
        return Mutation != 4 && Op.Opcode == NdOp::INDIR_CALL &&
               Op.Addr == 0x1100;
      };
      RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr,
                                     nullptr, nullptr, nullptr, nullptr,
                                     &Runtime);
      const auto Result = Solver.run();
      const bool Accepted = Setting.Accepted && Mutation == 0;
      EXPECT_EQ(Result.HasBoundSEHCallees, Accepted);
      if (Accepted) {
        EXPECT_TRUE(Result.Complete);
        ASSERT_EQ(Result.SEHBoundCalls.size(), 1u);
        const auto &Args = Result.SEHBoundCalls.front().Arguments;
        ASSERT_EQ(Args.size(), 3u);
        EXPECT_EQ(Args[0].ArgumentKind,
                  RegistrationSEHBoundArgument::Kind::NullPointer);
        const bool Precision = Setting.Mask == 0x30000;
        EXPECT_EQ(Args[1].ArgumentKind,
                  Precision ? RegistrationSEHBoundArgument::Kind::ConstantScalar
                            : RegistrationSEHBoundArgument::Kind::Scalar);
        if (Precision) {
          EXPECT_EQ(Args[1].ScalarValue, Setting.Control);
          EXPECT_EQ(Args[2].ScalarValue, Setting.Mask);
        }
      }
    }
}

TEST(RegistrationJumpState, CallbackStateReloadControlsDirectResumption) {
  for (int32_t Written : {0, -1, 2}) {
    SCOPED_TRACE(Written);
    auto F = makeJumpFrame(0);
    F.Blocks.resize(14);
    auto &Chain = *F.ExceptionMetadata->Registration;
    Chain.Scopes = {
        {-1, 0, 0x1940, true}, {0, 0, 0x1900, true}, {1, 0, 0x1800, true}};
    auto StateBlock = [&](unsigned Id, va_t Address, int32_t Level, int Next) {
      auto &Block = F.Blocks[Id];
      Block.Id = Id;
      Block.StartAddr = Address;
      Block.EndAddr = Address + 7;
      Block.Succs = {Next};
      addSlotStore(Block, Level);
      Block.Ops[0].Seq = 0;
      Block.Ops[1].Seq = 1;
      Block.InstructionBoundaries = {{Address, 7, 0, 2}};
      Chain.TryLevelStores.push_back({Address, Address + 7, Level});
    };
    StateBlock(7, 0x10a0, 0, 2);
    StateBlock(8, 0x1140, 2, 4);
    F.Blocks[1].Succs = {7};
    F.Blocks[3].Succs = {8, 5};
    auto &Setup = F.Blocks[1];
    emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
    emitOp(Setup, 0x1086, NdOp::INT_ADD, NdVar::tmp(1, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-40), 4)});
    emitOp(Setup, 0x1086, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::tmp(1, 4)});
    Setup.InstructionBoundaries.back().OpCount += 3;
    auto &Resume = F.Blocks[3];
    Resume.Ops.front().Inputs[1] = NdVar::cst(uint32_t(-28), 4);
    LowOp Read;
    Read.Opcode = NdOp::LOAD;
    Read.Addr = 0x1105;
    Read.Output = NdVar::reg(x86reg::RDI, 4);
    Read.addInput(NdVar::tmp(0, 4));
    Resume.Ops.insert(Resume.Ops.begin() + 1, Read);
    ++Resume.InstructionBoundaries.front().OpCount;
    for (unsigned I = 1; I != Resume.InstructionBoundaries.size(); ++I)
      ++Resume.InstructionBoundaries[I].FirstOp;
    for (unsigned I = 0; I != Resume.Ops.size(); ++I)
      Resume.Ops[I].Seq = I;
    auto &Jump = F.Blocks[4];
    Jump.StartAddr += 0x38;
    Jump.EndAddr += 0x38;
    for (auto &Op : Jump.Ops)
      Op.Addr += 0x38;
    for (auto &Boundary : Jump.InstructionBoundaries)
      Boundary.Address += 0x38;
    StateBlock(9, 0x1800, Written, 10);
    auto Return = [&](unsigned Id, va_t Address) {
      auto &Block = F.Blocks[Id];
      Block.Id = Id;
      Block.StartAddr = Address;
      Block.EndAddr = Address + 1;
      emitOp(Block, Address, NdOp::RETURN, {}, {});
      Block.InstructionBoundaries = {{Address, 1, 0, 1}};
      Block.InstructionBoundaries.front().Control =
          LowInstructionControl::Return;
      Block.InstructionBoundaries.front().ControlFlags =
          LowInstructionControlFlag::Return;
    };
    Return(10, 0x1807);
    auto &Middle = F.Blocks[11];
    Middle.Id = 11;
    Middle.StartAddr = 0x1900;
    Middle.EndAddr = 0x1906;
    Middle.Succs = {12};
    emitOp(Middle, 0x1900, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-28), 4)});
    emitOp(Middle, 0x1900, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::reg(x86reg::RBP, 4)});
    Middle.InstructionBoundaries = {{0x1900, 6, 0, 2}};
    Return(12, 0x1906);
    Return(13, 0x1940);
    SEHJumpRuntime Runtime;
    Runtime.Capture = [](const LowOp &Op, const FrameState &Frame, bool,
                         size_t &) -> std::optional<SEHJumpCapture> {
      if (Op.Addr != 0x1100)
        return std::nullopt;
      SEHJumpCapture Capture;
      Capture.Call = Op.Addr;
      Capture.ReturnPC = 0x1105;
      Capture.BufferOffset = -144;
      Capture.StackOffset = -192;
      Capture.Level = 0;
      Capture.UnwindTarget = 0x3300;
      constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                        x86reg::RSI};
      for (unsigned I = 0; I != 4; ++I)
        Capture.Nonvolatile[I] =
            Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
      return Capture;
    };
    Runtime.Resume = [](const LowOp &Op, size_t &) {
      return Op.Addr == 0x1152;
    };
    RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr, nullptr,
                                   nullptr, nullptr, nullptr, &Runtime);
    const auto A = Solver.run();
    const bool Valid = Written != 2;
    if (Valid && !A.Complete)
      for (const auto &Diagnostic : A.Diagnostics)
        ADD_FAILURE() << Diagnostic;
    EXPECT_EQ(A.Complete, Valid);
    EXPECT_EQ(A.SEHStateStoresComplete, Valid);
    EXPECT_EQ(A.SEHJumpResumptionsComplete, Valid);
    if (!Valid) {
      EXPECT_TRUE(A.SEHStateStores.empty());
      EXPECT_TRUE(A.SEHJumpResumptions.empty());
      continue;
    }
    ASSERT_EQ(A.SEHJumpResumptions.size(), 1u);
    EXPECT_EQ(A.SEHJumpResumptions.front().ResumedLevels,
              (std::vector<int32_t>{Written}));
    const auto Marker = std::find_if(
        A.FrameValues.begin(), A.FrameValues.end(), [](const auto &Value) {
          return Value.Address == 0x1105 && Value.OpSeq == 1;
        });
    ASSERT_NE(Marker, A.FrameValues.end());
    EXPECT_EQ(Marker->EstablishedFrameOffset, -40);
  }
}

TEST(RegistrationJumpState, ImageEffectsGateLaterCapture) {
  for (unsigned Mode = 0; Mode != 10; ++Mode) {
    SCOPED_TRACE(Mode);
    auto F = makeJumpFrame(0);
    std::vector<RegistrationCalleeFrameContract> Callees;
    std::vector<RegistrationCalleeStackContract> Stacks;
    if (Mode) {
      LowBlock Call;
      Call.Id = 10;
      Call.StartAddr = 0x1070;
      Call.EndAddr = 0x1075;
      Call.Succs = F.Blocks[0].Succs;
      Call.InstructionBoundaries = {{0x1070, 5, 0, 1}};
      Call.InstructionBoundaries[0].Control = LowInstructionControl::Call;
      Call.InstructionBoundaries[0].Immediate = 0x4300;
      emitOp(Call, 0x1070, NdOp::CALL, {}, {NdVar::cst(0x4300, 4)});
      F.Blocks[0].Succs = {10};
      F.Blocks.push_back(std::move(Call));
      Stacks.push_back({0x4300, 0, false});
      if (Mode >= 2) {
        RegistrationCalleeFrameContract Callee;
        Callee.Target = 0x4300;
        if (Mode == 3)
          Callee.ImageWrites = {{0x4100, 0x4104}};
        if (Mode == 4)
          Callee.ImageWrites = {{0x4400, 0x4404}};
        if (Mode == 5)
          // A merged image effect is not limited to one instruction's width.
          Callee.ImageWrites = {{0x4000, 0x14000}};
        if (Mode == 6)
          Callee.ImageWrites = {{0x4000, 0x4100}};
        if (Mode == 7)
          Callee.ImageWrites = {{0x4104, 0x4200}};
        if (Mode == 8)
          Callee.ImageWrites = {{0x40ff, 0x4101}};
        if (Mode == 9)
          Callee.ImageWrites = {{0x4103, 0x4104}};
        Callees.push_back(Callee);
      }
    }
    SEHJumpRuntime Runtime;
    Runtime.ImportSlots.insert(0x4100);
    Runtime.Capture = [](const LowOp &Op, const FrameState &Frame, bool,
                         size_t &) -> std::optional<SEHJumpCapture> {
      if (Op.Addr != 0x1100)
        return std::nullopt;
      SEHJumpCapture Capture;
      Capture.Call = Op.Addr;
      Capture.ReturnPC = 0x1105;
      Capture.BufferOffset = -144;
      Capture.StackOffset = -192;
      Capture.UnwindTarget = 0x3300;
      constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                        x86reg::RSI};
      for (unsigned I = 0; I != 4; ++I)
        Capture.Nonvolatile[I] =
            Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
      return Capture;
    };
    Runtime.Resume = [](const LowOp &Op, size_t &) {
      return Op.Addr == 0x111a;
    };
    RegistrationStateSolver Solver(F, 0, 0, Mode >= 2 ? &Callees : nullptr,
                                   nullptr, &Stacks, nullptr, nullptr, nullptr,
                                   nullptr, &Runtime);
    const auto Result = Solver.run();
    EXPECT_EQ(Result.HasSEHJumpCaptures,
              Mode == 0 || Mode == 2 || Mode == 4 || Mode == 6 || Mode == 7);
  }
}

TEST(RegistrationJumpState, OpaqueCallsRevokeExceptionalRuntimeBindings) {
  for (bool Filter : {false, true}) {
    SCOPED_TRACE(Filter);
    for (unsigned Mutation = 0; Mutation != 2; ++Mutation) {
      SCOPED_TRACE(Mutation);
      auto F = makeJumpFrame(0);
      F.Blocks.resize(8);
      auto &Chain = *F.ExceptionMetadata->Registration;
      Chain.Scopes =
          Filter ? std::vector<RegistrationScopeRecord>{{-1, 0x110f, 0x1120,
                                                         false}}
                 : std::vector<RegistrationScopeRecord>{{-1, 0, 0x110f, true}};
      Chain.TryLevelStores = {{0x1120, 0x1127, -1}, {0x1140, 0x1147, 0}};
      auto &Resume = F.Blocks[3];
      Resume.Ops.clear();
      Resume.Succs = {7};
      Resume.EndAddr = 0x1108;
      emitOp(Resume, 0x1105, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
             {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
      Resume.InstructionBoundaries = {{0x1105, 3, 0, 1}};
      auto &Active = F.Blocks[7];
      Active.Id = 7;
      Active.StartAddr = 0x1140;
      Active.EndAddr = 0x1147;
      Active.Succs = {5};
      addSlotStore(Active, 0);
      for (unsigned I = 0; I != Active.Ops.size(); ++I)
        Active.Ops[I].Seq = I;
      Active.InstructionBoundaries = {{0x1140, 7, 0, 2}};
      if (Mutation) {
        LowBlock Call;
        Call.Id = 8;
        Call.StartAddr = 0x1180;
        Call.EndAddr = 0x1185;
        emitOp(Call, 0x1180, NdOp::CALL, {}, {NdVar::cst(0x4300, 4)});
        Call.InstructionBoundaries = {{0x1180, 5, 0, 1}};
        auto &Insn = Call.InstructionBoundaries[0];
        Insn.Control = LowInstructionControl::Call;
        Insn.ControlFlags = LowInstructionControlFlag::Call |
                            LowInstructionControlFlag::NoReturn;
        Insn.Immediate = 0x4300;
        Active.Succs = {8};
        F.Blocks.push_back(std::move(Call));
      }
      SEHJumpRuntime Runtime;
      Runtime.ImportSlots.insert(0x4100);
      Runtime.Capture = [](const LowOp &Op, const FrameState &Frame, bool,
                           size_t &) -> std::optional<SEHJumpCapture> {
        if (Op.Addr != 0x1100)
          return std::nullopt;
        SEHJumpCapture Capture;
        Capture.Call = Op.Addr;
        Capture.ReturnPC = 0x1105;
        Capture.BufferOffset = -144;
        Capture.StackOffset = -192;
        Capture.UnwindTarget = 0x3300;
        constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                          x86reg::RSI};
        for (unsigned I = 0; I != 4; ++I)
          Capture.Nonvolatile[I] =
              Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
        return Capture;
      };
      Runtime.Resume = [](const LowOp &Op, size_t &) {
        return Op.Addr == 0x111a;
      };
      std::vector<RegistrationSEHControlContract> Controls;
      RegistrationStateSolver Solver(F, 0, 0, nullptr, nullptr, nullptr,
                                     nullptr, &Controls, nullptr, nullptr,
                                     &Runtime);
      const auto Result = Solver.run();
      if (!Mutation) {
        for (const auto &Diagnostic : Result.Diagnostics)
          ADD_FAILURE() << Diagnostic;
        EXPECT_TRUE(Result.Complete);
        ASSERT_EQ(Result.SEHJumpResumptions.size(), 1u);
        EXPECT_EQ(Result.SEHJumpResumptions.front().EntryVA, 0x110fu);
        EXPECT_EQ(Result.SEHJumpResumptions.front().TargetVA, 0x1105u);
        EXPECT_EQ(Result.SEHJumpResumptions.front().ValueOffset, -4);
        EXPECT_EQ(Result.SEHJumpResumptions.front().ResumedLevels,
                  std::vector<int32_t>{-1});
      } else {
        EXPECT_FALSE(Result.Complete);
        EXPECT_FALSE(Result.SEHJumpResumptionsComplete);
        EXPECT_TRUE(Result.SEHJumpResumptions.empty());
      }
    }
  }
}
