//===- RegistrationJumpTestUtils.cpp - EH3 jump frame fixtures ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Builds checked source frames shared by nonlocal and filter-state tests.

#include "RegistrationStateTestUtils.h"

#include "neverd/lift/X86Regs.h"

#include <utility>

namespace neverd::registration_test {
LowFunc makeJumpFrame(unsigned Mutation) {
  auto F = makeBranchingFrame();
  F.Blocks.resize(7);
  F.Blocks[0].Succs = {1};
  for (unsigned I = 1; I != F.Blocks.size(); ++I) {
    F.Blocks[I] = {};
    F.Blocks[I].Id = I;
  }
  auto &Setup = F.Blocks[1];
  Setup.StartAddr = 0x1080;
  Setup.EndAddr = 0x1088;
  Setup.Succs = {2};
  emitOp(Setup, 0x1080, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(160, 4)});
  emitOp(Setup, 0x1083, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-24), 4)});
  emitOp(Setup, 0x1083, NdOp::STORE, {},
         {NdVar::tmp(0, 4), NdVar::reg(x86reg::RSP, 4)});
  emitOp(Setup, 0x1086, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  Setup.InstructionBoundaries = {
      {0x1080, 3, 0, 1}, {0x1083, 3, 1, 2}, {0x1086, 2, 3, 1}};
  auto &Capture = F.Blocks[2];
  Capture.StartAddr = 0x1100;
  Capture.EndAddr = 0x1105;
  Capture.Succs = {3};
  emitOp(Capture, 0x1100, NdOp::CALL, NdVar::reg(x86reg::RAX, 4),
         {NdVar::cst(0x3000, 4)});
  Capture.InstructionBoundaries = {{0x1100, 5, 0, 1}};
  Capture.InstructionBoundaries[0].Control = LowInstructionControl::Call;
  Capture.InstructionBoundaries[0].ControlFlags =
      LowInstructionControlFlag::Call;
  Capture.InstructionBoundaries[0].Immediate = 0x3000;
  auto &Resume = F.Blocks[3];
  Resume.StartAddr = 0x1105;
  Resume.EndAddr = 0x110f;
  Resume.Succs = {4, 5};
  emitOp(Resume, 0x1105, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4),
          NdVar::cst(uint32_t(Mutation == 2 ? -104 : -143), 4)});
  if (Mutation == 1 || Mutation == 2)
    emitOp(Resume, 0x1105, NdOp::STORE, {},
           {NdVar::tmp(0, 4), NdVar::cst(0, 1)});
  if (Mutation == 3)
    emitOp(Resume, 0x1105, NdOp::CALL, {}, {NdVar::cst(0x3200, 4)});
  if (Mutation == 6)
    emitOp(Resume, 0x1105, NdOp::STORE, {},
           {NdVar::cst(0x4101, 4), NdVar::cst(0, 1)});
  emitOp(Resume, 0x1108, NdOp::INT_ADD, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(16, 4)});
  emitOp(Resume, 0x110b, NdOp::COND_BR, {},
         {NdVar::cst(0x1120, 8), NdVar::reg(x86reg::RAX, 4)});
  Resume.InstructionBoundaries = {
      {0x1105, 3, 0, uint32_t(Resume.Ops.size() - 2)},
      {0x1108, 3, uint32_t(Resume.Ops.size() - 2), 1},
      {0x110b, 4, uint32_t(Resume.Ops.size() - 1), 1}};
  Resume.InstructionBoundaries.back().Control = LowInstructionControl::Branch;
  Resume.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Branch |
      LowInstructionControlFlag::Conditional;
  auto &Jump = F.Blocks[4];
  Jump.StartAddr = 0x110f;
  Jump.EndAddr = 0x111f;
  emitOp(Jump, 0x110f, NdOp::INT_SUB, NdVar::reg(x86reg::RSP, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(8, 4)});
  emitOp(Jump, 0x1112, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-144), 4)});
  emitOp(Jump, 0x1112, NdOp::STORE, {},
         {NdVar::reg(x86reg::RSP, 4), NdVar::tmp(0, 4)});
  emitOp(Jump, 0x1115, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RSP, 4), NdVar::cst(4, 4)});
  emitOp(Jump, 0x1115, NdOp::STORE, {}, {NdVar::tmp(0, 4), NdVar::cst(0, 4)});
  emitOp(Jump, 0x111a, NdOp::CALL, {}, {NdVar::cst(0x3100, 4)});
  Jump.InstructionBoundaries = {{0x110f, 3, 0, 1},
                                {0x1112, 3, 1, 2},
                                {0x1115, 5, 3, 2},
                                {0x111a, 5, 5, 1}};
  Jump.InstructionBoundaries.back().Control = LowInstructionControl::Call;
  Jump.InstructionBoundaries.back().ControlFlags =
      LowInstructionControlFlag::Call | LowInstructionControlFlag::NoReturn;
  Jump.InstructionBoundaries.back().Immediate = 0x3100;
  auto &Retire = F.Blocks[5];
  Retire.StartAddr = 0x1120;
  Retire.EndAddr = 0x1127;
  Retire.Succs = {6};
  addSlotStore(Retire, -1);
  Retire.InstructionBoundaries = {{0x1120, 7, 0, 2}};
  F.ExceptionMetadata->Registration->TryLevelStores = {{0x1120, 0x1127, -1}};
  auto &Return = F.Blocks[6];
  Return.StartAddr = 0x1130;
  Return.EndAddr = 0x1134;
  emitOp(Return, 0x1130, NdOp::INT_ADD, NdVar::tmp(0, 4),
         {NdVar::reg(x86reg::RBP, 4), NdVar::cst(uint32_t(-16), 4)});
  emitOp(Return, 0x1130, NdOp::LOAD, NdVar::tmp(1, 4), {NdVar::tmp(0, 4)});
  emitOp(Return, 0x1130, NdOp::STORE, {}, {NdVar::cst(0, 4), NdVar::tmp(1, 4)},
         NdMemoryAddressSpace::X86FS);
  Return.InstructionBoundaries = {{0x1130, 4, 0, 3}};
  return F;
}
} // namespace neverd::registration_test
