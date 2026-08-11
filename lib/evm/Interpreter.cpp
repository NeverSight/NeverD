//===- Interpreter.cpp - Deterministic EVM semantic oracle --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/Interpreter.h"

#include "Keccak.h"

#include "neverd/evm/Semantics.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/bit.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <utility>

namespace neverd::evm {
namespace {

llvm::APInt zeroWord() { return llvm::APInt(kWordBits, 0); }
llvm::APInt boolWord(bool Value) {
  return llvm::APInt(kWordBits, Value ? 1 : 0);
}

llvm::Error validateWordWidth(llvm::Twine Field, const llvm::APInt &Value) {
  if (Value.getBitWidth() == kWordBits)
    return llvm::Error::success();
  return llvm::make_error<llvm::StringError>(
      "evm: environment field " + Field + " must be " + llvm::Twine(kWordBits) +
          "-bit, got " + llvm::Twine(Value.getBitWidth()) + "-bit",
      llvm::inconvertibleErrorCode());
}

llvm::Error validateWordMap(llvm::StringRef Name, const WordMap &Map) {
  for (const auto &[Key, Value] : Map) {
    if (llvm::Error E = validateWordWidth(llvm::Twine(Name) + " key", Key))
      return E;
    if (llvm::Error E = validateWordWidth(llvm::Twine(Name) + " value", Value))
      return E;
  }
  return llvm::Error::success();
}

llvm::Error validateAddress(llvm::Twine Field, const llvm::APInt &Value) {
  if (llvm::Error E = validateWordWidth(Field, Value))
    return E;
  if (Value.getActiveBits() <= kAddressBits)
    return llvm::Error::success();
  return llvm::make_error<llvm::StringError>(
      "evm: environment field " + Field + " must fit a " +
          llvm::Twine(kAddressBits) + "-bit EVM address",
      llvm::inconvertibleErrorCode());
}

llvm::Error validateAddressMap(llvm::StringRef Name, const WordMap &Map) {
  for (const auto &[Key, Value] : Map) {
    if (llvm::Error E = validateAddress(llvm::Twine(Name) + " key", Key))
      return E;
    if (llvm::Error E = validateWordWidth(llvm::Twine(Name) + " value", Value))
      return E;
  }
  return llvm::Error::success();
}

llvm::Error validateEnvironment(const ExecutionEnvironment &Environment) {
  struct NamedWord {
    llvm::StringLiteral Name;
    const llvm::APInt *Value;
  };
  const NamedWord Words[] = {
      {"CallValue", &Environment.CallValue},
      {"GasPrice", &Environment.GasPrice},
      {"Timestamp", &Environment.Timestamp},
      {"BlockNumber", &Environment.BlockNumber},
      {"PrevRandao", &Environment.PrevRandao},
      {"GasLimit", &Environment.GasLimit},
      {"ChainID", &Environment.ChainID},
      {"BaseFee", &Environment.BaseFee},
      {"BlobBaseFee", &Environment.BlobBaseFee},
      {"GasRemaining", &Environment.GasRemaining},
  };
  for (const NamedWord &Word : Words)
    if (llvm::Error E = validateWordWidth(Word.Name, *Word.Value))
      return E;

  const NamedWord Addresses[] = {
      {"Address", &Environment.Address},
      {"Origin", &Environment.Origin},
      {"Caller", &Environment.Caller},
      {"Coinbase", &Environment.Coinbase},
      {"CreatedAddress", &Environment.CreatedAddress},
  };
  for (const NamedWord &Address : Addresses)
    if (llvm::Error E = validateAddress(Address.Name, *Address.Value))
      return E;

  const std::pair<llvm::StringLiteral, const WordMap *> Maps[] = {
      {"Storage", &Environment.Storage},
      {"TransientStorage", &Environment.TransientStorage},
      {"BlockHashes", &Environment.BlockHashes},
  };
  for (const auto &[Name, Map] : Maps)
    if (llvm::Error E = validateWordMap(Name, *Map))
      return E;

  if (llvm::Error E = validateAddressMap("Balances", Environment.Balances))
    return E;
  if (llvm::Error E = validateAddressMap("CodeHashes", Environment.CodeHashes))
    return E;

  for (const auto &Entry : Environment.ExternalCode)
    if (llvm::Error E = validateAddress("ExternalCode key", Entry.first))
      return E;
  for (const llvm::APInt &Hash : Environment.BlobHashes)
    if (llvm::Error E = validateWordWidth("BlobHashes value", Hash))
      return E;
  return llvm::Error::success();
}

std::optional<size_t> toSize(const llvm::APInt &Word, size_t Limit) {
  if (Word.getActiveBits() > std::numeric_limits<size_t>::digits)
    return std::nullopt;
  const uint64_t Value = Word.getZExtValue();
  if (Value > Limit)
    return std::nullopt;
  return static_cast<size_t>(Value);
}

bool checkedRange(size_t Offset, size_t Size, size_t Limit, size_t &End) {
  if (Offset > Limit || Size > Limit - Offset)
    return false;
  End = Offset + Size;
  return true;
}

llvm::APInt bytesToWord(const std::vector<uint8_t> &Bytes, size_t Offset) {
  llvm::APInt Result(kWordBits, 0);
  for (size_t I = 0; I < kWordBytes; ++I) {
    Result <<= kBitsPerByte;
    if (Offset <= Bytes.size() && I < Bytes.size() - Offset)
      Result |= Bytes[Offset + I];
  }
  return Result;
}

void wordToBytes(const llvm::APInt &Word, uint8_t *Output) {
  for (size_t I = 0; I < kWordBytes; ++I)
    Output[I] = static_cast<uint8_t>(Word.extractBitsAsZExtValue(
        kBitsPerByte,
        static_cast<unsigned>((kWordBytes - 1 - I) * kBitsPerByte)));
}

llvm::APInt mapLookup(const WordMap &Map, const llvm::APInt &Key) {
  auto It = Map.find(Key);
  return It == Map.end() ? zeroWord() : It->second;
}

llvm::APInt canonicalAddress(const llvm::APInt &Word) {
  return Word.trunc(kAddressBits).zext(kWordBits);
}

std::vector<uint8_t>::iterator byteIterator(std::vector<uint8_t> &Bytes,
                                            size_t Offset) {
  return Bytes.begin() +
         static_cast<std::vector<uint8_t>::difference_type>(Offset);
}

std::vector<uint8_t>::const_iterator
byteIterator(const std::vector<uint8_t> &Bytes, size_t Offset) {
  return Bytes.begin() +
         static_cast<std::vector<uint8_t>::difference_type>(Offset);
}

const std::vector<uint8_t> &codeLookup(const BytecodeMap &Map,
                                       const llvm::APInt &Address) {
  static const std::vector<uint8_t> Empty;
  auto It = Map.find(canonicalAddress(Address));
  return It == Map.end() ? Empty : It->second;
}

} // namespace

llvm::Expected<ExecutionResult> execute(const EVMLowIR &Program,
                                        ExecutionEnvironment Environment,
                                        InterpreterOptions Options) {
  if (llvm::Error E = validateEnvironment(Environment))
    return std::move(E);

  ExecutionResult Result;
  Result.Storage = std::move(Environment.Storage);
  Result.TransientStorage = std::move(Environment.TransientStorage);
  // EIP-211's return-data buffer belongs to the executing frame but is not the
  // frame's output.  STOP must not expose the most recent subcall's bytes as
  // though this frame had returned them.
  std::vector<uint8_t> ReturnDataBuffer =
      std::move(Environment.InitialReturnData);
  const size_t MemoryLimit =
      Options.MaxMemoryBytes - (Options.MaxMemoryBytes % kWordBytes);

  llvm::DenseMap<uint64_t, const LowInstruction *> Instructions;
  for (const auto &Instruction : Program.Instructions)
    Instructions[Instruction.PC] = &Instruction;

  uint64_t PC = kEntryPC;
  bool Fault = false;
  auto Fail = [&](llvm::Twine Message) {
    if (!Fault) {
      Result.Status = ExecutionStatus::Faulted;
      Result.Error = Message.str();
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
  auto Range = [&](const llvm::APInt &OffsetWord, const llvm::APInt &SizeWord,
                   size_t &Offset, size_t &Size, size_t &End) {
    auto S = toSize(SizeWord, MemoryLimit);
    if (!S) {
      Fail("memory range exceeds configured limit");
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
      Fail("memory range exceeds configured limit");
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
  auto CopyToMemory = [&](const std::vector<uint8_t> &Source,
                          std::optional<size_t> SourceOffset,
                          size_t Destination, size_t Size) {
    EnsureMemory(Destination + Size);
    if (!SourceOffset || *SourceOffset >= Source.size()) {
      std::fill_n(byteIterator(Result.Memory, Destination), Size, uint8_t{0});
      return;
    }
    const size_t Available = std::min(Size, Source.size() - *SourceOffset);
    std::copy_n(byteIterator(Source, *SourceOffset), Available,
                byteIterator(Result.Memory, Destination));
    std::fill_n(byteIterator(Result.Memory, Destination + Available),
                Size - Available, uint8_t{0});
  };

  while (Result.Status == ExecutionStatus::Running && !Fault) {
    if (Result.Steps >= Options.MaxSteps) {
      Result.Status = ExecutionStatus::StepLimit;
      Result.Error = "execution step limit exceeded";
      Result.FinalPC = PC;
      break;
    }
    if (PC >= Program.Code.size()) {
      Result.Status = ExecutionStatus::Stopped;
      Result.FinalPC = PC;
      break;
    }
    auto Found = Instructions.find(PC);
    if (Found == Instructions.end()) {
      Fail("program counter points into PUSH immediate data");
      break;
    }
    const LowInstruction &Instruction = *Found->second;
    const Opcode Op = Instruction.opcode();
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
        EnsureMemory(End);
        Log.Data.assign(byteIterator(Result.Memory, Offset),
                        byteIterator(Result.Memory, End));
        for (unsigned I = 0; I < logTopicCount(Op); ++I)
          Log.Topics.push_back(Pop());
        if (!Fault)
          Result.Logs.push_back(std::move(Log));
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
          const std::vector<uint8_t> *Bytes =
              Op == Opcode::CALLDATACOPY ? &Environment.Calldata
              : Op == Opcode::CODECOPY   ? &Program.Code
                                         : &ReturnDataBuffer;
          if (Op == Opcode::RETURNDATACOPY &&
              (!Source || *Source > Bytes->size() ||
               Size > Bytes->size() - *Source)) {
            Fail("RETURNDATACOPY source range is out of bounds");
          } else {
            CopyToMemory(*Bytes, Source, Destination, Size);
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
      case Opcode::PREVRANDAO:
        Push(Environment.PrevRandao);
        break;
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
          Fail("MLOAD offset exceeds configured memory limit");
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
          Fail("MSTORE offset exceeds configured memory limit");
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
          Fail("MSTORE8 offset exceeds configured memory limit");
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
        if (!Fault)
          Result.Storage[std::move(Key)] = std::move(Value);
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
        if (!Fault)
          Result.TransientStorage[std::move(Key)] = std::move(Value);
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
            ReturnDataBuffer.clear();
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
    if (Options.RecordTrace)
      Result.Trace.push_back(Trace);
    Result.FinalPC = PC;
    if (Result.Status == ExecutionStatus::Running && !Fault)
      PC = NextPC;
  }
  return Result;
}

} // namespace neverd::evm
