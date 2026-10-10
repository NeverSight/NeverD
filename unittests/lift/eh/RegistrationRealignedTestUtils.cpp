//===- RegistrationRealignedTestUtils.cpp - x86 aligned frame fixture -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "RegistrationRealignedTestUtils.h"

#include "neverd/lift/X86Regs.h"

namespace neverd::registration_test {

void emit(LowBlock &Block, va_t Address, NdOp Opcode, NdVar Output,
          std::initializer_list<NdVar> Inputs, NdMemoryAddressSpace Space) {
  LowOp Op;
  Op.Addr = Address;
  Op.Seq = Block.Ops.size();
  Op.Opcode = Opcode;
  Op.Output = Output;
  Op.MemoryAddressSpace = Space;
  for (const auto &Input : Inputs)
    Op.addInput(Input);
  Block.Ops.push_back(Op);
}

LowFunc makeRealignedRegistrationFrame() {
  LowFunc F;
  F.Entry = 0x1000;
  auto &EH = F.ExceptionMetadata.emplace();
  EH.CodeRange = {0x1000, 0x1061};
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  EH.Personality = ExceptionPersonality::CxxFrameHandler3;
  auto &Cxx = EH.Cxx.emplace();
  Cxx.MaxState = 1;
  Cxx.UnwindMap.resize(1);
  Cxx.UnwindMap[0].Kind = CxxUnwindAction::ActionKind::None;
  auto &Chain = EH.Registration.emplace();
  Chain.HandlerVA = 0x2000;
  Chain.SeededTryLevel = -1;
  Chain.TryLevelOffset = -4;
  Chain.RegistrationOffset = -12;
  Chain.ChainInstallVA = 0x1044;
  Chain.RealignedFrame =
      RegistrationRealignedFrame{6, 0x100f, 16, 64, -60, -20};
  Chain.TryLevelStores = {{0x101d, 0x1027, -1}, {0x104a, 0x1054, 0}};
  F.Blocks.resize(4);
  const va_t Begins[] = {0x1000, 0x1027, 0x104a, 0x1054};
  const va_t Ends[] = {0x1027, 0x104a, 0x1054, 0x1061};
  for (unsigned I = 0; I != 4; ++I) {
    auto &B = F.Blocks[I];
    B.Id = I;
    B.StartAddr = Begins[I];
    B.EndAddr = Ends[I];
    if (I != 3)
      B.Succs = {int(I + 1)};
  }
  auto &Entry = F.Blocks[0];
  Entry.InstructionBoundaries = {{0x1000, 1}, {0x1001, 2}, {0x1003, 1},
                                 {0x1004, 1}, {0x1005, 1}, {0x1006, 3},
                                 {0x1009, 6}, {0x100f, 2}, {0x1011, 6},
                                 {0x1017, 6}, {0x101d, 10}};
  const auto SP = NdVar::reg(x86reg::RSP, 4);
  const auto FP = NdVar::reg(x86reg::RBP, 4);
  const auto Local = NdVar::reg(x86reg::RSI, 4);
  auto Push = [&](va_t Address, NdVar Value) {
    emit(Entry, Address, NdOp::INT_SUB, SP, {SP, NdVar::cst(4, 4)});
    emit(Entry, Address, NdOp::STORE, {}, {SP, Value});
  };
  Push(0x1000, FP);
  emit(Entry, 0x1001, NdOp::COPY, FP, {SP});
  Push(0x1003, NdVar::reg(x86reg::RBX, 4));
  Push(0x1004, NdVar::reg(x86reg::RDI, 4));
  Push(0x1005, Local);
  emit(Entry, 0x1006, NdOp::INT_AND, SP, {SP, NdVar::cst(0xfffffff0, 4)});
  emit(Entry, 0x1009, NdOp::INT_SUB, SP, {SP, NdVar::cst(64, 4)});
  emit(Entry, 0x100f, NdOp::COPY, Local, {SP});
  auto Store = [&](LowBlock &B, va_t Address, int Offset, NdVar Value) {
    emit(B, Address, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {Local, NdVar::cst(Offset, 4)});
    emit(B, Address, NdOp::STORE, {}, {NdVar::tmp(0, 4), Value});
  };
  Store(Entry, 0x1011, 40, FP);
  Store(Entry, 0x1017, 44, SP);
  Store(Entry, 0x101d, 56, NdVar::cst(0xffffffff, 4));
  auto &Install = F.Blocks[1];
  Install.InstructionBoundaries = {
      {0x1027, 6}, {0x102d, 10}, {0x1037, 7}, {0x103e, 6}, {0x1044, 6}};
  const auto Node = NdVar::reg(x86reg::RAX, 4);
  const auto Link = NdVar::reg(x86reg::RDX, 4);
  emit(Install, 0x1027, NdOp::INT_ADD, Node, {Local, NdVar::cst(48, 4)});
  Store(Install, 0x102d, 52, NdVar::cst(0x2000, 4));
  emit(Install, 0x1037, NdOp::LOAD, Link, {NdVar::cst(0, 4)},
       NdMemoryAddressSpace::X86FS);
  Store(Install, 0x103e, 48, Link);
  emit(Install, 0x1044, NdOp::STORE, {}, {NdVar::cst(0, 4), Node},
       NdMemoryAddressSpace::X86FS);
  F.Blocks[2].InstructionBoundaries = {{0x104a, 10}};
  Store(F.Blocks[2], 0x104a, 56, NdVar::cst(0, 4));
  auto &Exit = F.Blocks[3];
  Exit.InstructionBoundaries = {{0x1054, 6}, {0x105a, 7}};
  emit(Exit, 0x1054, NdOp::INT_ADD, NdVar::tmp(0, 4),
       {Local, NdVar::cst(48, 4)});
  emit(Exit, 0x1054, NdOp::LOAD, Link, {NdVar::tmp(0, 4)});
  emit(Exit, 0x105a, NdOp::STORE, {}, {NdVar::cst(0, 4), Link},
       NdMemoryAddressSpace::X86FS);
  return F;
}

} // namespace neverd::registration_test
