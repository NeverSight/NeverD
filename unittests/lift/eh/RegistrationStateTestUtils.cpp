//===- RegistrationStateTestUtils.cpp - x86 EH state fixtures -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationStateTestUtils.h"

#include "neverd/lift/X86/X86Regs.h"

namespace neverd::registration_test {

void emitOp(LowBlock &Block, va_t Address, NdOp Opcode, NdVar Output,
            std::initializer_list<NdVar> Inputs, NdMemoryAddressSpace Space) {
  LowOp Op;
  Op.Opcode = Opcode;
  Op.Seq = Block.Ops.size();
  Op.Output = Output;
  Op.Addr = Address;
  Op.MemoryAddressSpace = Space;
  for (NdVar Input : Inputs)
    Op.addInput(Input);
  Block.Ops.push_back(Op);
}

void addSlotStore(LowBlock &Block, int32_t Value, uint16_t Width) {
  LowOp Address;
  Address.Opcode = NdOp::INT_ADD;
  Address.Output = NdVar::tmp(0, 4);
  Address.addInput(NdVar::reg(x86reg::RBP, 4));
  Address.addInput(NdVar::cst(uint32_t(-4), 4));
  Address.Addr = Block.StartAddr;
  Block.Ops.push_back(Address);
  LowOp Store;
  Store.Opcode = NdOp::STORE;
  Store.addInput(Address.Output);
  Store.addInput(NdVar::cst(uint32_t(Value), Width));
  Store.Addr = Block.StartAddr;
  Block.Ops.push_back(Store);
}

void addFinallyCallTrailingEffect(LowBlock &Call) {
  auto &Instruction = Call.InstructionBoundaries.back();
  emitOp(Call, Instruction.Address, NdOp::COPY, NdVar::reg(x86reg::RBP, 4),
         {NdVar::cst(0, 4)});
  ++Instruction.OpCount;
}

void addFinallyTailJump(LowBlock &Call, LowBlock &Resume,
                        std::vector<RegistrationSEHControlContract> &Controls,
                        unsigned Mutation) {
  const va_t OriginalBegin = Resume.StartAddr;
  const va_t OriginalEnd = Resume.EndAddr;
  Resume.StartAddr += 0x10;
  Resume.EndAddr += 0x10;
  for (auto &Op : Resume.Ops)
    Op.Addr += 0x10;
  for (auto &Boundary : Resume.InstructionBoundaries)
    Boundary.Address += 0x10;
  for (auto &Control : Controls)
    if (Control.Address >= OriginalBegin && Control.Address < OriginalEnd) {
      Control.Address += 0x10;
      Control.EndAddress += 0x10;
    }
  const va_t ReturnPC = Call.EndAddr;
  const size_t First = Call.Ops.size();
  emitOp(Call, ReturnPC, NdOp::BRANCH, {}, {NdVar::cst(Resume.StartAddr, 8)});
  Call.EndAddr += 2;
  Call.InstructionBoundaries.push_back({ReturnPC, 2, First, 1});
  auto &Tail = Call.InstructionBoundaries.back();
  Tail.Control = LowInstructionControl::Branch;
  Tail.ControlFlags = LowInstructionControlFlag::Branch;
  Tail.Immediate = Resume.StartAddr;
  Controls.push_back({RegistrationSEHControlContract::Kind::NearJump, ReturnPC,
                      Call.EndAddr, Resume.StartAddr});
  switch (Mutation) {
  case 0:
    break;
  case 1:
    Controls.pop_back();
    break;
  case 2:
    ++Controls.back().Target;
    break;
  case 3:
    Tail.ControlFlags |= LowInstructionControlFlag::Conditional;
    break;
  case 4:
    Tail.ControlFlags |= LowInstructionControlFlag::InstructionGuard;
    break;
  case 5:
    Call.Ops.back().Opcode = NdOp::COPY;
    Call.Ops.back().Output = NdVar::reg(x86reg::RBP, 4);
    Call.Ops.back().Inputs[0] = NdVar::cst(0, 4);
    emitOp(Call, ReturnPC, NdOp::BRANCH, {}, {NdVar::cst(Resume.StartAddr, 8)});
    Tail.OpCount = 2;
    break;
  case 6:
    Call.Ops.back().MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
    break;
  case 7:
    Call.Ops.back().MemoryOrdering = NdMemoryOrdering::Acquire;
    break;
  case 8:
    --Controls.back().EndAddress;
    break;
  }
}

LowFunc makeBranchingFrame() {
  LowFunc F;
  F.Entry = 0x1000;
  F.ExceptionMetadata.emplace();
  F.ExceptionMetadata->CodeRange = {0x1000, 0x2000};
  F.ExceptionMetadata->Personality = ExceptionPersonality::ExceptHandler3;
  F.ExceptionMetadata->Encoding = ExceptionEncoding::X86ScopeTableEH3;
  RegistrationChainInfo &Chain = F.ExceptionMetadata->Registration.emplace();
  Chain.SeededTryLevel = -1;
  Chain.TryLevelOffset = -4;
  Chain.RegistrationOffset = -16;
  Chain.ChainInstallVA = 0x1000;
  Chain.Scopes.push_back({-1, 0x1800, 0x1900, false});
  Chain.TryLevelStores = {{0x1010, 0x1017, 0}, {0x1020, 0x1027, -1}};
  F.Blocks.resize(4);
  for (int I = 0; I < 4; ++I) {
    LowBlock &B = F.Blocks[I];
    B.Id = I;
    B.StartAddr = 0x1000 + I * 0x10;
    B.EndAddr = B.StartAddr + 7;
    B.InstructionBoundaries.push_back({B.StartAddr, 7});
  }
  F.Blocks[0].Succs = {1, 2};
  F.Blocks[1].Succs = {3};
  F.Blocks[2].Succs = {3};
  auto Emit = [&](NdOp Opcode, NdVar Output,
                  std::initializer_list<NdVar> Inputs,
                  NdMemoryAddressSpace Space = NdMemoryAddressSpace::Default) {
    LowOp Op;
    Op.Opcode = Opcode;
    Op.Seq = F.Blocks[0].Ops.size();
    Op.Output = Output;
    Op.Addr = 0x1000;
    Op.MemoryAddressSpace = Space;
    for (NdVar Input : Inputs)
      Op.addInput(Input);
    F.Blocks[0].Ops.push_back(Op);
  };
  Emit(NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
       {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
  Emit(NdOp::COPY, NdVar::reg(x86reg::RBP, 4), {NdVar::reg(x86reg::RSP, 4)});
  Emit(NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
       {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  Emit(NdOp::LOAD, NdVar::tmp(8, 4), {NdVar::cst(0, 8)},
       NdMemoryAddressSpace::X86FS);
  Emit(NdOp::STORE, {}, {NdVar::reg(x86reg::RSP, 4), NdVar::tmp(8, 4)});
  LowOp Install;
  Install.Opcode = NdOp::STORE;
  Install.Seq = F.Blocks[0].Ops.size();
  Install.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
  Install.addInput(NdVar::cst(0, 8));
  Install.addInput(NdVar::reg(x86reg::RSP, 4));
  Install.Addr = 0x1000;
  F.Blocks[0].Ops.push_back(Install);
  addSlotStore(F.Blocks[1], 0);
  addSlotStore(F.Blocks[2], -1);
  return F;
}

LowFunc makeCookieFrame(bool GS) {
  auto F = makeBranchingFrame();
  auto &EH = *F.ExceptionMetadata;
  EH.Personality = ExceptionPersonality::ExceptHandler4;
  EH.Encoding = ExceptionEncoding::X86ScopeTableEH4;
  auto &Chain = *EH.Registration;
  Chain.SeededTryLevel = -2;
  Chain.ScopeTableVA = 0x3000;
  Chain.EHCookieOffset = -28;
  Chain.GSCookieOffset = GS ? -36 : -2;
  Chain.GSCookieXOROffset = GS ? 4 : 0;
  Chain.HasSecurityCookies = GS;
  Chain.Scopes.front().EnclosingLevel = -2;
  Chain.TryLevelStores.back().Level = -2;
  F.Blocks[2].Ops.back().Inputs[1] = NdVar::cst(uint32_t(-2), 4);
  auto &Entry = F.Blocks.front();
  auto Install = Entry.Ops.back();
  Entry.Ops.pop_back();
  emitOp(Entry, 0x1000, NdOp::COPY, NdVar::tmp(20, 4),
         {NdVar::reg(x86reg::RSP, 4)});
  emitOp(Entry, 0x1000, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(24, 4)});
  emitOp(Entry, 0x1000, NdOp::LOAD, NdVar::tmp(21, 4), {NdVar::cst(0x4000, 4)});
  emitOp(Entry, 0x1000, NdOp::INT_XOR, NdVar::tmp(22, 4),
         {NdVar::tmp(21, 4), NdVar::cst(0x3000, 4)});
  auto Store = [&](int32_t Slot, NdVar Value) {
    emitOp(Entry, 0x1000, NdOp::INT_ADD, NdVar::tmp(24, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(Slot), 4)});
    emitOp(Entry, 0x1000, NdOp::STORE, {}, {NdVar::tmp(24, 4), Value});
  };
  Store(-8, NdVar::tmp(22, 4));
  emitOp(Entry, 0x1000, NdOp::INT_XOR, NdVar::tmp(23, 4),
         {NdVar::tmp(21, 4), NdVar::reg(x86reg::RBP, 4)});
  Store(-28, NdVar::tmp(23, 4));
  if (GS) {
    emitOp(Entry, 0x1000, NdOp::INT_ADD, NdVar::tmp(25, 4),
           {NdVar::reg(x86reg::RBP, 4), NdVar::cst(4, 4)});
    emitOp(Entry, 0x1000, NdOp::INT_XOR, NdVar::tmp(26, 4),
           {NdVar::tmp(21, 4), NdVar::tmp(25, 4)});
    Store(-36, NdVar::tmp(26, 4));
  }
  Install.Seq = Entry.Ops.size();
  Install.Inputs[1] = NdVar::tmp(20, 4);
  Entry.Ops.push_back(Install);
  auto &Exit = F.Blocks.back();
  emitOp(Exit, Exit.StartAddr, NdOp::INT_ADD, NdVar::tmp(30, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::LOAD, NdVar::tmp(31, 4),
         {NdVar::tmp(30, 4)});
  emitOp(Exit, Exit.StartAddr, NdOp::STORE, {},
         {NdVar::cst(0, 4), NdVar::tmp(31, 4)}, NdMemoryAddressSpace::X86FS);
  return F;
}

LowFunc makeCxxCatchContinuation(bool IncludeResume) {
  auto F = makeBranchingFrame();
  auto &EH = *F.ExceptionMetadata;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  auto &Chain = *EH.Registration;
  Chain.RegistrationOffset = -12;
  Chain.Scopes.clear();
  Chain.TryLevelStores = {
      {0x1016, 0x101d, 0}, {0x1020, 0x1027, -1}, {0x1900, 0x1907, -1}};
  auto &Cxx = EH.Cxx.emplace();
  Cxx.MaxState = 2;
  Cxx.UnwindMap = {{-1, 0, CxxUnwindAction::ActionKind::None},
                   {-1, 0, CxxUnwindAction::ActionKind::None}};
  CxxTryBlock Try;
  Try.TryLow = Try.TryHigh = 0;
  Try.CatchHigh = 1;
  CxxCatchHandler Catch;
  Catch.HandlerVA = 0x1800;
  Try.Handlers.push_back(Catch);
  Cxx.TryBlocks.push_back(Try);

  F.Blocks.resize(IncludeResume ? 7 : 6);
  F.Blocks[0].Ops[2].Inputs[1] = NdVar::cst(12, 4);
  F.Blocks[0].Succs = {1};
  for (size_t I = 1; I < F.Blocks.size(); ++I) {
    F.Blocks[I] = LowBlock{};
    F.Blocks[I].Id = I;
    F.Blocks[I].StartAddr = 0x1000 + I * 0x10;
    F.Blocks[I].EndAddr = F.Blocks[I].StartAddr + 7;
  }
  auto &Body = F.Blocks[1];
  Body.EndAddr = 0x101d;
  Body.InstructionBoundaries = {{0x1010, 3}, {0x1013, 3}, {0x1016, 7}};
  Body.Succs = {2};
  emitOp(Body, 0x1010, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  emitOp(Body, 0x1013, NdOp::INT_ADD, NdVar::tmp(40, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(Body, 0x1013, NdOp::STORE, {},
         {NdVar::tmp(40, 4), NdVar::reg(x86reg::RSP, 4)});
  LowBlock StateStore;
  StateStore.StartAddr = 0x1016;
  addSlotStore(StateStore, 0);
  Body.Ops.insert(Body.Ops.end(), StateStore.Ops.begin(), StateStore.Ops.end());

  F.Blocks[2].InstructionBoundaries = {{0x1020, 7}};
  F.Blocks[2].Succs = {3};
  addSlotStore(F.Blocks[2], -1);
  auto &Unlink = F.Blocks[3];
  Unlink.InstructionBoundaries = {{0x1030, 7}};
  Unlink.Succs = {4};
  emitOp(Unlink, 0x1030, NdOp::INT_ADD, NdVar::tmp(50, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-12), 4)});
  emitOp(Unlink, 0x1030, NdOp::LOAD, NdVar::tmp(51, 4), {NdVar::tmp(50, 4)});
  emitOp(Unlink, 0x1030, NdOp::STORE, {}, {NdVar::cst(0, 4), NdVar::tmp(51, 4)},
         NdMemoryAddressSpace::X86FS);
  auto AddReturn = [&](LowBlock &Block, va_t Address) {
    LowInstructionBoundary Return;
    Return.Address = Address;
    Return.Size = 1;
    Return.Control = LowInstructionControl::Return;
    Block.InstructionBoundaries.push_back(Return);
    Block.EndAddr = Address + 1;
    emitOp(Block, Address, NdOp::RETURN, {}, {NdVar::reg(x86reg::RAX, 4)});
  };
  AddReturn(F.Blocks[4], 0x1040);
  auto &Handler = F.Blocks[5];
  Handler.StartAddr = 0x1800;
  Handler.InstructionBoundaries = {{0x1800, 5}};
  emitOp(Handler, 0x1800, NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(0x1900, 4)});
  AddReturn(Handler, 0x1805);
  if (IncludeResume) {
    auto &Resume = F.Blocks[6];
    Resume.StartAddr = 0x1900;
    Resume.EndAddr = 0x1907;
    Resume.InstructionBoundaries = {{0x1900, 7}};
    Resume.Succs = {3};
    addSlotStore(Resume, -1);
  }
  return F;
}

} // namespace neverd::registration_test
