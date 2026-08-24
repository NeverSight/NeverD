//===- SBFDataflowTransfer.cpp - Solana SBF transfer functions ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// What one instruction does to the tracked machine state: the constant and
/// address arithmetic a register write performs, the bytes a store leaves in
/// scratch, and what a call or a syscall's declared write windows invalidate.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/analysis/SBFDataflow.h"
#include "neverd/sbf/runtime/SBFSemantics.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::sbf {
namespace {

uint64_t normalizeResult(const MedInstruction &Instruction, uint64_t Value) {
  if (Instruction.Width == kWordBitWidth)
    return extendALU32Result(static_cast<uint32_t>(Value),
                             Instruction.Semantics.Result);
  return Value;
}

uint64_t instructionImmediate(const MedInstruction &Instruction) {
  return normalizeImmediate(Instruction.Immediate,
                            Instruction.Semantics.Immediate);
}

void applyAddSubImmediate(const MedInstruction &Instruction,
                          RegisterValue &Value) {
  const uint64_t Immediate = instructionImmediate(Instruction);
  if (Value.ValueKind == RegisterValue::Kind::Constant) {
    const uint64_t Result =
        Instruction.Op == Operation::Add     ? Value.Value + Immediate
        : Instruction.Semantics.SwapOperands ? Immediate - Value.Value
                                             : Value.Value - Immediate;
    Value.Value = normalizeResult(Instruction, Result);
    return;
  }

  const bool IsAddress =
      Value.ValueKind == RegisterValue::Kind::StackAddress ||
      Value.ValueKind == RegisterValue::Kind::RodataAddress ||
      Value.ValueKind == RegisterValue::Kind::InstructionDataAddress;
  if (!IsAddress || Instruction.Width != kDoubleWordBitWidth ||
      Instruction.Semantics.SwapOperands) {
    Value = {};
    return;
  }

  const int64_t Delta = std::bit_cast<int64_t>(Immediate);
  int64_t NewOffset = 0;
  const bool Overflow = Instruction.Op == Operation::Add
                            ? llvm::AddOverflow(Value.Offset, Delta, NewOffset)
                            : llvm::SubOverflow(Value.Offset, Delta, NewOffset);
  if (Overflow)
    Value = {};
  else
    Value.Offset = NewOffset;
}

/// The value a store writes, which is a register for the two-register form and
/// the sign-extended immediate otherwise.
RegisterValue storedValue(const MedInstruction &Instruction,
                          const RegisterState &Registers) {
  if (Instruction.Form == OperandForm::StoreReg)
    return Registers[Instruction.Src];
  return {RegisterValue::Kind::Constant, instructionImmediate(Instruction), 0};
}

/// The little-endian bytes a store of \p Value at \p ByteWidth leaves behind,
/// when the value denotes a number this analysis knows. An address counts: its
/// bytes are the address itself, which is how a pointer written into a
/// descriptor becomes readable at the call that follows the descriptor.
std::optional<llvm::SmallVector<uint8_t, 8>>
storedBytes(const RegisterValue &Value, unsigned ByteWidth) {
  if (ByteWidth == 0 || ByteWidth > sizeof(uint64_t))
    return std::nullopt;
  const std::optional<va_t> Number = effectiveAddress(Value, 0);
  if (!Number)
    return std::nullopt;
  llvm::SmallVector<uint8_t, 8> Bytes(sizeof(uint64_t));
  llvm::support::endian::write64le(Bytes.data(), *Number);
  Bytes.resize(ByteWidth);
  return Bytes;
}

void applyStore(const MedInstruction &Instruction, MachineState &State) {
  const std::optional<va_t> Address =
      effectiveAddress(State.Registers[Instruction.Dst], Instruction.Offset);
  const RegisterValue Value = storedValue(Instruction, State.Registers);
  const unsigned ByteWidth = Instruction.Width / kBitsPerByte;

  // A store whose address is unknown could land on any byte the model holds.
  if (!Address) {
    State.Scratch.Memory.clear();
    return;
  }
  if (!isScratchAddress(*Address))
    return;
  if (!isScratchRange(*Address, ByteWidth)) {
    // The runtime validates the whole access before writing. Execution cannot
    // continue after this fault, so retaining prior scratch facts would let a
    // later recovery visitor make claims about an unreachable state.
    State.Scratch.Memory.clear();
    return;
  }

  if (const std::optional<llvm::SmallVector<uint8_t, 8>> Bytes =
          storedBytes(Value, ByteWidth))
    State.Scratch.Memory.write(*Address, *Bytes);
  else
    State.Scratch.Memory.invalidate(*Address, ByteWidth);
}

/// The bytes at \p Address that either the program's own scratch or the loaded
/// image proves, empty when any of them is unproven.
llvm::ArrayRef<uint8_t> readProven(const MachineState &State,
                                   const ProgramImage &Image, va_t Address,
                                   uint64_t Size) {
  if (Size == 0)
    return {};
  if (const llvm::ArrayRef<uint8_t> Scratch =
          State.Scratch.Memory.read(Address, Size);
      !Scratch.empty())
    return Scratch;
  llvm::Expected<llvm::ArrayRef<uint8_t>> Mapped =
      Image.slice(Address, Size, /*DataAccess=*/true);
  if (!Mapped) {
    llvm::consumeError(Mapped.takeError());
    return {};
  }
  return *Mapped;
}

std::optional<uint64_t> constantArgument(const MachineState &State,
                                         SyscallArgument Argument) {
  const RegisterValue &Value = State.Registers[argumentRegister(Argument)];
  if (Value.ValueKind != RegisterValue::Kind::Constant)
    return std::nullopt;
  return Value.Value;
}

/// Forget everything one declared window of a syscall can overwrite.
void applySyscallWindow(const SyscallMemoryInfo &Window, MachineState &State) {
  const std::optional<va_t> Base =
      effectiveAddress(State.Registers[argumentRegister(Window.Argument)], 0);
  // A write through an address this analysis cannot name could land anywhere.
  if (!Base) {
    State.Scratch.Memory.clear();
    return;
  }
  if (!isScratchAddress(*Base))
    return;

  const auto InvalidateKnownRange = [&](uint64_t Bytes) {
    if (!isScratchRange(*Base, Bytes)) {
      // Runtime memory validation is atomic for one declared window. A known
      // range that crosses a VM region cannot return, so no scratch fact is
      // reachable after the call.
      State.Scratch.Memory.clear();
      return;
    }
    State.Scratch.Memory.invalidate(*Base, Bytes);
  };

  if (const std::optional<uint64_t> Bytes = Window.fixedBytes()) {
    InvalidateKnownRange(*Bytes);
    return;
  }
  if (const std::optional<SyscallArgument> Length = Window.lengthArgument())
    if (const std::optional<uint64_t> Bytes =
            constantArgument(State, *Length)) {
      InvalidateKnownRange(*Bytes);
      return;
    }
  State.Scratch.Memory.invalidateFrom(*Base);
}

/// Record what the byte-moving syscalls leave behind.
///
/// They are the only syscalls whose result is bytes the caller chose, so they
/// are the only ones whose output this analysis can name. A program that
/// assembles an invocation payload gets it into place with one of these, and
/// without following them the payload at the invocation reads as unproven.
struct ModeledMemoryTransfer {
  va_t Destination = 0;
  llvm::SmallVector<uint8_t> Bytes;
};

/// Snapshot a byte-moving syscall's result from its pre-call state.
///
/// In particular, memmove permits overlapping source and destination ranges.
/// The generic syscall write-window invalidation therefore cannot run before
/// this read: doing so would erase part of the source and lose a fact that the
/// runtime preserves by definition.
std::optional<ModeledMemoryTransfer>
modelMemoryTransfer(const MedInstruction &Instruction,
                    const MachineState &State, const ProgramImage &Image) {
  const Syscall Which = Instruction.Syscall->ID;
  if (Which != Syscall::Memcpy && Which != Syscall::Memmove &&
      Which != Syscall::Memset)
    return std::nullopt;

  const std::optional<va_t> Destination = effectiveAddress(
      State.Registers[argumentRegister(SyscallArgument::Arg1)], 0);
  const std::optional<uint64_t> Length =
      constantArgument(State, SyscallArgument::Arg3);
  if (!Destination || !Length || *Length == 0 ||
      *Length > kMaxModeledScratchBytes ||
      !isScratchRange(*Destination, *Length))
    return std::nullopt;

  if (Which == Syscall::Memset) {
    const std::optional<uint64_t> Fill =
        constantArgument(State, SyscallArgument::Arg2);
    if (!Fill)
      return std::nullopt;
    return ModeledMemoryTransfer{
        *Destination,
        llvm::SmallVector<uint8_t>(*Length, static_cast<uint8_t>(*Fill))};
  }

  const std::optional<va_t> Source = effectiveAddress(
      State.Registers[argumentRegister(SyscallArgument::Arg2)], 0);
  if (!Source)
    return std::nullopt;
  const llvm::ArrayRef<uint8_t> Bytes =
      readProven(State, Image, *Source, *Length);
  if (Bytes.empty())
    return std::nullopt;
  return ModeledMemoryTransfer{
      *Destination, llvm::SmallVector<uint8_t>(Bytes.begin(), Bytes.end())};
}

void applyCall(const MedInstruction &Instruction, MachineState &State,
               const ProgramImage &Image) {
  if (Instruction.Call != CallKind::Syscall || !Instruction.Syscall) {
    // Anything other than a resolved syscall is a function this analysis has
    // no proven mod/ref summary for. SBF heap addresses are process-global and
    // stack addresses can be reconstructed, so argument registers alone can
    // never prove that the callee cannot reach a tracked byte.
    State.Scratch.Memory.clear();
    return;
  }

  const std::optional<ModeledMemoryTransfer> Transfer =
      modelMemoryTransfer(Instruction, State, Image);
  // A syscall with no write window is exactly one that preservesCallerMemory
  // reports as harmless, and this loop leaves the model untouched for it.
  for (const SyscallMemoryInfo &Window :
       getSyscallMemory(Instruction.Syscall->ID))
    if (Window.Access == SyscallMemoryAccess::Write)
      applySyscallWindow(Window, State);
  if (Transfer)
    State.Scratch.Memory.write(Transfer->Destination, Transfer->Bytes);
}

} // namespace

//===----------------------------------------------------------------------===//
// Transfer
//===----------------------------------------------------------------------===//
MedInstructionIndex::MedInstructionIndex(const MedIR &IR) {
  size_t Slots = 0;
  for (const MedInstruction &Instruction : IR.Instructions)
    Slots = std::max(Slots, Instruction.Slot + 1);
  BySlot.assign(Slots, nullptr);
  for (const MedInstruction &Instruction : IR.Instructions)
    BySlot[Instruction.Slot] = &Instruction;
}

const MedInstruction *MedInstructionIndex::find(size_t Slot) const {
  return Slot < BySlot.size() ? BySlot[Slot] : nullptr;
}

bool consumesScratchFacts(const MedInstruction &Instruction) {
  if (!Instruction.Syscall ||
      !dispatchesRuntimeSyscall(Instruction.Call, Instruction.Dispatch))
    return false;
  return hasEffect(Instruction.Syscall->Effects, SyscallEffect::CPI) ||
         Instruction.Syscall->Category == SyscallCategory::PDA;
}

void applyRegisterTransfer(const MedInstruction &Instruction,
                           RegisterState &State) {
  auto SetConstant = [&](uint64_t Value) {
    State[Instruction.Dst] = {RegisterValue::Kind::Constant, Value, 0};
  };

  if (Instruction.Op == Operation::LoadImm) {
    SetConstant(
        normalizeResult(Instruction, instructionImmediate(Instruction)));
  } else if (Instruction.Op == Operation::Mov) {
    if (Instruction.Form == OperandForm::DstImm) {
      SetConstant(
          normalizeResult(Instruction, instructionImmediate(Instruction)));
    } else {
      const RegisterValue Source = State[Instruction.Src];
      if (Source.ValueKind == RegisterValue::Kind::Constant)
        SetConstant(normalizeResult(Instruction, Source.Value));
      else if (Instruction.Width == kDoubleWordBitWidth)
        State[Instruction.Dst] = Source;
      else
        State[Instruction.Dst] = {};
    }
  } else if ((Instruction.Op == Operation::Add ||
              Instruction.Op == Operation::Sub) &&
             Instruction.Form == OperandForm::DstImm) {
    applyAddSubImmediate(Instruction, State[Instruction.Dst]);
  } else if (Instruction.Op == Operation::HighOr &&
             State[Instruction.Dst].ValueKind ==
                 RegisterValue::Kind::Constant) {
    State[Instruction.Dst].Value |=
        static_cast<uint64_t>(static_cast<uint32_t>(Instruction.Immediate))
        << kWordBitWidth;
  } else if (Instruction.Semantics.WritesDestination) {
    State[Instruction.Dst] = {};
  }

  if (Instruction.Call != CallKind::None)
    for (unsigned Register = 0; Register < kFirstCalleeSavedRegister;
         ++Register)
      State[Register] = {};
}

void applyTransfer(const MedInstruction &Instruction, MachineState &State,
                   const ProgramImage &Image) {
  if (Instruction.Op == Operation::Store)
    applyStore(Instruction, State);
  if (Instruction.Call != CallKind::None)
    applyCall(Instruction, State, Image);
  applyRegisterTransfer(Instruction, State.Registers);
}

} // namespace neverd::sbf
