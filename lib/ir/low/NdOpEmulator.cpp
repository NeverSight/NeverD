//===- NdOpEmulator.cpp - Light-weight NdOp emulation -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements NdOpEmulator for computing switch-table targets by
/// executing NdOp sequences along data-flow paths.
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

bool NdOpEmulator::executeArith(const LowOp &Op) {
  if (Op.NumInputs < 2)
    return false;
  uint64_t A = readOperand(Op.Inputs[0]);
  uint64_t B = readOperand(Op.Inputs[1]);
  uint64_t Result = 0;

  switch (Op.Opcode) {
  case NdOp::INT_ADD:
    Result = A + B;
    break;
  case NdOp::INT_SUB:
    Result = A - B;
    break;
  case NdOp::INT_MULT:
    Result = A * B;
    break;
  case NdOp::INT_AND:
    Result = A & B;
    break;
  case NdOp::INT_OR:
    Result = A | B;
    break;
  case NdOp::INT_XOR:
    Result = A ^ B;
    break;
  case NdOp::INT_LEFT:
    Result = A << (B & 63);
    break;
  case NdOp::INT_RIGHT:
    Result = A >> (B & 63);
    break;
  case NdOp::INT_ASHR: {
    int64_t SA = static_cast<int64_t>(A);
    Result = static_cast<uint64_t>(SA >> (B & 63));
    break;
  }
  case NdOp::INT_DIV:
    if (B == 0)
      return false;
    Result = A / B;
    break;
  case NdOp::INT_SDIV:
    if (B == 0)
      return false;
    if (A == (1ULL << 63) && B == ~0ULL)
      return false;
    Result = static_cast<uint64_t>(static_cast<int64_t>(A) /
                                   static_cast<int64_t>(B));
    break;
  case NdOp::INT_REM:
    if (B == 0)
      return false;
    Result = A % B;
    break;
  case NdOp::INT_SREM:
    if (B == 0)
      return false;
    if (A == (1ULL << 63) && B == ~0ULL)
      return false;
    Result = static_cast<uint64_t>(static_cast<int64_t>(A) %
                                   static_cast<int64_t>(B));
    break;
  default:
    return false;
  }

  writeOutput(Op.Output, Result);
  return true;
}

bool NdOpEmulator::executeLoad(const LowOp &Op) {
  if (Op.NumInputs < 1)
    return false;

  uint64_t Addr;
  if (Op.NumInputs >= 2)
    Addr = readOperand(Op.Inputs[1]);
  else
    Addr = readOperand(Op.Inputs[0]);

  if (CollectLoads &&
      static_cast<int>(LoadLog.size()) < limits::kMaxLoadRecords)
    LoadLog.push_back({Addr, Op.Output.Size});

  auto Val = loadMemory(Addr, Op.Output.Size);
  if (!Val)
    return false;
  writeOutput(Op.Output, *Val);
  return true;
}

bool NdOpEmulator::executeStore(const LowOp &Op) {
  if (Op.NumInputs < 2)
    return false;

  uint64_t Addr;
  uint64_t Val;
  if (Op.NumInputs >= 3) {
    Addr = readOperand(Op.Inputs[1]);
    Val = readOperand(Op.Inputs[2]);
  } else {
    Addr = readOperand(Op.Inputs[0]);
    Val = readOperand(Op.Inputs[1]);
  }

  uint16_t Size = Op.NumInputs >= 3 ? Op.Inputs[2].Size : Op.Inputs[1].Size;
  if (Size == 0)
    Size = 8;
  if (Size != 1 && Size != 2 && Size != 4 && Size != 8)
    return false;
  storeMemory(Addr, Size, Val);
  return true;
}

bool NdOpEmulator::executeCopy(const LowOp &Op) {
  if (Op.NumInputs < 1)
    return false;
  uint64_t Val = readOperand(Op.Inputs[0]);

  switch (Op.Opcode) {
  case NdOp::COPY:
    writeOutput(Op.Output, Val);
    return true;
  case NdOp::INT_ZEXT:
    writeOutput(Op.Output, Val);
    return true;
  case NdOp::INT_SEXT: {
    if (Op.Inputs[0].Size > 0 && Op.Inputs[0].Size < 8) {
      int Bits = Op.Inputs[0].Size * 8;
      int64_t Signed = static_cast<int64_t>(Val << (64 - Bits)) >> (64 - Bits);
      writeOutput(Op.Output, static_cast<uint64_t>(Signed));
    } else {
      writeOutput(Op.Output, Val);
    }
    return true;
  }
  case NdOp::SUBBYTES: {
    uint64_t Off = Op.NumInputs >= 2 ? readOperand(Op.Inputs[1]) : 0;
    if (Off >= 8)
      return false;
    writeOutput(Op.Output, Val >> (Off * 8));
    return true;
  }
  case NdOp::INT_NEGATE:
    writeOutput(Op.Output, ~Val);
    return true;
  case NdOp::INT_NEG2:
    writeOutput(Op.Output, ~Val + 1);
    return true;
  case NdOp::CONCAT: {
    if (Op.NumInputs < 2)
      return false;
    uint64_t Hi = Val;
    uint64_t Lo = readOperand(Op.Inputs[1]);
    uint16_t LoSize = Op.Inputs[1].Size > 0 ? Op.Inputs[1].Size : 4;
    if (LoSize >= 8)
      writeOutput(Op.Output, Lo);
    else {
      uint64_t LoMask = (1ULL << (LoSize * 8)) - 1;
      writeOutput(Op.Output, (Hi << (LoSize * 8)) | (Lo & LoMask));
    }
    return true;
  }
  default:
    return false;
  }
}

bool NdOpEmulator::executeCompare(const LowOp &Op) {
  if (Op.NumInputs < 2)
    return false;
  uint64_t A = readOperand(Op.Inputs[0]);
  uint64_t B = readOperand(Op.Inputs[1]);
  uint64_t Result = 0;

  switch (Op.Opcode) {
  case NdOp::INT_EQUAL:
    Result = (A == B) ? 1 : 0;
    break;
  case NdOp::INT_NOTEQUAL:
    Result = (A != B) ? 1 : 0;
    break;
  case NdOp::INT_LESS:
    Result = (A < B) ? 1 : 0;
    break;
  case NdOp::INT_SLESS:
    Result = (static_cast<int64_t>(A) < static_cast<int64_t>(B)) ? 1 : 0;
    break;
  case NdOp::INT_LESSEQUAL:
    Result = (A <= B) ? 1 : 0;
    break;
  case NdOp::INT_SLESSEQUAL:
    Result = (static_cast<int64_t>(A) <= static_cast<int64_t>(B)) ? 1 : 0;
    break;
  case NdOp::INT_CARRY:
    Result = (A + B < A) ? 1 : 0;
    break;
  case NdOp::INT_SOVF: {
    int64_t SA = static_cast<int64_t>(A);
    int64_t SB = static_cast<int64_t>(B);
    int64_t Sum = static_cast<int64_t>(A + B);
    Result = ((SA > 0 && SB > 0 && Sum < 0) || (SA < 0 && SB < 0 && Sum >= 0))
                 ? 1
                 : 0;
    break;
  }
  case NdOp::INT_SBOR: {
    int64_t SA = static_cast<int64_t>(A);
    int64_t SB = static_cast<int64_t>(B);
    int64_t Diff = static_cast<int64_t>(A - B);
    Result =
        ((SA >= 0 && SB < 0 && Diff < 0) || (SA < 0 && SB >= 0 && Diff >= 0))
            ? 1
            : 0;
    break;
  }
  default:
    return false;
  }

  writeOutput(Op.Output, Result);
  return true;
}

bool NdOpEmulator::executeBool(const LowOp &Op) {
  if (Op.NumInputs < 1)
    return false;
  uint64_t A = readOperand(Op.Inputs[0]);
  uint64_t Result = 0;

  switch (Op.Opcode) {
  case NdOp::BOOL_NOT:
    Result = (A == 0) ? 1 : 0;
    break;
  case NdOp::BOOL_AND:
    if (Op.NumInputs < 2)
      return false;
    Result = ((A != 0) && (readOperand(Op.Inputs[1]) != 0)) ? 1 : 0;
    break;
  case NdOp::BOOL_OR:
    if (Op.NumInputs < 2)
      return false;
    Result = ((A != 0) || (readOperand(Op.Inputs[1]) != 0)) ? 1 : 0;
    break;
  case NdOp::BOOL_XOR:
    if (Op.NumInputs < 2)
      return false;
    Result = ((A != 0) != (readOperand(Op.Inputs[1]) != 0)) ? 1 : 0;
    break;
  default:
    return false;
  }

  writeOutput(Op.Output, Result);
  return true;
}

bool NdOpEmulator::executeMisc(const LowOp &Op) {
  switch (Op.Opcode) {
  case NdOp::SELECT: {
    if (Op.NumInputs < 3)
      return false;
    uint64_t Cond = readOperand(Op.Inputs[0]);
    uint64_t TrueVal = readOperand(Op.Inputs[1]);
    uint64_t FalseVal = readOperand(Op.Inputs[2]);
    writeOutput(Op.Output, Cond ? TrueVal : FalseVal);
    return true;
  }
  case NdOp::INT_NOT: {
    if (Op.NumInputs < 1)
      return false;
    writeOutput(Op.Output, ~readOperand(Op.Inputs[0]));
    return true;
  }
  case NdOp::POPCOUNT: {
    if (Op.NumInputs < 1)
      return false;
    uint64_t Val = readOperand(Op.Inputs[0]);
    writeOutput(Op.Output, __builtin_popcountll(Val));
    return true;
  }
  case NdOp::LZCOUNT: {
    if (Op.NumInputs < 1)
      return false;
    uint64_t Val = readOperand(Op.Inputs[0]);
    uint16_t ByteWidth = Op.Inputs[0].Size;
    if (ByteWidth == 0 || ByteWidth > 8)
      return false;
    uint16_t BitWidth = ByteWidth * 8;
    if (BitWidth < 64)
      Val &= (1ULL << BitWidth) - 1;
    if (Val == 0)
      writeOutput(Op.Output, BitWidth);
    else if (BitWidth == 64)
      writeOutput(Op.Output, __builtin_clzll(Val));
    else
      writeOutput(Op.Output, __builtin_clzll(Val) - (64 - BitWidth));
    return true;
  }
  case NdOp::INSERT: {
    if (Op.NumInputs < 4)
      return false;
    uint64_t Base = readOperand(Op.Inputs[0]);
    uint64_t Val = readOperand(Op.Inputs[1]);
    uint64_t Pos = readOperand(Op.Inputs[2]);
    uint64_t Len = readOperand(Op.Inputs[3]);
    if (Pos >= 64 || Len > 64 - Pos)
      return false;
    uint64_t FieldMask = Len == 64 ? ~0ULL : ((1ULL << Len) - 1);
    uint64_t Mask = FieldMask << Pos;
    writeOutput(Op.Output, (Base & ~Mask) | ((Val << Pos) & Mask));
    return true;
  }
  case NdOp::EXTRACT: {
    if (Op.NumInputs < 3)
      return false;
    uint64_t Base = readOperand(Op.Inputs[0]);
    uint64_t Pos = readOperand(Op.Inputs[1]);
    uint64_t Len = readOperand(Op.Inputs[2]);
    if (Pos >= 64 || Len > 64 - Pos)
      return false;
    uint64_t Mask = Len == 64 ? ~0ULL : ((1ULL << Len) - 1);
    writeOutput(Op.Output, (Base >> Pos) & Mask);
    return true;
  }
  default:
    return false;
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
