//===- Dataflow.cpp - Solana SBF register and scratch lattice -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/Dataflow.h"

#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/Semantics.h"
#include "neverd/sbf/Syscalls.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <bit>
#include <deque>
#include <iterator>
#include <utility>

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

  const bool IsAddress = Value.ValueKind == RegisterValue::Kind::StackAddress ||
                         Value.ValueKind == RegisterValue::Kind::RodataAddress;
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

/// True when a value could be a pointer into this program's own scratch.
bool mayAddressScratch(const RegisterValue &Value) {
  if (Value.ValueKind == RegisterValue::Kind::Unknown)
    return true;
  const std::optional<va_t> Address = effectiveAddress(Value, 0);
  return Address && isScratchAddress(*Address);
}

/// True when the callee of \p Instruction could write this frame or this heap.
///
/// A callee runs in a frame of its own and can only reach its caller's scratch
/// through an address it is given, so a call whose arguments are all proven not
/// to be scratch addresses leaves everything proven before it still proven.
bool calleeCanReachScratch(const MachineState &State) {
  if (State.Scratch.Escaped)
    return true;
  for (unsigned Ordinal = 0; Ordinal < kArgumentRegisterCount; ++Ordinal)
    if (mayAddressScratch(State.Registers[kFirstArgumentRegister + Ordinal]))
      return true;
  return false;
}

void applyStore(const MedInstruction &Instruction, MachineState &State) {
  const std::optional<va_t> Address =
      effectiveAddress(State.Registers[Instruction.Dst], Instruction.Offset);
  const RegisterValue Value = storedValue(Instruction, State.Registers);
  const unsigned ByteWidth = Instruction.Width / kBitsPerByte;

  // A store whose address is unknown could land on any byte the model holds,
  // and it could publish a scratch address anywhere a callee can read it.
  if (!Address) {
    State.Scratch.Memory.clear();
    State.Scratch.Escaped |= mayAddressScratch(Value);
    return;
  }
  if (!isScratchAddress(*Address)) {
    // Writing a scratch address outside scratch puts it somewhere the register
    // file no longer shows, so a later callee could load it back.
    State.Scratch.Escaped |= mayAddressScratch(Value);
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

  if (const std::optional<uint64_t> Bytes = Window.fixedBytes()) {
    State.Scratch.Memory.invalidate(*Base, *Bytes);
    return;
  }
  if (const std::optional<SyscallArgument> Length = Window.lengthArgument())
    if (const std::optional<uint64_t> Bytes = constantArgument(State, *Length)) {
      State.Scratch.Memory.invalidate(*Base, *Bytes);
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
void applyMemoryTransfer(const MedInstruction &Instruction, MachineState &State,
                         const ProgramImage &Image) {
  const Syscall Which = Instruction.Syscall->ID;
  if (Which != Syscall::Memcpy && Which != Syscall::Memmove &&
      Which != Syscall::Memset)
    return;

  const std::optional<va_t> Destination = effectiveAddress(
      State.Registers[argumentRegister(SyscallArgument::Arg1)], 0);
  const std::optional<uint64_t> Length =
      constantArgument(State, SyscallArgument::Arg3);
  if (!Destination || !isScratchAddress(*Destination) || !Length ||
      *Length == 0 || *Length > kMaxModeledScratchBytes)
    return;

  if (Which == Syscall::Memset) {
    const std::optional<uint64_t> Fill =
        constantArgument(State, SyscallArgument::Arg2);
    if (!Fill)
      return;
    const llvm::SmallVector<uint8_t> Bytes(*Length,
                                           static_cast<uint8_t>(*Fill));
    State.Scratch.Memory.write(*Destination, Bytes);
    return;
  }

  const std::optional<va_t> Source = effectiveAddress(
      State.Registers[argumentRegister(SyscallArgument::Arg2)], 0);
  if (!Source)
    return;
  const llvm::ArrayRef<uint8_t> Bytes =
      readProven(State, Image, *Source, *Length);
  if (Bytes.empty())
    return;
  // Copy out first: the source may be the model itself, and writing into it
  // can move the run the view points at.
  const llvm::SmallVector<uint8_t> Copied(Bytes.begin(), Bytes.end());
  State.Scratch.Memory.write(*Destination, Copied);
}

void applyCall(const MedInstruction &Instruction, MachineState &State,
               const ProgramImage &Image) {
  if (Instruction.Call != CallKind::Syscall || !Instruction.Syscall) {
    // Anything other than a resolved syscall is a function this analysis has
    // not described, so it keeps only what such a function cannot reach.
    if (calleeCanReachScratch(State))
      State.Scratch.Memory.clear();
    return;
  }

  // A syscall with no write window is exactly one that preservesCallerMemory
  // reports as harmless, and this loop leaves the model untouched for it.
  for (const SyscallMemoryInfo &Window :
       getSyscallMemory(Instruction.Syscall->ID))
    if (Window.Access == SyscallMemoryAccess::Write)
      applySyscallWindow(Window, State);
  applyMemoryTransfer(Instruction, State, Image);
}

bool isIntraFunctionEdge(EdgeKind Kind) {
  switch (Kind) {
  case EdgeKind::Fallthrough:
  case EdgeKind::BranchTaken:
  case EdgeKind::Branch:
    return true;
  case EdgeKind::Call:
  case EdgeKind::IndirectCall:
  case EdgeKind::Return:
  case EdgeKind::Invalid:
    return false;
  }
  return false;
}

} // namespace

std::optional<va_t> effectiveAddress(const RegisterValue &Base,
                                     int64_t Displacement) {
  switch (Base.ValueKind) {
  case RegisterValue::Kind::Unknown:
    return std::nullopt;
  case RegisterValue::Kind::Constant:
    return Base.Value + static_cast<uint64_t>(Displacement);
  case RegisterValue::Kind::StackAddress:
  case RegisterValue::Kind::RodataAddress:
    return Base.Value + static_cast<uint64_t>(Base.Offset) +
           static_cast<uint64_t>(Displacement);
  }
  return std::nullopt;
}

bool isScratchAddress(va_t Address) {
  const bool OnStack =
      Address >= kStackStart && Address < kStackStart + kMemoryRegionSize;
  const bool OnHeap =
      Address >= kHeapStart && Address < kHeapStart + kMemoryRegionSize;
  return OnStack || OnHeap;
}

//===----------------------------------------------------------------------===//
// MemoryModel
//===----------------------------------------------------------------------===//

void MemoryModel::clear() {
  Runs.clear();
  TrackedBytes = 0;
}

void MemoryModel::invalidate(va_t Address, uint64_t Size) {
  if (Size == 0 || Runs.empty())
    return;
  const va_t End = Address + Size;

  auto It = Runs.upper_bound(Address);
  if (It != Runs.begin())
    --It;
  while (It != Runs.end() && It->first < End) {
    const va_t Start = It->first;
    const uint64_t Length = It->second.size();
    if (Start + Length <= Address) {
      ++It;
      continue;
    }

    std::vector<uint8_t> Head;
    if (Start < Address)
      Head.assign(It->second.begin(),
                  It->second.begin() + static_cast<ptrdiff_t>(Address - Start));
    std::vector<uint8_t> Tail;
    if (Start + Length > End)
      Tail.assign(It->second.begin() + static_cast<ptrdiff_t>(End - Start),
                  It->second.end());

    TrackedBytes -= Length;
    It = Runs.erase(It);
    if (!Head.empty()) {
      TrackedBytes += Head.size();
      Runs.emplace(Start, std::move(Head));
    }
    if (!Tail.empty()) {
      TrackedBytes += Tail.size();
      Runs.emplace(End, std::move(Tail));
    }
  }
}

void MemoryModel::invalidateFrom(va_t Address) {
  // A buffer never crosses a VM region boundary, so a write of unproven length
  // starting here reaches no further than the end of its own region.
  const va_t RegionEnd = (Address / kMemoryRegionSize + 1) * kMemoryRegionSize;
  invalidate(Address, RegionEnd - Address);
}

void MemoryModel::write(va_t Address, llvm::ArrayRef<uint8_t> Bytes) {
  if (Bytes.empty())
    return;
  invalidate(Address, Bytes.size());
  // Past the budget the model stops describing new bytes rather than dropping
  // what it already proved, so a store loop degrades into silence.
  if (TrackedBytes + Bytes.size() > kMaxModeledScratchBytes)
    return;

  std::vector<uint8_t> Run(Bytes.begin(), Bytes.end());
  va_t Start = Address;

  auto After = Runs.upper_bound(Address);
  if (After != Runs.begin()) {
    auto Before = std::prev(After);
    if (Before->first + Before->second.size() == Address) {
      Start = Before->first;
      std::vector<uint8_t> Merged = std::move(Before->second);
      Merged.insert(Merged.end(), Run.begin(), Run.end());
      Run = std::move(Merged);
      Runs.erase(Before);
    }
  }
  if (auto Next = Runs.find(Address + Bytes.size()); Next != Runs.end()) {
    Run.insert(Run.end(), Next->second.begin(), Next->second.end());
    Runs.erase(Next);
  }

  TrackedBytes += Bytes.size();
  Runs.insert_or_assign(Start, std::move(Run));
}

llvm::ArrayRef<uint8_t> MemoryModel::read(va_t Address, uint64_t Size) const {
  if (Size == 0 || Runs.empty())
    return {};
  auto It = Runs.upper_bound(Address);
  if (It == Runs.begin())
    return {};
  --It;
  const uint64_t Offset = Address - It->first;
  const std::vector<uint8_t> &Bytes = It->second;
  if (Offset > Bytes.size() || Size > Bytes.size() - Offset)
    return {};
  return llvm::ArrayRef(Bytes).slice(Offset, Size);
}

std::optional<uint64_t> MemoryModel::readWord(va_t Address) const {
  const llvm::ArrayRef<uint8_t> Bytes = read(Address, sizeof(uint64_t));
  if (Bytes.empty())
    return std::nullopt;
  return llvm::support::endian::read64le(Bytes.data());
}

void MemoryModel::meet(const MemoryModel &Other) {
  MemoryModel Agreed;
  for (const auto &[Start, Bytes] : Runs) {
    std::vector<uint8_t> Span;
    va_t SpanStart = Start;
    for (size_t Index = 0; Index < Bytes.size(); ++Index) {
      const llvm::ArrayRef<uint8_t> Byte = Other.read(Start + Index, 1);
      if (!Byte.empty() && Byte.front() == Bytes[Index]) {
        if (Span.empty())
          SpanStart = Start + Index;
        Span.push_back(Bytes[Index]);
        continue;
      }
      if (!Span.empty()) {
        Agreed.write(SpanStart, Span);
        Span.clear();
      }
    }
    if (!Span.empty())
      Agreed.write(SpanStart, Span);
  }
  *this = std::move(Agreed);
}

bool MemoryModel::operator==(const MemoryModel &Other) const {
  return Runs == Other.Runs;
}

void ScratchState::meet(const ScratchState &Other) {
  Memory.meet(Other.Memory);
  Escaped = Escaped || Other.Escaped;
}

bool ScratchState::operator==(const ScratchState &Other) const {
  return Escaped == Other.Escaped && Memory == Other.Memory;
}

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

//===----------------------------------------------------------------------===//
// ScratchFlow
//===----------------------------------------------------------------------===//

ScratchFlow::ScratchFlow(const SBFProgram &Program,
                         const MedInstructionIndex &Index) {
  const size_t Count =
      std::min(Program.Med.Blocks.size(), Program.Low.Blocks.size());
  Entry.resize(Count);
  if (Count == 0 || Count > kMaxScratchFlowBlocks)
    return;

  std::vector<llvm::SmallVector<size_t, 2>> Predecessors(Count);
  std::vector<llvm::SmallVector<size_t, 2>> Successors(Count);
  for (const CFGEdge &Edge : Program.Low.Edges) {
    if (!Edge.To || *Edge.To >= Count || Edge.From >= Count ||
        !isIntraFunctionEdge(Edge.Kind))
      continue;
    Predecessors[*Edge.To].push_back(Edge.From);
    Successors[Edge.From].push_back(*Edge.To);
  }

  const auto Replay = [&](size_t ID, const ScratchState &In) {
    MachineState State;
    State.Registers = Program.Med.Blocks[ID].Inputs;
    State.Scratch = In;
    const MedBlock &Block = Program.Med.Blocks[ID];
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      if (const MedInstruction *Instruction = Index.find(Slot))
        applyTransfer(*Instruction, State, Program.ExecutableImage);
    return std::move(State.Scratch);
  };

  std::vector<ScratchState> Exit(Count);
  std::vector<char> Computed(Count, 0);
  std::vector<char> Queued(Count, 0);
  std::deque<size_t> Worklist;
  for (size_t ID = 0; ID < Count; ++ID)
    if (Predecessors[ID].empty()) {
      Worklist.push_back(ID);
      Queued[ID] = 1;
    }

  // Each recomputation can only drop facts, so the analysis settles on its
  // own; the cap only keeps a malformed CFG from costing unbounded time.
  const size_t StepLimit = Count * 8 + 64;
  for (size_t Step = 0; Step < StepLimit && !Worklist.empty(); ++Step) {
    const size_t ID = Worklist.front();
    Worklist.pop_front();
    Queued[ID] = 0;

    ScratchState Merged;
    bool Any = false;
    for (size_t Predecessor : Predecessors[ID]) {
      if (!Computed[Predecessor])
        continue;
      if (!Any) {
        Merged = Exit[Predecessor];
        Any = true;
        continue;
      }
      Merged.meet(Exit[Predecessor]);
    }
    // A block whose predecessors are all still unknown waits for one of them
    // rather than claiming the empty state as a fact about this path.
    if (!Any && !Predecessors[ID].empty())
      continue;

    if (Computed[ID] && Merged == Entry[ID])
      continue;
    Entry[ID] = std::move(Merged);
    Exit[ID] = Replay(ID, Entry[ID]);
    Computed[ID] = 1;

    for (size_t Successor : Successors[ID]) {
      if (Queued[Successor])
        continue;
      Worklist.push_back(Successor);
      Queued[Successor] = 1;
    }
  }
}

const ScratchState &ScratchFlow::entryState(size_t BlockID) const {
  return BlockID < Entry.size() ? Entry[BlockID] : Unreached;
}

void replayBlock(
    const MedInstructionIndex &Index, const MedBlock &Block,
    const ScratchState &Entry, const ProgramImage &Image,
    llvm::function_ref<void(const MedInstruction &, const MachineState &)>
        Visit) {
  MachineState State;
  State.Registers = Block.Inputs;
  State.Scratch = Entry;
  for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
    const MedInstruction *Instruction = Index.find(Slot);
    if (!Instruction)
      continue;
    Visit(*Instruction, State);
    applyTransfer(*Instruction, State, Image);
  }
}

} // namespace neverd::sbf
