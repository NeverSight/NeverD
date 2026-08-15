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
#include <utility>

namespace neverd {

namespace {

bool isInstructionControl(NdOp Opcode) {
  switch (Opcode) {
  case NdOp::BRANCH:
  case NdOp::INDIR_BR:
  case NdOp::CALL:
  case NdOp::INDIR_CALL:
  case NdOp::RETURN:
    return true;
  default:
    return false;
  }
}

const LowInstructionBoundary *
boundaryForOp(llvm::ArrayRef<LowOp> Ops,
              llvm::ArrayRef<LowInstructionBoundary> Boundaries,
              size_t OpIndex) {
  auto It = std::upper_bound(
      Boundaries.begin(), Boundaries.end(), OpIndex,
      [](size_t Index, const LowInstructionBoundary &Boundary) {
        return Index < Boundary.FirstOp;
      });
  if (It == Boundaries.begin())
    return nullptr;
  --It;
  const uint64_t First = It->FirstOp;
  if (First > Ops.size() || It->OpCount > Ops.size() - First)
    return nullptr;
  const uint64_t End = First + It->OpCount;
  return OpIndex >= First && OpIndex < End ? &*It : nullptr;
}

/// End of the instruction containing an ARM predicate guard, or zero when
/// this is an ordinary block-level conditional branch.  A LowBlock's boundary
/// metadata uses InstructionGuard authoritatively after structural validation.
/// The legacy vector API retains the conservative control-only same-address
/// rule.
size_t
predicatedInstructionEnd(llvm::ArrayRef<LowOp> Ops,
                         llvm::ArrayRef<LowInstructionBoundary> Boundaries,
                         size_t Guard) {
  if (Guard >= Ops.size() || Ops[Guard].Opcode != NdOp::COND_BR)
    return 0;

  size_t End = 0;
  if (!Boundaries.empty()) {
    const LowInstructionBoundary *Boundary =
        boundaryForOp(Ops, Boundaries, Guard);
    if (!Boundary || (Boundary->Mode != InstructionMode::ARM &&
                      Boundary->Mode != InstructionMode::Thumb))
      return 0;
    const bool IsTaggedGuard = hasLowInstructionControlFlag(
        Boundary->ControlFlags, LowInstructionControlFlag::InstructionGuard);
    if (!IsTaggedGuard)
      return 0;
    End = static_cast<size_t>(Boundary->FirstOp + Boundary->OpCount);
    return Guard + 1 < End ? End : 0;
  } else {
    if (Ops[Guard].Addr == 0)
      return 0;
    End = Guard + 1;
    while (End < Ops.size() && Ops[End].Addr == Ops[Guard].Addr)
      ++End;
  }

  for (size_t I = Guard + 1; I < End; ++I)
    if (isInstructionControl(Ops[I].Opcode))
      return End;
  return 0;
}

} // namespace

void NdOpEmulator::reset() {
  Registers.clear();
  MemStore.clear();
  LoadLog.clear();
  ReachedIndirectBranchTarget.reset();
  ReachedSourceMode.reset();
  ReachedTargetMode.reset();
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

bool NdOpEmulator::storeMemory(uint64_t Addr, uint16_t Size, uint64_t Value) {
  if (static_cast<int>(MemStore.size()) >= limits::kMaxEmulatorStoreEntries) {
    // A dropped store leaves whatever was underneath it visible to the next
    // load of the same address, so it is not a missing write but a wrong read
    // waiting to happen.  The bound stays; what changes is that a caller can
    // find out it was reached.
    ++Skips.DroppedStores;
    return false;
  }
  uint64_t Mask = Size < 8 ? (1ULL << (Size * 8)) - 1 : ~0ULL;
  MemStore[Addr] = Value & Mask;
  return true;
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
  case NdOp::RETURN:
    // These change or end control flow: the single linear execution path the
    // emulator models stops here.
    return false;

  case NdOp::INDIR_BR:
    ReachedIndirectBranchTarget.reset();
    ReachedSourceMode.reset();
    ReachedTargetMode.reset();
    if (Op.NumInputs >= 1)
      ReachedIndirectBranchTarget = Op.Inputs[0];
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
    ++Skips.ApproximatedOps;
    if (Op.Output.isReg() || Op.Output.isTemp())
      Registers.erase(Op.Output.Offset);
    return !StrictMode;

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

  case NdOp::ATOMIC_XCHG:
    // The constant-tracing emulator stores one host word per value and cannot
    // represent the 128-bit exchanges this opcode exists for.  Stop instead
    // of carrying stale memory or destination state past an observable RMW.
    ++Skips.UnsupportedOps;
    if (Op.Output.isReg() || Op.Output.isTemp())
      Registers.erase(Op.Output.Offset);
    return false;

  case NdOp::NOP:
    return true;

  default:
    // An opcode with no model here — floating point, a cast the lifters route
    // through no other operation.  The path continues by default because a
    // switch dispatch is recovered from a constant that every use site
    // re-validates against the image, so carrying on past what does not feed
    // the address costs nothing and stopping loses tables.  Counted either
    // way, so a caller can tell a folded constant from one that stepped over
    // something on its way here.
    ++Skips.UnsupportedOps;
    return !StrictMode;
  }
}

size_t NdOpEmulator::run(const std::vector<LowOp> &Ops) {
  return runImpl(Ops, {});
}

size_t NdOpEmulator::run(const LowBlock &Block) {
  if (!Block.InstructionBoundaries.empty()) {
    if (llvm::Error Error = validateLowInstructionBoundaries(
            Block, LowInstructionBoundaryRequirement::Required)) {
      llvm::consumeError(std::move(Error));
      return 0;
    }
  }
  return runImpl(Block.Ops, Block.InstructionBoundaries);
}

size_t
NdOpEmulator::runImpl(llvm::ArrayRef<LowOp> Ops,
                      llvm::ArrayRef<LowInstructionBoundary> Boundaries) {
  size_t Count = 0;
  for (size_t I = 0; I < Ops.size();) {
    const LowOp &Op = Ops[I];
    if (const size_t End = predicatedInstructionEnd(Ops, Boundaries, I)) {
      if (Op.NumInputs < 2)
        break;
      const NdVar &Predicate = Op.Inputs[1];
      std::optional<uint64_t> PredicateValue;
      if (Predicate.isConst())
        PredicateValue = Predicate.Offset;
      else if (Predicate.isReg() || Predicate.isTemp())
        PredicateValue = getRegister(Predicate.Offset);
      if (!PredicateValue)
        break;
      // ARM's guard targets the next instruction when !predicate holds.  A
      // concrete true guard therefore skips every remaining effect in this
      // instruction; a false guard executes them in order.  Neither choice is
      // a block terminator by itself.
      if (*PredicateValue != 0) {
        I = End;
        continue;
      }
      ++I;
      continue;
    }
    if (!step(Op)) {
      if (Op.Opcode == NdOp::INDIR_BR) {
        if (const LowInstructionBoundary *Boundary =
                boundaryForOp(Ops, Boundaries, I)) {
          ReachedSourceMode = Boundary->Mode;
          ReachedTargetMode = Boundary->TargetMode;
        }
      }
      break;
    }
    ++Count;
    ++I;
  }
  return Count;
}

std::optional<uint64_t> NdOpEmulator::reachedIndirectTarget() const {
  if (!ReachedIndirectBranchTarget)
    return std::nullopt;
  if (ReachedIndirectBranchTarget->isConst())
    return ReachedIndirectBranchTarget->Offset;
  if (ReachedIndirectBranchTarget->isReg() ||
      ReachedIndirectBranchTarget->isTemp())
    return getRegister(ReachedIndirectBranchTarget->Offset);
  return std::nullopt;
}

std::optional<uint64_t>
NdOpEmulator::computeTarget(const std::vector<LowOp> &Ops, uint64_t IndexRegOff,
                            uint64_t IndexValue) {
  reset();
  setRegister(IndexRegOff, IndexValue);
  run(Ops);
  return reachedIndirectTarget();
}

std::optional<LowControlTarget>
NdOpEmulator::computeTarget(const LowBlock &Block, uint64_t IndexRegOff,
                            uint64_t IndexValue) {
  reset();
  setRegister(IndexRegOff, IndexValue);
  run(Block);

  const std::optional<uint64_t> RawTarget = reachedIndirectTarget();
  if (!RawTarget || !ReachedSourceMode || !ReachedTargetMode)
    return std::nullopt;
  llvm::Expected<LowControlTarget> Target = canonicalizeLowControlTarget(
      *RawTarget, *ReachedSourceMode, *ReachedTargetMode);
  if (!Target) {
    llvm::consumeError(Target.takeError());
    return std::nullopt;
  }
  return *Target;
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
