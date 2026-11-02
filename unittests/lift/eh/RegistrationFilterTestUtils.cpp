//===- RegistrationFilterTestUtils.cpp - SEH search fixtures -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationFilterTestUtils.h"

#include "../../../lib/ir/low/X86/RegistrationStateSolver.h"
#include "RegistrationStateTestUtils.h"

#include "neverd/lift/X86/X86Regs.h"

#include <utility>

using namespace neverd::registration_state;
using namespace neverd::registration_test;

namespace neverd::registration_filter_test {

static void addReturn(LowBlock &Block, va_t Address) {
  const auto First = Block.Ops.size();
  emitOp(Block, Address, NdOp::RETURN, {}, {});
  Block.EndAddr = Address + 1;
  Block.InstructionBoundaries.push_back({Address, 1, First, 1});
  auto &Boundary = Block.InstructionBoundaries.back();
  Boundary.Control = LowInstructionControl::Return;
  Boundary.ControlFlags = LowInstructionControlFlag::Return;
}

void addStateStore(LowBlock &Block, va_t Address, int32_t Level,
                   RegistrationChainInfo &Chain) {
  const auto First = Block.Ops.size();
  emitOp(Block, Address, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-4), 4)});
  emitOp(Block, Address, NdOp::STORE, {},
         {NdVar::tmp(0, 4), NdVar::cst(uint32_t(Level), 4)});
  Block.InstructionBoundaries.push_back({Address, 7, First, 2});
  Chain.TryLevelStores.push_back({Address, Address + 7, Level});
}

LowFunc filterFrame(Variant V, unsigned Protocol) {
  auto F = makeJumpFrame();
  F.Blocks.resize(V == Variant::StoresAroundRaise ? 18 : 16);
  auto &Chain = *F.ExceptionMetadata->Registration;
  Chain.Scopes = {{-1, 0x1880, 0x1120, false},
                  {0, 0x1800, 0x1120, false},
                  {1, 0, 0x1840, true}};
  if (V == Variant::SkipTwoFiltersBeforeResume)
    Chain.Scopes.push_back({1, 0x1800, 0x1120, false});
  if (V == Variant::TwoResumeMappings)
    Chain.Scopes = {{-1, 0x1880, 0x1120, false},
                    {0, 0x1880, 0x1120, false},
                    {1, 0x1880, 0x1120, false},
                    {0, 0x1800, 0x1120, false},
                    {3, 0, 0x1840, true}};
  F.Blocks[3].Succs = {8, 5};
  auto &Active = F.Blocks[8];
  Active.Id = 8;
  Active.StartAddr = 0x1140;
  Active.EndAddr = 0x1147;
  Active.Succs = {4};
  addStateStore(Active, 0x1140, V == Variant::TwoResumeMappings ? 4 : 2, Chain);
  for (auto &Op : F.Blocks[4].Ops)
    Op.Addr += 0x41;
  for (auto &Boundary : F.Blocks[4].InstructionBoundaries)
    Boundary.Address += 0x41;
  F.Blocks[4].StartAddr += 0x41;
  F.Blocks[4].EndAddr += 0x41;

  auto &Filter = F.Blocks[7];
  Filter.Id = 7;
  Filter.StartAddr = 0x1800;
  Filter.EndAddr = 0x1802;
  Filter.Succs = V == Variant::ReversedEdges ? std::vector<int>{12, 11}
                                             : std::vector<int>{11, 12};
  emitOp(Filter, 0x1800, NdOp::COND_BR, {},
         {NdVar::cst(0x1940, 8), NdVar::reg(x86reg::RCX, 4)});
  Filter.InstructionBoundaries = {{0x1800, 2, 0, 1}};
  Filter.InstructionBoundaries.back().Control = LowInstructionControl::Branch;
  Filter.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Branch |
      LowInstructionControlFlag::Conditional;
  for (unsigned I = 0; I != 2; ++I) {
    auto &Arm = F.Blocks[11 + I];
    Arm.Id = 11 + I;
    Arm.StartAddr = I ? 0x1940 : 0x1900;
    Arm.EndAddr = Arm.StartAddr + 7;
    Arm.Succs = {int(14 + I)};
    const int32_t Level = V == Variant::TwoResumeMappings        ? (I ? 3 : 2)
                          : V == Variant::SameState              ? 0
                          : V == Variant::HandlerCleanup         ? 3
                          : V == Variant::RecurrentSelection     ? 2
                          : V == Variant::CapturedCursor || !I   ? -1
                          : V == Variant::SkipFilterBeforeResume ? 1
                          : V == Variant::SkipTwoFiltersBeforeResume ? 3
                                                                     : 0;
    addStateStore(Arm, Arm.StartAddr, Level, Chain);
    const uint32_t Disposition =
        V == Variant::HandlerOnly || V == Variant::HandlerCleanup ||
                V == Variant::RecurrentSelection
            ? 1
        : !I && V == Variant::NegativeDisposition ? 0x80000000u
        : !I || V == Variant::BothContinue        ? UINT32_MAX
        : V == Variant::SelectedHandler           ? 1
                                                  : 0;
    auto &DispositionBlock = F.Blocks[14 + I];
    DispositionBlock.Id = 14 + I;
    DispositionBlock.StartAddr = Arm.EndAddr;
    DispositionBlock.EndAddr = Arm.EndAddr + 5;
    DispositionBlock.Succs = {13};
    emitOp(
        DispositionBlock, Arm.EndAddr, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
        {I && V == Variant::UnknownDisposition ? NdVar::reg(x86reg::RCX, 4)
                                               : NdVar::cst(Disposition, 4)});
    DispositionBlock.InstructionBoundaries = {{Arm.EndAddr, 5, 0, 1}};
  }
  if (V == Variant::RecurrentArm)
    F.Blocks[12].Succs = {12};
  if (V == Variant::WrongStoreWidth)
    F.Blocks[12].Ops[1].Inputs[1].Size = 2;
  auto &Return = F.Blocks[13];
  Return.Id = 13;
  Return.StartAddr = 0x1980;
  if (V == Variant::BrokenStack) {
    emitOp(Return, 0x1980, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
    Return.InstructionBoundaries = {{0x1980, 3, 0, 1}};
  }
  addReturn(Return, V == Variant::BrokenStack ? 0x1983 : 0x1980);

  auto &Outer = F.Blocks[10];
  Outer.Id = 10;
  Outer.StartAddr = 0x1880;
  emitOp(Outer, 0x1880, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(V == Variant::NegativeDisposition ? UINT32_MAX - 1
                     : V == Variant::EscapingSearch    ? 0
                     : V == Variant::OuterHandler      ? 1
                                                       : UINT32_MAX,
                     4)});
  Outer.InstructionBoundaries = {{0x1880, 5, 0, 1}};
  addReturn(Outer, 0x1885);

  auto &FinallyEntry = F.Blocks[9];
  FinallyEntry.Id = 9;
  FinallyEntry.StartAddr = 0x1840;
  auto &Finally = V == Variant::StoresAroundRaise ? F.Blocks[16] : FinallyEntry;
  if (V == Variant::StoresAroundRaise) {
    addStateStore(FinallyEntry, 0x1840, 1, Chain);
    FinallyEntry.EndAddr = 0x1847;
    FinallyEntry.Succs = {16};
    Finally.Id = 16;
    Finally.StartAddr = 0x1847;
  }
  const va_t Arguments = Finally.StartAddr;
  const size_t First = Finally.Ops.size();
  emitOp(Finally, Arguments, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  for (unsigned I = 0; I != 4; ++I) {
    emitOp(Finally, Arguments, NdOp::INT_ADD, NdVar::tmp(0, 4),
           {NdVar::reg(x86reg::RSP, 4), NdVar::cst(I * 4, 4)});
    emitOp(Finally, Arguments, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::cst(0, 4)});
  }
  Finally.InstructionBoundaries.push_back(
      {Arguments, 16, First, Finally.Ops.size() - First});
  const auto Call = Finally.Ops.size();
  emitOp(Finally, Arguments + 16, NdOp::INDIR_CALL, {},
         {NdVar::cst(0x4100, 4)});
  Finally.InstructionBoundaries.push_back({Arguments + 16, 6, Call, 1});
  Finally.InstructionBoundaries.back().Control = LowInstructionControl::Call;
  Finally.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Call | LowInstructionControlFlag::Indirect;
  if (V == Variant::IncompleteCallInstruction) {
    emitOp(Finally, Arguments + 16, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
           {NdVar::cst(0, 4)});
    ++Finally.InstructionBoundaries.back().OpCount;
  }
  va_t End = Arguments + 22;
  if (V == Variant::StoresAroundRaise) {
    addStateStore(Finally, End, 0, Chain);
    End += 7;
    Finally.EndAddr = End;
    Finally.Succs = {17};
    auto &EndBlock = F.Blocks[17];
    EndBlock.Id = 17;
    EndBlock.StartAddr = End;
    addReturn(EndBlock, End);
  } else {
    addReturn(Finally, End);
  }
  if (V != Variant::SkipFilterAfterCallback) {
    LowBlock InitialState;
    InitialState.Id = F.Blocks.size();
    InitialState.StartAddr = 0x1088;
    InitialState.EndAddr = 0x108f;
    InitialState.Succs = {2};
    F.Blocks[1].Succs = {InitialState.Id};
    addStateStore(InitialState, 0x1088, V == Variant::TwoResumeMappings ? 1 : 0,
                  Chain);
    F.Blocks.push_back(std::move(InitialState));
  }
  if (V == Variant::HandlerCleanup) {
    LowBlock Cleanup;
    Cleanup.Id = F.Blocks.size();
    Cleanup.StartAddr = 0x1a00;
    addReturn(Cleanup, 0x1a00);
    F.Blocks.push_back(std::move(Cleanup));
    Chain.Scopes.push_back({1, 0, 0x1a00, true});
  }
  if (Protocol) {
    initializeEH4JumpFrame(F, Protocol == 2);
    for (auto &Store : Chain.TryLevelStores)
      if (Store.Level == -1) {
        Store.Level = -2;
        for (auto &Block : F.Blocks)
          for (auto &Op : Block.Ops)
            if (Op.Addr == Store.StoreVA && Op.Opcode == NdOp::STORE)
              Op.Inputs[1] = NdVar::cst(uint32_t(-2), 4);
      }
  }
  for (auto &Block : F.Blocks)
    for (unsigned I = 0; I != Block.Ops.size(); ++I)
      Block.Ops[I].Seq = I;
  return F;
}

RegistrationStateAnalysis analyze(LowFunc F, Variant V, va_t CookieVA,
                                  bool UnwindJump) {
  const bool EH4 =
      F.ExceptionMetadata->Personality == ExceptionPersonality::ExceptHandler4;
  SEHJumpRuntime Runtime;
  Runtime.ImportSlots.insert(0x4100);
  Runtime.Capture = [V, EH4](const LowOp &Op, const FrameState &Frame, bool,
                             size_t &) -> std::optional<SEHJumpCapture> {
    if (Op.Addr != 0x1100)
      return std::nullopt;
    SEHJumpCapture Capture;
    Capture.Call = Op.Addr;
    Capture.ReturnPC = 0x1105;
    Capture.BufferOffset = -144;
    Capture.StackOffset = -192;
    Capture.UnwindTarget = 0x3300;
    Capture.Level = V == Variant::SkipFilterAfterCallback ? (EH4 ? -2 : -1)
                    : V == Variant::TwoResumeMappings     ? 1
                                                          : 0;
    if (EH4) {
      Capture.Protocol = RegistrationSEHJumpCapture::Kind::EH4;
      Capture.SecurityCookieVA = 0x4000;
    }
    constexpr uint64_t Registers[] = {x86reg::RBP, x86reg::RBX, x86reg::RDI,
                                      x86reg::RSI};
    for (unsigned I = 0; I != 4; ++I)
      Capture.Nonvolatile[I] =
          Frame.Registers[Registers[I] / x86reg::GeneralRegStride];
    return Capture;
  };
  Runtime.Resume = [](const LowOp &Op, size_t &) {
    return Op.Addr == 0x115b || Op.Addr == 0x1a0b;
  };
  Runtime.Raise = [](const LowOp &Op, const FrameState &, size_t &) {
    return Op.Addr == 0x1850 || Op.Addr == 0x1857;
  };
  Runtime.ImageMemory = [](va_t Address, uint16_t Width, bool Write, size_t &) {
    return Address == 0x4400 && Width == 4 && !Write;
  };
  std::vector<RegistrationSEHControlContract> Controls;
  if (UnwindJump)
    for (const auto &Block : F.Blocks)
      for (const auto &Op : Block.Ops)
        if (Op.Opcode == NdOp::RETURN)
          Controls.push_back({RegistrationSEHControlContract::Kind::NearReturn,
                              Op.Addr, Block.EndAddr, 0});
  RegistrationStateSolver Solver(
      F, EH4 ? CookieVA : 0, 0, nullptr, nullptr, nullptr, nullptr,
      UnwindJump ? &Controls : nullptr, nullptr, nullptr, &Runtime);
  return Solver.run();
}

RegistrationStateAnalysis analyze(Variant V, unsigned Protocol) {
  return analyze(filterFrame(V, Protocol), V);
}

LowFunc callbackActivatedFrame() {
  auto F = filterFrame(Variant::TwoResumeMappings);
  auto &Chain = *F.ExceptionMetadata->Registration;
  // State 2 is entered only by the search filter. Give its roots distinct
  // addresses so another scope cannot accidentally provide their proof.
  Chain.Scopes[2].FilterVA = 0x19c0;
  Chain.Scopes[2].HandlerVA = 0x19d0;
  LowBlock Filter;
  Filter.Id = F.Blocks.size();
  Filter.StartAddr = 0x19c0;
  emitOp(Filter, 0x19c0, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(UINT32_MAX, 4)});
  Filter.InstructionBoundaries = {{0x19c0, 5, 0, 1}};
  addReturn(Filter, 0x19c5);
  F.Blocks.push_back(std::move(Filter));
  LowBlock Handler;
  Handler.Id = F.Blocks.size();
  Handler.StartAddr = 0x19d0;
  Handler.EndAddr = 0x19d7;
  Handler.Succs = {6};
  addStateStore(Handler, 0x19d0, -1, Chain);
  F.Blocks.push_back(std::move(Handler));
  for (auto &Block : F.Blocks)
    for (unsigned I = 0; I != Block.Ops.size(); ++I)
      Block.Ops[I].Seq = I;
  return F;
}
} // namespace neverd::registration_filter_test
