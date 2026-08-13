//===- NdOpEmulator.cpp - Light-weight NdOp emulation -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements NdOpEmulator for computing switch-table targets by
/// executing NdOp sequences along data-flow paths.  This file holds the
/// machine state (registers, write-back memory, load log, call-preserved
/// register set) and the step/run driver; the per-opcode execution handlers
/// live in NdOpEmulatorExec.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/NdOpEmulator.h"

#include "neverd/Limits.h"

#include <algorithm>
#include <cstring>

namespace neverd {

void NdOpEmulator::reset() {
  Registers.clear();
  MemStore.clear();
  LoadLog.clear();
}

void NdOpEmulator::setRegister(uint64_t RegOff, uint64_t Value) {
  Registers[RegOff] = Value;
}

std::optional<uint64_t> NdOpEmulator::getRegister(uint64_t RegOff) const {
  auto It = Registers.find(RegOff);
  if (It != Registers.end())
    return It->second;
  return std::nullopt;
}

uint64_t NdOpEmulator::readOperand(const NdVar &Op) const {
  if (Op.isConst())
    return Op.Offset;
  if (Op.isReg() || Op.isTemp()) {
    auto It = Registers.find(Op.Offset);
    if (It != Registers.end())
      return It->second;
  }
  return 0;
}

void NdOpEmulator::writeOutput(const NdVar &Output, uint64_t Value) {
  if (!Output.isReg() && !Output.isTemp())
    return;
  uint64_t Mask = Output.Size < 8 ? (1ULL << (Output.Size * 8)) - 1 : ~0ULL;
  Registers[Output.Offset] = Value & Mask;
}

std::optional<uint64_t> NdOpEmulator::loadMemory(uint64_t Addr,
                                                 uint16_t Size) const {
  if (Size != 1 && Size != 2 && Size != 4 && Size != 8)
    return std::nullopt;

  // Check write-back store first (overlay semantics).
  auto StIt = MemStore.find(Addr);
  if (StIt != MemStore.end()) {
    uint64_t Mask = Size < 8 ? (1ULL << (Size * 8)) - 1 : ~0ULL;
    return StIt->second & Mask;
  }

  const auto *Seg = Img.getSegmentFor(Addr);
  if (!Seg || Seg->Data.empty())
    return std::nullopt;

  size_t Off = static_cast<size_t>(Addr - Seg->VA);
  if (!rangeInBounds(Off, Size, Seg->Data.size()))
    return std::nullopt;

  const uint8_t *P = Seg->Data.data() + Off;
  uint64_t Val = 0;
  switch (Size) {
  case 1:
    Val = *P;
    break;
  case 2: {
    uint16_t V;
    std::memcpy(&V, P, 2);
    Val = V;
    break;
  }
  case 4: {
    uint32_t V;
    std::memcpy(&V, P, 4);
    Val = V;
    break;
  }
  case 8:
    std::memcpy(&Val, P, 8);
    break;
  default:
    return std::nullopt;
  }
  return Val;
}

void NdOpEmulator::storeMemory(uint64_t Addr, uint16_t Size, uint64_t Value) {
  if (static_cast<int>(MemStore.size()) >= limits::kMaxEmulatorStoreEntries)
    return;
  uint64_t Mask = Size < 8 ? (1ULL << (Size * 8)) - 1 : ~0ULL;
  MemStore[Addr] = Value & Mask;
}

void NdOpEmulator::setCallPreservedRegisters(std::vector<uint64_t> Regs) {
  CallPreservedRegs = std::move(Regs);
  std::sort(CallPreservedRegs.begin(), CallPreservedRegs.end());
  StepOverCalls = true;
}

void NdOpEmulator::clobberVolatileRegisters() {
  for (auto It = Registers.begin(); It != Registers.end();) {
    // The stack pointer, frame pointer, and callee-saved registers (declared by
    // the caller) survive a call by ABI; every other register (caller-saved
    // GPRs, the return-value register, the link register, vector registers) may
    // be overwritten, so its cached value is no longer trustworthy afterward.
    bool Preserved = std::binary_search(CallPreservedRegs.begin(),
                                        CallPreservedRegs.end(), It->first);
    if (Preserved)
      ++It;
    else
      It = Registers.erase(It);
  }
}

bool NdOpEmulator::step(const LowOp &Op) {
  switch (Op.Opcode) {
  case NdOp::BRANCH:
  case NdOp::COND_BR:
  case NdOp::INDIR_BR:
  case NdOp::RETURN:
    // These change or end control flow: the single linear execution path the
    // emulator models stops here.
    return false;

  case NdOp::CALL:
  case NdOp::INDIR_CALL:
    // A call returns to the following instruction, so it does not end the
    // linear path the way a branch does.  When the caller has declared the
    // call-preserved registers, drop the values of every other (caller-saved)
    // register and continue, so a table base materialised *after* an
    // intervening call (a `bl`/`call` inside a switch block before the base
    // `lea`/`adr`) is still folded, without ever reading a stale caller-saved
    // value across the call.  Table/target soundness is still enforced by the
    // segment and validity checks at every use site.  Absent an ABI, fall back
    // to the conservative model: a call ends the emulated path.
    if (!StepOverCalls)
      return false;
    clobberVolatileRegisters();
    return true;

  case NdOp::INTRINSIC:
    // Opaque intrinsic (SSE/NEON vector op, etc.): it cannot alter control
    // flow, so linear constant-tracing must continue past it.  Its result is
    // unknown — invalidate the output register so dependents do not fold a
    // stale value; registers it does not write (e.g. a loop-invariant table
    // base in a GP register materialised by a later `lea`) are preserved.
    if (Op.Output.isReg() || Op.Output.isTemp())
      Registers.erase(Op.Output.Offset);
    return true;

  case NdOp::COPY:
  case NdOp::INT_ZEXT:
  case NdOp::INT_SEXT:
  case NdOp::SUBBYTES:
  case NdOp::INT_NEGATE:
  case NdOp::INT_NEG2:
  case NdOp::CONCAT:
    return executeCopy(Op);

  case NdOp::LOAD:
    return executeLoad(Op);

  case NdOp::INT_ADD:
  case NdOp::INT_SUB:
  case NdOp::INT_MULT:
  case NdOp::INT_DIV:
  case NdOp::INT_SDIV:
  case NdOp::INT_REM:
  case NdOp::INT_SREM:
  case NdOp::INT_AND:
  case NdOp::INT_OR:
  case NdOp::INT_XOR:
  case NdOp::INT_LEFT:
  case NdOp::INT_RIGHT:
  case NdOp::INT_ASHR:
    return executeArith(Op);

  case NdOp::INT_EQUAL:
  case NdOp::INT_NOTEQUAL:
  case NdOp::INT_LESS:
  case NdOp::INT_SLESS:
  case NdOp::INT_LESSEQUAL:
  case NdOp::INT_SLESSEQUAL:
  case NdOp::INT_CARRY:
  case NdOp::INT_SOVF:
  case NdOp::INT_SBOR:
    return executeCompare(Op);

  case NdOp::BOOL_AND:
  case NdOp::BOOL_OR:
  case NdOp::BOOL_XOR:
  case NdOp::BOOL_NOT:
    return executeBool(Op);

  case NdOp::SELECT:
  case NdOp::INT_NOT:
  case NdOp::POPCOUNT:
  case NdOp::LZCOUNT:
  case NdOp::INSERT:
  case NdOp::EXTRACT:
    return executeMisc(Op);

  case NdOp::STORE:
    return executeStore(Op);

  case NdOp::NOP:
    return true;

  default:
    return true;
  }
}

size_t NdOpEmulator::run(const std::vector<LowOp> &Ops) {
  size_t Count = 0;
  for (const auto &Op : Ops) {
    if (!step(Op))
      break;
    ++Count;
  }
  return Count;
}

std::optional<uint64_t>
NdOpEmulator::computeTarget(const std::vector<LowOp> &Ops, uint64_t IndexRegOff,
                            uint64_t IndexValue) {
  reset();
  setRegister(IndexRegOff, IndexValue);
  run(Ops);

  for (auto It = Ops.rbegin(); It != Ops.rend(); ++It) {
    if (It->Opcode == NdOp::INDIR_BR && It->NumInputs >= 1 &&
        It->Inputs[0].isReg())
      return getRegister(It->Inputs[0].Offset);
  }
  return std::nullopt;
}

void NdOpEmulator::collapseLoadRecords(std::vector<LoadRecord> &Records) {
  if (Records.size() <= 1)
    return;

  std::sort(
      Records.begin(), Records.end(),
      [](const LoadRecord &A, const LoadRecord &B) { return A.Addr < B.Addr; });

  std::vector<LoadRecord> Collapsed;
  Collapsed.push_back(Records[0]);
  uint16_t UnitSize = Records[0].Size;
  for (size_t I = 1; I < Records.size(); ++I) {
    auto &Last = Collapsed.back();
    uint64_t LastEnd = Last.Addr + Last.Size;
    if (Records[I].Addr <= LastEnd && Records[I].Size == UnitSize) {
      uint64_t NewEnd = Records[I].Addr + Records[I].Size;
      if (NewEnd > LastEnd)
        Last.Size = static_cast<uint16_t>(NewEnd - Last.Addr);
    } else {
      Collapsed.push_back(Records[I]);
      UnitSize = Records[I].Size;
    }
  }
  Records = std::move(Collapsed);
}

} // namespace neverd
