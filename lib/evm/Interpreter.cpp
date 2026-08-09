//===- Interpreter.cpp - Deterministic EVM semantic oracle --------------===//

#include "neverd/evm/Interpreter.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <unordered_map>

namespace neverd::evm {
namespace {

inline constexpr unsigned kKeccakLaneBits = 64;
inline constexpr unsigned kKeccakDimension = 5;
inline constexpr unsigned kKeccakLaneCount =
    kKeccakDimension * kKeccakDimension;
inline constexpr unsigned kKeccakRoundCount = 24;
inline constexpr size_t kKeccak256RateBytes = 136;
inline constexpr uint8_t kKeccakDomainSeparator = 0x01;
inline constexpr uint8_t kKeccakFinalBit = 0x80;

llvm::APInt zeroWord() { return llvm::APInt(kWordBits, 0); }
llvm::APInt boolWord(bool Value) {
  return llvm::APInt(kWordBits, Value ? 1 : 0);
}

std::optional<size_t> toSize(const llvm::APInt &Word, size_t Limit) {
  if (Word.getActiveBits() > sizeof(size_t) * kBitsPerByte)
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
    Output[I] = static_cast<uint8_t>(
        Word.extractBitsAsZExtValue(
            kBitsPerByte,
            static_cast<unsigned>((kWordBytes - 1 - I) * kBitsPerByte)));
}

uint64_t rotateLeft(uint64_t Value, unsigned Shift) {
  return Shift == 0 ? Value
                    : (Value << Shift) |
                          (Value >> (kKeccakLaneBits - Shift));
}

void keccakF1600(std::array<uint64_t, kKeccakLaneCount> &State) {
  static constexpr uint64_t RoundConstants[kKeccakRoundCount] = {
      0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
      0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
      0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
      0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
      0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
      0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
      0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
      0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
  static constexpr unsigned Rotation[kKeccakLaneCount] = {
      0,  1, 62, 28, 27, 36, 44, 6,  55, 20, 3, 10, 43,
      25, 39, 41, 45, 15, 21, 8, 18, 2,  61, 56, 14};

  for (uint64_t RC : RoundConstants) {
    uint64_t C[kKeccakDimension], D[kKeccakDimension],
        B[kKeccakLaneCount];
    for (unsigned X = 0; X < kKeccakDimension; ++X)
      C[X] = State[X] ^ State[X + kKeccakDimension] ^
             State[X + 2 * kKeccakDimension] ^
             State[X + 3 * kKeccakDimension] ^
             State[X + 4 * kKeccakDimension];
    for (unsigned X = 0; X < kKeccakDimension; ++X)
      D[X] = C[(X + kKeccakDimension - 1) % kKeccakDimension] ^
             rotateLeft(C[(X + 1) % kKeccakDimension], 1);
    for (unsigned Y = 0; Y < kKeccakDimension; ++Y)
      for (unsigned X = 0; X < kKeccakDimension; ++X)
        State[X + kKeccakDimension * Y] ^= D[X];
    for (unsigned Y = 0; Y < kKeccakDimension; ++Y)
      for (unsigned X = 0; X < kKeccakDimension; ++X)
        B[Y + kKeccakDimension *
                  ((2 * X + 3 * Y) % kKeccakDimension)] =
            rotateLeft(State[X + kKeccakDimension * Y],
                       Rotation[X + kKeccakDimension * Y]);
    for (unsigned Y = 0; Y < kKeccakDimension; ++Y)
      for (unsigned X = 0; X < kKeccakDimension; ++X)
        State[X + kKeccakDimension * Y] =
            B[X + kKeccakDimension * Y] ^
            ((~B[(X + 1) % kKeccakDimension + kKeccakDimension * Y]) &
             B[(X + 2) % kKeccakDimension + kKeccakDimension * Y]);
    State[0] ^= RC;
  }
}

llvm::APInt keccak256(const uint8_t *Data, size_t Size) {
  std::array<uint64_t, kKeccakLaneCount> State{};
  while (Size >= kKeccak256RateBytes) {
    for (size_t I = 0; I < kKeccak256RateBytes; ++I)
      State[I / sizeof(uint64_t)] ^=
          static_cast<uint64_t>(Data[I])
          << ((I % sizeof(uint64_t)) * kBitsPerByte);
    keccakF1600(State);
    Data += kKeccak256RateBytes;
    Size -= kKeccak256RateBytes;
  }
  std::array<uint8_t, kKeccak256RateBytes> Last{};
  std::copy_n(Data, Size, Last.begin());
  Last[Size] = kKeccakDomainSeparator;
  Last[kKeccak256RateBytes - 1] |= kKeccakFinalBit;
  for (size_t I = 0; I < kKeccak256RateBytes; ++I)
    State[I / sizeof(uint64_t)] ^=
        static_cast<uint64_t>(Last[I])
        << ((I % sizeof(uint64_t)) * kBitsPerByte);
  keccakF1600(State);

  llvm::APInt Result(kWordBits, 0);
  for (size_t I = 0; I < kWordBytes; ++I) {
    Result <<= kBitsPerByte;
    Result |= static_cast<uint8_t>(
        State[I / sizeof(uint64_t)] >>
        ((I % sizeof(uint64_t)) * kBitsPerByte));
  }
  return Result;
}

llvm::APInt modularExponent(llvm::APInt Base, llvm::APInt Exponent) {
  llvm::APInt Result(kWordBits, 1);
  while (!Exponent.isZero()) {
    if (Exponent[0])
      Result *= Base;
    Exponent.lshrInPlace(1);
    Base *= Base;
  }
  return Result;
}

llvm::APInt signExtend(const llvm::APInt &ByteIndex, const llvm::APInt &Value) {
  if (ByteIndex.uge(kWordBytes))
    return Value;
  const unsigned Width =
      static_cast<unsigned>((ByteIndex.getZExtValue() + 1) * kBitsPerByte);
  return Value.trunc(Width).sext(kWordBits);
}

llvm::APInt mapLookup(const WordMap &Map, const llvm::APInt &Key) {
  auto It = Map.find(Key);
  return It == Map.end() ? zeroWord() : It->second;
}

const std::vector<uint8_t> &codeLookup(const BytecodeMap &Map,
                                       const llvm::APInt &Address) {
  static const std::vector<uint8_t> Empty;
  auto It = Map.find(Address);
  return It == Map.end() ? Empty : It->second;
}

} // namespace

llvm::Expected<ExecutionResult> execute(const EVMLowIR &Program,
                                        ExecutionEnvironment Environment,
                                        InterpreterOptions Options) {
  ExecutionResult Result;
  Result.Storage = std::move(Environment.Storage);
  Result.TransientStorage = std::move(Environment.TransientStorage);
  Result.ReturnData = std::move(Environment.InitialReturnData);

  std::unordered_map<uint64_t, const LowInstruction *> Instructions;
  for (const auto &Instruction : Program.Instructions)
    Instructions[Instruction.PC] = &Instruction;

  uint64_t PC = 0;
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
    auto S = toSize(SizeWord, Options.MaxMemoryBytes);
    if (!S) {
      Fail("memory range exceeds configured limit");
      return false;
    }
    // EVM zero-length memory operations do not expand memory and accept any
    // A full-word offset is valid because the offset is never dereferenced.
    if (*S == 0) {
      Offset = 0;
      Size = 0;
      End = 0;
      return true;
    }
    auto O = toSize(OffsetWord, Options.MaxMemoryBytes);
    if (!O || !checkedRange(*O, *S, Options.MaxMemoryBytes, End)) {
      Fail("memory range exceeds configured limit");
      return false;
    }
    Offset = *O;
    Size = *S;
    return true;
  };
  auto EnsureMemory = [&](size_t End) {
    if (End > Result.Memory.size()) {
      const size_t RoundedEnd =
          (End + kWordBytes - 1) & ~size_t(kWordBytes - 1);
      Result.Memory.resize(RoundedEnd, 0);
    }
  };
  auto CopyToMemory = [&](const std::vector<uint8_t> &Source,
                          std::optional<size_t> SourceOffset,
                          size_t Destination, size_t Size) {
    EnsureMemory(Destination + Size);
    if (!SourceOffset || *SourceOffset >= Source.size()) {
      std::fill_n(Result.Memory.begin() + Destination, Size, uint8_t{0});
      return;
    }
    const size_t Available =
        std::min(Size, Source.size() - *SourceOffset);
    std::copy_n(Source.begin() + *SourceOffset, Available,
                Result.Memory.begin() + Destination);
    std::fill_n(Result.Memory.begin() + Destination + Available,
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
    const Opcode Op = Instruction.Op;
    TraceEntry Trace{PC, Op, Result.Stack.size(), 0};
    ++Result.Steps;
    uint64_t NextPC = Instruction.NextPC;

    if (!Instruction.Known) {
      Fail("unknown or inactive opcode");
    } else if (Op == Opcode::STOP) {
      Result.Status = ExecutionStatus::Stopped;
    } else if (isPush(Op)) {
      Push(Instruction.Immediate);
    } else if (isDup(Op)) {
      const size_t Depth = dupDepth(Op);
      if (Result.Stack.size() < Depth)
        Fail("stack underflow in DUP");
      else
        Push(Result.Stack[Result.Stack.size() - Depth]);
    } else if (isSwap(Op)) {
      const size_t Depth = swapDepth(Op);
      if (Result.Stack.size() <= Depth)
        Fail("stack underflow in SWAP");
      else
        std::swap(Result.Stack.back(),
                  Result.Stack[Result.Stack.size() - Depth - 1]);
    } else if (isLog(Op)) {
      llvm::APInt OffsetWord = Pop();
      llvm::APInt SizeWord = Pop();
      size_t Offset = 0, Size = 0, End = 0;
      LogEntry Log;
      if (!Fault && Range(OffsetWord, SizeWord, Offset, Size, End)) {
        EnsureMemory(End);
        Log.Data.assign(Result.Memory.begin() + Offset,
                        Result.Memory.begin() + End);
        for (unsigned I = 0; I < logTopicCount(Op); ++I)
          Log.Topics.push_back(Pop());
        if (!Fault)
          Result.Logs.push_back(std::move(Log));
      }
    } else {
      switch (Op) {
      case Opcode::ADD: {
        llvm::APInt A = Pop(), B = Pop();
        if (!Fault)
          Push(A + B);
        break;
      }
      case Opcode::MUL: {
        llvm::APInt A = Pop(), B = Pop();
        if (!Fault)
          Push(A * B);
        break;
      }
      case Opcode::SUB: {
        llvm::APInt A = Pop(), B = Pop();
        if (!Fault)
          Push(A - B);
        break;
      }
      case Opcode::DIV: {
        llvm::APInt A = Pop(), B = Pop();
        if (!Fault)
          Push(B.isZero() ? zeroWord() : A.udiv(B));
        break;
      }
      case Opcode::SDIV: {
        llvm::APInt A = Pop(), B = Pop();
        if (!Fault) {
          if (B.isZero())
            Push(zeroWord());
          else if (A.isMinSignedValue() && B.isAllOnes())
            Push(A);
          else
            Push(A.sdiv(B));
        }
        break;
      }
      case Opcode::MOD: {
        llvm::APInt A = Pop(), B = Pop();
        if (!Fault)
          Push(B.isZero() ? zeroWord() : A.urem(B));
        break;
      }
      case Opcode::SMOD: {
        llvm::APInt A = Pop(), B = Pop();
        if (!Fault) {
          if (B.isZero() || (A.isMinSignedValue() && B.isAllOnes()))
            Push(zeroWord());
          else
            Push(A.srem(B));
        }
        break;
      }
      case Opcode::ADDMOD:
      case Opcode::MULMOD: {
        llvm::APInt A = Pop(), B = Pop(), Modulus = Pop();
        if (!Fault) {
          if (Modulus.isZero()) {
            Push(zeroWord());
          } else {
            llvm::APInt WideModulus = Modulus.zext(kWideWordBits);
            llvm::APInt Wide =
                Op == Opcode::ADDMOD
                    ? A.zext(kWideWordBits) + B.zext(kWideWordBits)
                    : A.zext(kWideWordBits) * B.zext(kWideWordBits);
            Push(Wide.urem(WideModulus).trunc(kWordBits));
          }
        }
        break;
      }
      case Opcode::EXP: {
        llvm::APInt Base = Pop(), Exponent = Pop();
        if (!Fault)
          Push(modularExponent(std::move(Base), std::move(Exponent)));
        break;
      }
      case Opcode::SIGNEXTEND: {
        llvm::APInt Index = Pop(), Value = Pop();
        if (!Fault)
          Push(signExtend(Index, Value));
        break;
      }
      case Opcode::LT:
      case Opcode::GT:
      case Opcode::SLT:
      case Opcode::SGT:
      case Opcode::EQ: {
        llvm::APInt A = Pop(), B = Pop();
        if (!Fault) {
          bool Value = Op == Opcode::LT    ? A.ult(B)
                       : Op == Opcode::GT  ? A.ugt(B)
                       : Op == Opcode::SLT ? A.slt(B)
                       : Op == Opcode::SGT ? A.sgt(B)
                                           : A == B;
          Push(boolWord(Value));
        }
        break;
      }
      case Opcode::ISZERO: {
        llvm::APInt A = Pop();
        if (!Fault)
          Push(boolWord(A.isZero()));
        break;
      }
      case Opcode::AND:
      case Opcode::OR:
      case Opcode::XOR: {
        llvm::APInt A = Pop(), B = Pop();
        if (!Fault)
          Push(Op == Opcode::AND ? A & B : Op == Opcode::OR ? A | B : A ^ B);
        break;
      }
      case Opcode::NOT: {
        llvm::APInt A = Pop();
        if (!Fault)
          Push(~A);
        break;
      }
      case Opcode::BYTE: {
        llvm::APInt Index = Pop(), Value = Pop();
        if (!Fault) {
          if (Index.uge(kWordBytes))
            Push(zeroWord());
          else
            Push(
                llvm::APInt(kWordBits,
                            Value.extractBitsAsZExtValue(
                                kBitsPerByte,
                                static_cast<unsigned>(
                                    (kWordBytes - 1 - Index.getZExtValue()) *
                                    kBitsPerByte))));
        }
        break;
      }
      case Opcode::SHL:
      case Opcode::SHR:
      case Opcode::SAR: {
        llvm::APInt Shift = Pop(), Value = Pop();
        if (!Fault) {
          if (Shift.uge(kWordBits))
            Push(Op == Opcode::SAR && Value.isNegative()
                     ? llvm::APInt::getAllOnes(kWordBits)
                     : zeroWord());
          else {
            unsigned Amount = static_cast<unsigned>(Shift.getZExtValue());
            Push(Op == Opcode::SHL   ? Value.shl(Amount)
                 : Op == Opcode::SHR ? Value.lshr(Amount)
                                     : Value.ashr(Amount));
          }
        }
        break;
      }
      case Opcode::CLZ: {
        llvm::APInt Value = Pop();
        if (!Fault)
          Push(llvm::APInt(kWordBits, Value.countl_zero()));
        break;
      }
      case Opcode::SHA3: {
        llvm::APInt OffsetWord = Pop(), SizeWord = Pop();
        size_t Offset = 0, Size = 0, End = 0;
        if (!Fault && Range(OffsetWord, SizeWord, Offset, Size, End)) {
          EnsureMemory(End);
          Push(keccak256(Result.Memory.data() + Offset, Size));
        }
        break;
      }
      case Opcode::ADDRESS:
        Push(Environment.Address);
        break;
      case Opcode::BALANCE: {
        llvm::APInt Address = Pop();
        if (!Fault)
          Push(mapLookup(Environment.Balances, Address));
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
              Op == Opcode::CALLDATACOPY
                  ? &Environment.Calldata
                  : Op == Opcode::CODECOPY ? &Program.Code
                                           : &Result.ReturnData;
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
        if (!Fault &&
            Range(DestinationWord, SizeWord, Destination, Size, End))
          CopyToMemory(codeLookup(Environment.ExternalCode, Address), Source,
                       Destination, Size);
        break;
      }
      case Opcode::RETURNDATASIZE:
        Push(llvm::APInt(kWordBits, Result.ReturnData.size()));
        break;
      case Opcode::EXTCODEHASH: {
        llvm::APInt Address = Pop();
        if (!Fault)
          Push(mapLookup(Environment.CodeHashes, Address));
        break;
      }
      case Opcode::BLOCKHASH: {
        llvm::APInt Number = Pop();
        if (!Fault)
          Push(mapLookup(Environment.BlockHashes, Number));
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
        Push(mapLookup(Environment.Balances, Environment.Address));
        break;
      case Opcode::BASEFEE:
        Push(Environment.BaseFee);
        break;
      case Opcode::BLOBHASH: {
        llvm::APInt Index = Pop();
        if (!Fault &&
            Index.getActiveBits() <= sizeof(size_t) * kBitsPerByte &&
            Index.getZExtValue() < Environment.BlobHashes.size())
          Push(Environment.BlobHashes[Index.getZExtValue()]);
        else if (!Fault)
          Push(zeroWord());
        break;
      }
      case Opcode::BLOBBASEFEE:
        Push(Environment.BlobBaseFee);
        break;
      case Opcode::POP:
        (void)Pop();
        break;
      case Opcode::MLOAD: {
        llvm::APInt OffsetWord = Pop();
        auto Offset = toSize(OffsetWord, Options.MaxMemoryBytes);
        if (!Offset || *Offset > Options.MaxMemoryBytes - kWordBytes)
          Fail("MLOAD offset exceeds configured memory limit");
        else if (!Fault) {
          EnsureMemory(*Offset + kWordBytes);
          Push(bytesToWord(Result.Memory, *Offset));
        }
        break;
      }
      case Opcode::MSTORE: {
        llvm::APInt OffsetWord = Pop(), Value = Pop();
        auto Offset = toSize(OffsetWord, Options.MaxMemoryBytes);
        if (!Offset || *Offset > Options.MaxMemoryBytes - kWordBytes)
          Fail("MSTORE offset exceeds configured memory limit");
        else if (!Fault) {
          EnsureMemory(*Offset + kWordBytes);
          wordToBytes(Value, Result.Memory.data() + *Offset);
        }
        break;
      }
      case Opcode::MSTORE8: {
        llvm::APInt OffsetWord = Pop(), Value = Pop();
        auto Offset = toSize(OffsetWord, Options.MaxMemoryBytes);
        if (!Offset || *Offset >= Options.MaxMemoryBytes)
          Fail("MSTORE8 offset exceeds configured memory limit");
        else if (!Fault) {
          EnsureMemory(*Offset + 1);
          Result.Memory[*Offset] =
              static_cast<uint8_t>(
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
                  sizeof(uint64_t) * kBitsPerByte ||
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
          Result.ReturnData.clear();
          Push(Environment.CreatedAddress);
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
          Result.ReturnData = Environment.CallReturnData;
          const size_t CopySize =
              std::min(OutputSize, Result.ReturnData.size());
          std::copy_n(Result.ReturnData.begin(), CopySize,
                      Result.Memory.begin() + OutputOffset);
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
          Result.ReturnData.assign(Result.Memory.begin() + Offset,
                                   Result.Memory.begin() + End);
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
          Result.SelfDestructBeneficiary = std::move(Beneficiary);
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
