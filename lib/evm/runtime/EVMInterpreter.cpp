//===- EVMInterpreter.cpp - Deterministic EVM semantic oracle -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/runtime/EVMInterpreter.h"

#include "../analysis/EVMMedIRVerifier.h"
#include "EVMInterpreterDetail.h"
#include "EVMKeccak.h"

#include "neverd/evm/runtime/EVMSemantics.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace neverd::evm {

using detail::boolWord;
using detail::byteIterator;
using detail::bytesToWord;
using detail::canonicalAddress;
using detail::checkedRange;
using detail::codeLookup;
using detail::mapLookup;
using detail::toSize;
using detail::validateEnvironment;
using detail::wordToBytes;
using detail::zeroWord;

namespace {

static_assert(std::is_nothrow_move_assignable_v<WordMap>,
              "persistent-state rollback must not allocate");

void markResourceExhausted(ExecutionResult &Result) noexcept {
  Result.Status = ExecutionStatus::Faulted;
  Result.FaultKind = ExecutionFaultKind::ResourceExhausted;
  Result.Error.clear();
}

ExecutionResult resourceExhaustionBeforeSnapshot() {
  ExecutionResult Result;
  markResourceExhausted(Result);
  Result.HasPersistentStateSnapshot = false;
  return Result;
}

bool chargeFits(size_t Used, size_t Amount, size_t Limit) {
  return Used <= Limit && Amount <= Limit - Used;
}

/// Assigns one logical EVM state word while keeping the host representation
/// sparse. Zero is the default EVM value, so clearing a present slot releases
/// its entry and clearing an absent slot is a no-op. A failed insertion leaves
/// both the map and the shared entry count unchanged.
bool assignSparseStateWord(WordMap &State, llvm::APInt Key, llvm::APInt Value,
                           size_t &EntryCount, size_t EntryLimit) {
  const auto Existing = State.find(Key);
  if (Value.isZero()) {
    if (Existing != State.end()) {
      State.erase(Existing);
      --EntryCount;
    }
    return true;
  }
  if (Existing != State.end()) {
    Existing->second = std::move(Value);
    return true;
  }
  if (!chargeFits(EntryCount, 1, EntryLimit))
    return false;
  State.emplace(std::move(Key), std::move(Value));
  ++EntryCount;
  return true;
}

void runInterpreter(const EVMLowIR &Program, ExecutionEnvironment &Environment,
                    InterpreterOptions Options, ExecutionResult &Result) {
  // EIP-211's return-data buffer belongs to the executing frame but is not
  // the frame's output.  STOP must not expose the most recent subcall's bytes
  // as though this frame had returned them.
  llvm::ArrayRef<uint8_t> ReturnDataBuffer = Environment.InitialReturnData;
  const size_t MemoryLimit =
      Options.MaxMemoryBytes - (Options.MaxMemoryBytes % kWordBytes);

  uint64_t PC = kEntryPC;
  bool Fault = false;
  size_t LogEntries = 0;
  size_t LogDataBytes = 0;
  size_t PersistentStateEntries = Result.Storage.size();
  if (!chargeFits(PersistentStateEntries, Result.TransientStorage.size(),
                  Options.MaxPersistentStateEntries)) {
    markResourceExhausted(Result);
    return;
  }
  PersistentStateEntries += Result.TransientStorage.size();
  auto Fail = [&](llvm::Twine Message) {
    if (!Fault) {
      Result.Status = ExecutionStatus::Faulted;
      Result.FaultKind = ExecutionFaultKind::Semantic;
      Result.Error = Message.str();
      Result.FinalPC = PC;
      Fault = true;
    }
  };
  auto Exhaust = [&]() {
    if (!Fault) {
      markResourceExhausted(Result);
      Result.FinalPC = PC;
      Fault = true;
    }
  };
  auto Pop = [&]() -> llvm::APInt {
    if (Result.Stack.empty()) {
      Fail("stack underflow");
      return zeroWord();
    }
    llvm::APInt Value = std::move(Result.Stack.back());
    Result.Stack.pop_back();
    return Value;
  };
  auto Push = [&](llvm::APInt Value) {
    if (Result.Stack.size() >= kStackLimit) {
      Fail("stack overflow (limit " + llvm::Twine(kStackLimit) + ")");
      return;
    }
    Result.Stack.push_back(std::move(Value));
  };
  auto PreflightStack = [&](const LowInstruction &Instruction) {
    const size_t StackHeight = Result.Stack.size();
    const size_t RequiredHeight = Instruction.requiredStackHeight();
    if (StackHeight < RequiredHeight) {
      Fail("stack underflow in " + Instruction.Info.Name);
      return false;
    }

    const size_t StackPops = Instruction.stackPops();
    if (StackHeight < StackPops) {
      Fail("stack underflow in " + Instruction.Info.Name);
      return false;
    }
    const size_t RetainedHeight = StackHeight - StackPops;
    const size_t StackPushes = Instruction.stackPushes();
    if (RetainedHeight > kStackLimit ||
        StackPushes > kStackLimit - RetainedHeight) {
      Fail("stack overflow in " + Instruction.Info.Name + " (limit " +
           llvm::Twine(kStackLimit) + ")");
      return false;
    }
    return true;
  };
  auto Range = [&](const llvm::APInt &OffsetWord, const llvm::APInt &SizeWord,
                   size_t &Offset, size_t &Size, size_t &End) {
    auto S = toSize(SizeWord, MemoryLimit);
    if (!S) {
      Exhaust();
      return false;
    }
    // EVM zero-length memory operations do not expand memory and accept any
    // full-word offset because the offset is never dereferenced.
    if (*S == 0) {
      Offset = 0;
      Size = 0;
      End = 0;
      return true;
    }
    auto O = toSize(OffsetWord, MemoryLimit);
    if (!O || !checkedRange(*O, *S, MemoryLimit, End)) {
      Exhaust();
      return false;
    }
    Offset = *O;
    Size = *S;
    return true;
  };
  auto EnsureMemory = [&](size_t End) {
    if (End > Result.Memory.size()) {
      const size_t RoundedEnd = llvm::alignTo(End, size_t{kWordBytes});
      Result.Memory.resize(RoundedEnd, 0);
    }
  };
  auto CopyToMemory = [&](llvm::ArrayRef<uint8_t> Source,
                          std::optional<size_t> SourceOffset,
                          size_t Destination, size_t Size) {
    EnsureMemory(Destination + Size);
    if (!SourceOffset || *SourceOffset >= Source.size()) {
      std::fill_n(byteIterator(Result.Memory, Destination), Size, uint8_t{0});
      return;
    }
    const size_t Available = std::min(Size, Source.size() - *SourceOffset);
    std::copy_n(Source.begin() + *SourceOffset, Available,
                byteIterator(Result.Memory, Destination));
    std::fill_n(byteIterator(Result.Memory, Destination + Available),
                Size - Available, uint8_t{0});
  };

  while (Result.Status == ExecutionStatus::Running && !Fault) {
    if (PC >= Program.Code.size()) {
      Result.Status = ExecutionStatus::Stopped;
      Result.FinalPC = PC;
      break;
    }
    if (Result.Steps >= Options.MaxSteps) {
      Result.Status = ExecutionStatus::StepLimit;
      Result.Error = "execution step limit exceeded";
      Result.FinalPC = PC;
      break;
    }
    const auto Found = llvm::lower_bound(
        Program.Instructions, PC,
        [](const LowInstruction &Instruction, uint64_t Address) {
          return Instruction.PC < Address;
        });
    if (Found == Program.Instructions.end() || Found->PC != PC) {
      Fail("program counter points into PUSH immediate data");
      break;
    }
    const LowInstruction &Instruction = *Found;
    const Opcode Op = Instruction.opcode();
    Result.FinalPC = PC;
    TraceEntry Trace{PC, Op, Result.Stack.size(), 0};
    ++Result.Steps;
    uint64_t NextPC = Instruction.NextPC;

    if (!Instruction.isExecutable()) {
      if (Instruction.ImmediateStatus == ImmediateDecodeStatus::Invalid)
        Fail("invalid immediate in " + std::string(Instruction.Info.Name));
      else if (Instruction.DecodeStatus == OpcodeDecodeStatus::Inactive)
        Fail("inactive opcode");
      else
        Fail("unknown opcode");
    } else if (!PreflightStack(Instruction)) {
      // A stack fault is detected before any opcode-specific side effect.
    } else if (Instruction.is(Opcode::STOP)) {
      Result.Status = ExecutionStatus::Stopped;
    } else if (Instruction.isPush()) {
      Push(Instruction.Immediate);
    } else if (Instruction.isDup()) {
      const size_t Depth = Instruction.dupDepth();
      if (Result.Stack.size() < Depth)
        Fail("stack underflow in DUP");
      else
        Push(Result.Stack[Result.Stack.size() - Depth]);
    } else if (Instruction.isSwap()) {
      const size_t Depth = Instruction.swapDepth();
      if (Result.Stack.size() <= Depth)
        Fail("stack underflow in SWAP");
      else
        std::swap(Result.Stack.back(),
                  Result.Stack[Result.Stack.size() - Depth - 1]);
    } else if (Instruction.isExchange()) {
      const auto [First, Second] = *Instruction.exchangeDepths();
      if (Result.Stack.size() <= Second)
        Fail("stack underflow in EXCHANGE");
      else
        std::swap(Result.Stack[Result.Stack.size() - First - 1],
                  Result.Stack[Result.Stack.size() - Second - 1]);
    } else if (Instruction.isLog()) {
      llvm::APInt OffsetWord = Pop();
      llvm::APInt SizeWord = Pop();
      size_t Offset = 0, Size = 0, End = 0;
      LogEntry Log;
      if (!Fault && Range(OffsetWord, SizeWord, Offset, Size, End)) {
        if (!chargeFits(LogEntries, 1, Options.MaxLogEntries) ||
            !chargeFits(LogDataBytes, Size, Options.MaxLogDataBytes)) {
          Exhaust();
        } else {
          ++LogEntries;
          LogDataBytes += Size;
          EnsureMemory(End);
          Log.Data.assign(byteIterator(Result.Memory, Offset),
                          byteIterator(Result.Memory, End));
          for (unsigned I = 0; I < logTopicCount(Op); ++I)
            Log.Topics.push_back(Pop());
          if (!Fault)
            Result.Logs.push_back(std::move(Log));
        }
      }
    } else if (isALU(Instruction.Info)) {
      llvm::SmallVector<llvm::APInt, kMaxALUStackPops> Inputs;
      Inputs.reserve(Instruction.Info.StackPops);
      for (uint8_t I = 0; I < Instruction.Info.StackPops; ++I)
        Inputs.push_back(Pop());
      if (!Fault) {
        auto Value = evaluateALU(Op, Inputs);
        if (!Value)
          Fail("scalar ALU semantics are not implemented");
        else
          Push(std::move(*Value));
      }
    } else {
      switch (Op) {
      case Opcode::SHA3: {
        llvm::APInt OffsetWord = Pop(), SizeWord = Pop();
        size_t Offset = 0, Size = 0, End = 0;
        if (!Fault && Range(OffsetWord, SizeWord, Offset, Size, End)) {
          EnsureMemory(End);
          Push(
              keccak256Word(llvm::ArrayRef(Result.Memory).slice(Offset, Size)));
        }
        break;
      }
      case Opcode::ADDRESS:
        Push(Environment.Address);
        break;
      case Opcode::BALANCE: {
        llvm::APInt Address = Pop();
        if (!Fault)
          Push(mapLookup(Environment.Balances, canonicalAddress(Address)));
        break;
      }
      case Opcode::ORIGIN:
        Push(Environment.Origin);
        break;
      case Opcode::CALLER:
        Push(Environment.Caller);
        break;
      case Opcode::CALLVALUE:
        Push(Environment.CallValue);
        break;
      case Opcode::CALLDATALOAD: {
        llvm::APInt OffsetWord = Pop();
        auto Offset = toSize(OffsetWord, std::numeric_limits<size_t>::max());
        if (!Fault)
          Push(Offset ? bytesToWord(Environment.Calldata, *Offset)
                      : zeroWord());
        break;
      }
      case Opcode::CALLDATASIZE:
        Push(llvm::APInt(kWordBits, Environment.Calldata.size()));
        break;
      case Opcode::CALLDATACOPY:
      case Opcode::CODECOPY:
      case Opcode::RETURNDATACOPY: {
        llvm::APInt DestinationWord = Pop(), SourceWord = Pop(),
                    SizeWord = Pop();
        size_t Destination = 0, Size = 0, End = 0;
        auto Source = toSize(SourceWord, std::numeric_limits<size_t>::max());
        if (!Fault &&
            Range(DestinationWord, SizeWord, Destination, Size, End)) {
          const llvm::ArrayRef<uint8_t> Bytes =
              Op == Opcode::CALLDATACOPY
                  ? llvm::ArrayRef<uint8_t>(Environment.Calldata)
              : Op == Opcode::CODECOPY ? llvm::ArrayRef<uint8_t>(Program.Code)
                                       : ReturnDataBuffer;
          if (Op == Opcode::RETURNDATACOPY &&
              (!Source || *Source > Bytes.size() ||
               Size > Bytes.size() - *Source)) {
            Fail("RETURNDATACOPY source range is out of bounds");
          } else {
            CopyToMemory(Bytes, Source, Destination, Size);
          }
        }
        break;
      }
      case Opcode::CODESIZE:
        Push(llvm::APInt(kWordBits, Program.Code.size()));
        break;
      case Opcode::GASPRICE:
        Push(Environment.GasPrice);
        break;
      case Opcode::EXTCODESIZE: {
        llvm::APInt Address = Pop();
        if (!Fault)
          Push(llvm::APInt(
              kWordBits, codeLookup(Environment.ExternalCode, Address).size()));
        break;
      }
      case Opcode::EXTCODECOPY: {
        llvm::APInt Address = Pop(), DestinationWord = Pop(),
                    SourceWord = Pop(), SizeWord = Pop();
        size_t Destination = 0, Size = 0, End = 0;
        auto Source = toSize(SourceWord, std::numeric_limits<size_t>::max());
        if (!Fault && Range(DestinationWord, SizeWord, Destination, Size, End))
          CopyToMemory(codeLookup(Environment.ExternalCode, Address), Source,
                       Destination, Size);
        break;
      }
      case Opcode::RETURNDATASIZE:
        Push(llvm::APInt(kWordBits, ReturnDataBuffer.size()));
        break;
      case Opcode::EXTCODEHASH: {
        llvm::APInt Address = Pop();
        if (!Fault)
          Push(mapLookup(Environment.CodeHashes, canonicalAddress(Address)));
        break;
      }
      case Opcode::BLOCKHASH: {
        llvm::APInt Number = Pop();
        if (!Fault) {
          const bool IsPrevious = Number.ult(Environment.BlockNumber);
          const bool IsAvailable =
              IsPrevious &&
              (Environment.BlockNumber - Number)
                  .ule(llvm::APInt(kWordBits, kBlockHashHistoryWindow));
          Push(IsAvailable ? mapLookup(Environment.BlockHashes, Number)
                           : zeroWord());
        }
        break;
      }
      case Opcode::COINBASE:
        Push(Environment.Coinbase);
        break;
      case Opcode::TIMESTAMP:
        Push(Environment.Timestamp);
        break;
      case Opcode::NUMBER:
        Push(Environment.BlockNumber);
        break;
#define EVM_FORK_ENVIRONMENT_VALUE(OPCODE, BEFORE_FORK_FIELD,                  \
                                   AT_OR_AFTER_FIELD, TRANSITION_FORK)         \
  case Opcode::OPCODE:                                                         \
    Push(hardforkAtLeast(Program.Fork, Hardfork::TRANSITION_FORK)              \
             ? Environment.AT_OR_AFTER_FIELD                                   \
             : Environment.BEFORE_FORK_FIELD);                                 \
    break;
#include "neverd/evm/runtime/EVMForkSemantics.def"
      case Opcode::GASLIMIT:
        Push(Environment.GasLimit);
        break;
      case Opcode::CHAINID:
        Push(Environment.ChainID);
        break;
      case Opcode::SELFBALANCE:
        Push(mapLookup(Environment.Balances,
                       canonicalAddress(Environment.Address)));
        break;
      case Opcode::BASEFEE:
        Push(Environment.BaseFee);
        break;
      case Opcode::BLOBHASH: {
        llvm::APInt Index = Pop();
        if (!Fault &&
            Index.getActiveBits() <= std::numeric_limits<size_t>::digits &&
            Index.getZExtValue() < Environment.BlobHashes.size())
          Push(Environment.BlobHashes[Index.getZExtValue()]);
        else if (!Fault)
          Push(zeroWord());
        break;
      }
      case Opcode::BLOBBASEFEE:
        Push(Environment.BlobBaseFee);
        break;
      case Opcode::SLOTNUM:
        Push(llvm::APInt(kWordBits, Environment.SlotNumber));
        break;
      case Opcode::POP:
        (void)Pop();
        break;
      case Opcode::MLOAD: {
        llvm::APInt OffsetWord = Pop();
        auto Offset = toSize(OffsetWord, MemoryLimit);
        if (!Offset || MemoryLimit < kWordBytes ||
            *Offset > MemoryLimit - kWordBytes)
          Exhaust();
        else if (!Fault) {
          EnsureMemory(*Offset + kWordBytes);
          Push(bytesToWord(Result.Memory, *Offset));
        }
        break;
      }
      case Opcode::MSTORE: {
        llvm::APInt OffsetWord = Pop(), Value = Pop();
        auto Offset = toSize(OffsetWord, MemoryLimit);
        if (!Offset || MemoryLimit < kWordBytes ||
            *Offset > MemoryLimit - kWordBytes)
          Exhaust();
        else if (!Fault) {
          EnsureMemory(*Offset + kWordBytes);
          wordToBytes(Value, Result.Memory.data() + *Offset);
        }
        break;
      }
      case Opcode::MSTORE8: {
        llvm::APInt OffsetWord = Pop(), Value = Pop();
        auto Offset = toSize(OffsetWord, MemoryLimit);
        if (!Offset || *Offset >= MemoryLimit)
          Exhaust();
        else if (!Fault) {
          EnsureMemory(*Offset + 1);
          Result.Memory[*Offset] = static_cast<uint8_t>(
              Value.extractBitsAsZExtValue(kBitsPerByte, 0));
        }
        break;
      }
      case Opcode::SLOAD: {
        llvm::APInt Key = Pop();
        if (!Fault)
          Push(mapLookup(Result.Storage, Key));
        break;
      }
      case Opcode::SSTORE: {
        llvm::APInt Key = Pop(), Value = Pop();
        if (!Fault &&
            !assignSparseStateWord(Result.Storage, std::move(Key),
                                   std::move(Value), PersistentStateEntries,
                                   Options.MaxPersistentStateEntries))
          Exhaust();
        break;
      }
      case Opcode::JUMP:
      case Opcode::JUMPI: {
        llvm::APInt Destination = Pop();
        llvm::APInt Condition =
            Op == Opcode::JUMPI ? Pop() : llvm::APInt(kWordBits, 1);
        if (!Fault && !Condition.isZero()) {
          if (Destination.getActiveBits() >
                  std::numeric_limits<uint64_t>::digits ||
              !Program.JumpDestinations.contains(Destination.getZExtValue()))
            Fail("jump target is not a JUMPDEST");
          else
            NextPC = Destination.getZExtValue();
        }
        break;
      }
      case Opcode::PC:
        Push(llvm::APInt(kWordBits, PC));
        break;
      case Opcode::MSIZE:
        Push(llvm::APInt(kWordBits, Result.Memory.size()));
        break;
      case Opcode::GAS:
        Push(Environment.GasRemaining);
        break;
      case Opcode::JUMPDEST:
        break;
      case Opcode::TLOAD: {
        llvm::APInt Key = Pop();
        if (!Fault)
          Push(mapLookup(Result.TransientStorage, Key));
        break;
      }
      case Opcode::TSTORE: {
        llvm::APInt Key = Pop(), Value = Pop();
        if (!Fault &&
            !assignSparseStateWord(Result.TransientStorage, std::move(Key),
                                   std::move(Value), PersistentStateEntries,
                                   Options.MaxPersistentStateEntries))
          Exhaust();
        break;
      }
      case Opcode::MCOPY: {
        llvm::APInt DestinationWord = Pop(), SourceWord = Pop(),
                    SizeWord = Pop();
        size_t Destination = 0, Size = 0, DestinationEnd = 0;
        size_t Source = 0, Ignored = 0, SourceEnd = 0;
        if (!Fault &&
            Range(DestinationWord, SizeWord, Destination, Size,
                  DestinationEnd) &&
            Range(SourceWord, SizeWord, Source, Ignored, SourceEnd)) {
          EnsureMemory(std::max(DestinationEnd, SourceEnd));
          if (Size != 0)
            std::memmove(Result.Memory.data() + Destination,
                         Result.Memory.data() + Source, Size);
        }
        break;
      }
      case Opcode::CREATE:
      case Opcode::CREATE2: {
        (void)Pop(); // value
        llvm::APInt OffsetWord = Pop(), SizeWord = Pop();
        if (Op == Opcode::CREATE2)
          (void)Pop(); // salt
        size_t Offset = 0, Size = 0, End = 0;
        if (!Fault && Range(OffsetWord, SizeWord, Offset, Size, End)) {
          EnsureMemory(End);
          if (Environment.CreateSuccess) {
            ReturnDataBuffer = {};
            Push(canonicalAddress(Environment.CreatedAddress));
          } else {
            ReturnDataBuffer = Environment.CreateReturnData;
            Push(zeroWord());
          }
        }
        break;
      }
      case Opcode::CALL:
      case Opcode::CALLCODE:
      case Opcode::DELEGATECALL:
      case Opcode::STATICCALL: {
        (void)Pop(); // gas
        (void)Pop(); // address
        if (Op == Opcode::CALL || Op == Opcode::CALLCODE)
          (void)Pop(); // value
        llvm::APInt InputOffsetWord = Pop(), InputSizeWord = Pop(),
                    OutputOffsetWord = Pop(), OutputSizeWord = Pop();
        size_t InputOffset = 0, InputSize = 0, InputEnd = 0;
        size_t OutputOffset = 0, OutputSize = 0, OutputEnd = 0;
        if (!Fault &&
            Range(InputOffsetWord, InputSizeWord, InputOffset, InputSize,
                  InputEnd) &&
            Range(OutputOffsetWord, OutputSizeWord, OutputOffset, OutputSize,
                  OutputEnd)) {
          EnsureMemory(std::max(InputEnd, OutputEnd));
          ReturnDataBuffer = Environment.CallReturnData;
          const size_t CopySize = std::min(OutputSize, ReturnDataBuffer.size());
          if (CopySize != 0)
            std::copy_n(ReturnDataBuffer.begin(), CopySize,
                        byteIterator(Result.Memory, OutputOffset));
          Push(boolWord(Environment.CallSuccess));
        }
        break;
      }
      case Opcode::RETURN:
      case Opcode::REVERT: {
        llvm::APInt OffsetWord = Pop(), SizeWord = Pop();
        size_t Offset = 0, Size = 0, End = 0;
        if (!Fault && Range(OffsetWord, SizeWord, Offset, Size, End)) {
          EnsureMemory(End);
          Result.ReturnData.assign(byteIterator(Result.Memory, Offset),
                                   byteIterator(Result.Memory, End));
          Result.Status = Op == Opcode::RETURN ? ExecutionStatus::Returned
                                               : ExecutionStatus::Reverted;
        }
        break;
      }
      case Opcode::INVALID:
        Fail("INVALID opcode executed");
        break;
      case Opcode::SELFDESTRUCT: {
        llvm::APInt Beneficiary = Pop();
        if (!Fault) {
          Result.SelfDestructBeneficiary = canonicalAddress(Beneficiary);
          Result.Status = ExecutionStatus::SelfDestructed;
        }
        break;
      }
      default:
        Fail("opcode semantics are not implemented");
        break;
      }
    }

    Trace.StackAfter = Result.Stack.size();
    if (Options.RecordTrace) {
      if (Result.Trace.size() >= Options.MaxTraceEntries) {
        Exhaust();
      } else {
        Result.Trace.push_back(Trace);
      }
    }
    Result.FinalPC = PC;
    if (Result.Status == ExecutionStatus::Running && !Fault)
      PC = NextPC;
  }
}

bool restoresPersistentState(ExecutionStatus Status) {
  return Status == ExecutionStatus::Reverted ||
         Status == ExecutionStatus::Faulted ||
         Status == ExecutionStatus::StepLimit;
}

llvm::Expected<ExecutionResult> executeOwned(const EVMLowIR &Program,
                                             ExecutionEnvironment &Environment,
                                             InterpreterOptions Options) {
  ExecutionResult Result;
  try {
    Result.Storage = Environment.Storage;
    Result.TransientStorage = Environment.TransientStorage;
    runInterpreter(Program, Environment, Options, Result);
  } catch (const std::bad_alloc &) {
    markResourceExhausted(Result);
  } catch (const std::length_error &) {
    markResourceExhausted(Result);
  }

  if (restoresPersistentState(Result.Status)) {
    Result.Storage = std::move(Environment.Storage);
    Result.TransientStorage = std::move(Environment.TransientStorage);
    Result.Logs.clear();
    Result.SelfDestructBeneficiary.reset();
  }
  return Result;
}

llvm::Error validateExecutionInputs(const EVMLowIR &Program,
                                    const ExecutionEnvironment &Environment,
                                    const InterpreterOptions &Options) {
  if (llvm::Error E = detail::verifyLowIRForExecution(Program))
    return E;
  return validateEnvironment(Environment, Options);
}

llvm::Expected<ExecutionResult>
validateAndExecuteOwned(const EVMLowIR &Program,
                        ExecutionEnvironment &Environment,
                        InterpreterOptions Options) {
  try {
    if (llvm::Error E = validateExecutionInputs(Program, Environment, Options))
      return std::move(E);
    return executeOwned(Program, Environment, Options);
  } catch (const std::bad_alloc &) {
    return resourceExhaustionBeforeSnapshot();
  } catch (const std::length_error &) {
    return resourceExhaustionBeforeSnapshot();
  }
}

} // namespace

llvm::Expected<ExecutionResult> execute(const EVMLowIR &Program) {
  ExecutionEnvironment Environment;
  return validateAndExecuteOwned(Program, Environment, {});
}

llvm::Expected<ExecutionResult> execute(const EVMLowIR &Program,
                                        ExecutionEnvironment &&Environment,
                                        InterpreterOptions Options) {
  return validateAndExecuteOwned(Program, Environment, Options);
}

llvm::Expected<ExecutionResult> execute(const EVMLowIR &Program,
                                        const ExecutionEnvironment &Environment,
                                        InterpreterOptions Options) {
  try {
    if (llvm::Error E = validateExecutionInputs(Program, Environment, Options))
      return std::move(E);
    ExecutionEnvironment OwnedEnvironment = Environment;
    return executeOwned(Program, OwnedEnvironment, Options);
  } catch (const std::bad_alloc &) {
    return resourceExhaustionBeforeSnapshot();
  } catch (const std::length_error &) {
    return resourceExhaustionBeforeSnapshot();
  }
}

} // namespace neverd::evm
