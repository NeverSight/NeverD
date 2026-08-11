//===- Interpreter.cpp - Deterministic SBF semantic oracle --------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements direct execution of verified SBF instruction bytes for
/// differential backend testing.  Validates program layout and VM memory,
/// decodes raw opcodes, and interprets ALU, memory, control-flow, call, and
/// syscall semantics without consuming MedIR.
///
//===----------------------------------------------------------------------===//

#include "neverd/sbf/Interpreter.h"

#include "neverd/sbf/Semantics.h"

#include "llvm/ADT/bit.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <set>
#include <utility>

namespace neverd::sbf {
namespace {

//===----------------------------------------------------------------------===//
// Instruction decoding, validation, and semantic helpers
//===----------------------------------------------------------------------===//

struct RawInstruction {
  size_t Slot = 0;
  uint8_t RawOpcode = 0;
  uint8_t Dst = 0;
  uint8_t Src = 0;
  int16_t Offset = 0;
  int32_t Immediate = 0;
  const OpcodeInfo *Info = nullptr;
};

struct CallFrame {
  std::array<uint64_t, kCalleeSavedRegisterCount> SavedRegisters{};
  uint64_t FramePointer = 0;
  size_t ReturnSlot = 0;
};

int64_t signed64(uint64_t Value) { return std::bit_cast<int64_t>(Value); }
int32_t signed32(uint32_t Value) { return std::bit_cast<int32_t>(Value); }

uint32_t arithmeticShiftRight32(uint32_t Value, uint32_t Shift) {
  Shift &= kWordBitWidth - 1;
  if (Shift == 0)
    return Value;
  uint32_t Result = Value >> Shift;
  if ((Value & (uint32_t{1} << (kWordBitWidth - 1))) != 0)
    Result |= std::numeric_limits<uint32_t>::max() << (kWordBitWidth - Shift);
  return Result;
}

uint64_t arithmeticShiftRight64(uint64_t Value, uint64_t Shift) {
  Shift &= kDoubleWordBitWidth - 1;
  if (Shift == 0)
    return Value;
  uint64_t Result = Value >> Shift;
  if ((Value & (uint64_t{1} << (kDoubleWordBitWidth - 1))) != 0)
    Result |= std::numeric_limits<uint64_t>::max()
              << (kDoubleWordBitWidth - Shift);
  return Result;
}

uint64_t unsignedHighMultiply64(uint64_t Left, uint64_t Right) {
  const uint64_t LeftLow = static_cast<uint32_t>(Left);
  const uint64_t LeftHigh = Left >> kWordBitWidth;
  const uint64_t RightLow = static_cast<uint32_t>(Right);
  const uint64_t RightHigh = Right >> kWordBitWidth;
  const uint64_t LowProduct = LeftLow * RightLow;
  const uint64_t CrossProduct =
      LeftHigh * RightLow + (LowProduct >> kWordBitWidth);
  uint64_t Middle = static_cast<uint32_t>(CrossProduct);
  const uint64_t Carry = CrossProduct >> kWordBitWidth;
  Middle += LeftLow * RightHigh;
  return LeftHigh * RightHigh + Carry + (Middle >> kWordBitWidth);
}

uint64_t signedHighMultiply64(uint64_t Left, uint64_t Right) {
  return unsignedHighMultiply64(Left, Right) -
         ((Left >> (kDoubleWordBitWidth - 1)) != 0 ? Right : 0) -
         ((Right >> (kDoubleWordBitWidth - 1)) != 0 ? Left : 0);
}

RawInstruction decodeRaw(const SBFProgram &Program, size_t Slot) {
  const uint8_t *Bytes = Program.text().data() + Slot * kInstructionSize;
  RawInstruction Instruction;
  Instruction.Slot = Slot;
  Instruction.RawOpcode = Bytes[kOpcodeOffset];
  Instruction.Dst = Bytes[kRegisterByteOffset] & kRegisterEncodingMask;
  Instruction.Src = Bytes[kRegisterByteOffset] >> kRegisterEncodingBits;
  Instruction.Offset = static_cast<int16_t>(
      llvm::support::endian::read16le(Bytes + kBranchOffsetOffset));
  Instruction.Immediate = static_cast<int32_t>(
      llvm::support::endian::read32le(Bytes + kImmediateOffset));
  Instruction.Info =
      getOpcodeInfo(Instruction.RawOpcode, Program.Low.TheVersion);
  return Instruction;
}

llvm::Error validateProgram(const SBFProgram &Program,
                            const InterpreterOptions &Options) {
  if (llvm::Error Error = validateVMConfig(Program.Config))
    return Error;
  if (!isConcreteVersion(Program.Low.TheVersion))
    return llvm::make_error<llvm::StringError>(
        "sbf: raw interpreter requires a concrete SBF version",
        llvm::inconvertibleErrorCode());
  const llvm::ArrayRef<uint8_t> Text = Program.text();
  if (Text.empty() || Text.size() % kInstructionSize != 0)
    return llvm::make_error<llvm::StringError>(
        "sbf: raw interpreter requires non-empty, instruction-aligned text",
        llvm::inconvertibleErrorCode());
  if (Text.size() / kInstructionSize > kMaxInstructions)
    return llvm::make_error<llvm::StringError>(
        "sbf: raw interpreter program exceeds the instruction limit",
        llvm::inconvertibleErrorCode());
  if (Program.Low.EntrySlot >= Text.size() / kInstructionSize)
    return llvm::make_error<llvm::StringError>(
        "sbf: raw interpreter entry point is outside program text",
        llvm::inconvertibleErrorCode());
  if (Options.MaxCallDepth && *Options.MaxCallDepth == 0)
    return llvm::make_error<llvm::StringError>(
        "sbf: raw interpreter call-depth limit must be non-zero",
        llvm::inconvertibleErrorCode());
  if (Options.MaxCallDepth &&
      *Options.MaxCallDepth > Program.Config.MaxCallDepth)
    return llvm::make_error<llvm::StringError>(
        "sbf: raw interpreter call-depth limit exceeds the VM configuration",
        llvm::inconvertibleErrorCode());
  return llvm::Error::success();
}

bool rangeContains(const MemoryRegion &Region, uint64_t Address, size_t Size,
                   size_t &Offset) {
  if (Address < Region.Address)
    return false;
  const uint64_t Delta = Address - Region.Address;
  if (Delta > Region.Bytes.size() || Size > Region.Bytes.size() - Delta)
    return false;
  Offset = static_cast<size_t>(Delta);
  return true;
}

bool rangesOverlap(const MemoryRegion &Left, const MemoryRegion &Right) {
  if (Left.Bytes.empty() || Right.Bytes.empty())
    return false;
  const uint64_t LeftSize = Left.Bytes.size();
  const uint64_t RightSize = Right.Bytes.size();
  if (LeftSize > std::numeric_limits<uint64_t>::max() - Left.Address ||
      RightSize > std::numeric_limits<uint64_t>::max() - Right.Address)
    return true;
  return Left.Address < Right.Address + RightSize &&
         Right.Address < Left.Address + LeftSize;
}

llvm::Error validateMemory(const std::vector<MemoryRegion> &Memory,
                           Version TheVersion) {
  std::set<uint64_t> AlignedRegionIndices;
  for (size_t I = 0; I < Memory.size(); ++I) {
    if (Memory[I].Bytes.size() >
        std::numeric_limits<uint64_t>::max() - Memory[I].Address)
      return llvm::make_error<llvm::StringError>(
          "sbf: VM memory region address range overflows",
          llvm::inconvertibleErrorCode());
    if (versionHasFeature(TheVersion, VersionFeature::AlignedMemoryMapping)) {
      const uint64_t RegionIndex = Memory[I].Address >> kVirtualAddressBits;
      const uint64_t LastAddress =
          Memory[I].Bytes.empty()
              ? Memory[I].Address
              : Memory[I].Address + Memory[I].Bytes.size() - 1;
      if (LastAddress >> kVirtualAddressBits != RegionIndex)
        return llvm::make_error<llvm::StringError>(
            "sbf: v4 memory region crosses an aligned VM region boundary",
            llvm::inconvertibleErrorCode());
      if (!AlignedRegionIndices.insert(RegionIndex).second)
        return llvm::make_error<llvm::StringError>(
            "sbf: v4 memory mapping has multiple regions at one aligned index",
            llvm::inconvertibleErrorCode());
    }
    for (size_t J = I + 1; J < Memory.size(); ++J)
      if (rangesOverlap(Memory[I], Memory[J]))
        return llvm::make_error<llvm::StringError>(
            "sbf: VM memory regions overlap", llvm::inconvertibleErrorCode());
  }
  return llvm::Error::success();
}

void appendProgramMemory(const SBFProgram &Program,
                         std::vector<MemoryRegion> &Memory) {
  for (const ProgramRegion &Region : Program.ExecutableImage.regions())
    if (Region.DataVisible && !Region.Bytes.empty())
      Memory.push_back({Region.Address, Region.Bytes, false, Region.Name});

  if (usesStackFrameGaps(Program.Low.TheVersion, Program.Config)) {
    for (size_t Frame = 0; Frame < Program.Config.MaxCallDepth; ++Frame) {
      MemoryRegion Stack;
      Stack.Address = kStackStart + Frame * Program.Config.StackFrameSize *
                                        kStackFrameGapMultiplier;
      Stack.Bytes.resize(Program.Config.StackFrameSize);
      Stack.Writable = true;
      Stack.Name = "stack." + std::to_string(Frame);
      Memory.push_back(std::move(Stack));
    }
  } else {
    MemoryRegion Stack;
    Stack.Address = kStackStart;
    Stack.Bytes.resize(stackSize(Program.Config));
    Stack.Writable = true;
    Stack.Name = "stack";
    Memory.push_back(std::move(Stack));
  }
}

const LowInstruction *findAnalyzedInstruction(const SBFProgram &Program,
                                              size_t Slot) {
  if (Slot >= Program.Low.Instructions.size())
    return nullptr;
  const LowInstruction &Instruction = Program.Low.Instructions[Slot];
  return Instruction.Slot == Slot && !Instruction.IsContinuation ? &Instruction
                                                                 : nullptr;
}

} // namespace

//===----------------------------------------------------------------------===//
// executeRaw
//===----------------------------------------------------------------------===//

llvm::Expected<ExecutionResult> executeRaw(const SBFProgram &Program,
                                           ExecutionEnvironment Environment,
                                           InterpreterOptions Options) {
  if (llvm::Error Error = validateProgram(Program, Options))
    return std::move(Error);

  appendProgramMemory(Program, Environment.Memory);
  if (llvm::Error Error =
          validateMemory(Environment.Memory, Program.Low.TheVersion))
    return std::move(Error);

  ExecutionResult Result;
  std::array<uint64_t, kRegisterCount> Registers{};
  Registers[kFirstArgumentRegister] = Environment.Input;
  Registers[kFramePointerRegister] =
      initialFramePointer(Program.Low.TheVersion, Program.Config);
  std::vector<CallFrame> Frames;
  const size_t MaxCallDepth =
      Options.MaxCallDepth.value_or(Program.Config.MaxCallDepth);
  Frames.reserve(MaxCallDepth);
  const llvm::ArrayRef<uint8_t> Text = Program.text();
  const size_t InstructionCount = Text.size() / kInstructionSize;
  size_t PC = Program.Low.EntrySlot;

  auto Finish = [&] {
    Result.Registers = Registers;
    Result.ReturnValue = Registers[kReturnRegister];
    Result.Memory = std::move(Environment.Memory);
    return Result;
  };
  auto Fail = [&](FaultCode Code, llvm::Twine Message) {
    if (Result.Status == ExecutionStatus::Running) {
      Result.Status = ExecutionStatus::Faulted;
      Result.Fault = Code;
      Result.Error = Message.str();
      Result.FinalSlot = PC;
    }
  };
  auto Load = [&](uint64_t Address, unsigned Width, uint64_t &Value) {
    const size_t Size = Width / kBitsPerByte;
    for (MemoryRegion &Region : Environment.Memory) {
      size_t Offset = 0;
      if (!rangeContains(Region, Address, Size, Offset))
        continue;
      Value = 0;
      for (size_t I = 0; I < Size; ++I)
        Value |= static_cast<uint64_t>(Region.Bytes[Offset + I])
                 << (I * kBitsPerByte);
      return true;
    }
    Fail(FaultCode::MemoryAccess, "memory load is outside mapped VM regions");
    return false;
  };
  auto Store = [&](uint64_t Address, unsigned Width, uint64_t Value) {
    const size_t Size = Width / kBitsPerByte;
    for (MemoryRegion &Region : Environment.Memory) {
      size_t Offset = 0;
      if (!rangeContains(Region, Address, Size, Offset))
        continue;
      if (!Region.Writable) {
        Fail(FaultCode::MemoryAccess,
             "memory store targets a read-only VM region");
        return false;
      }
      for (size_t I = 0; I < Size; ++I)
        Region.Bytes[Offset + I] =
            static_cast<uint8_t>(Value >> (I * kBitsPerByte));
      return true;
    }
    Fail(FaultCode::MemoryAccess, "memory store is outside mapped VM regions");
    return false;
  };
  auto PushFrame = [&](size_t ReturnSlot) {
    // The current Anza interpreter increments depth and faults when it reaches
    // max_call_depth, so the terminal depth value is reserved for the fault.
    if (Frames.size() + 1 >= MaxCallDepth) {
      Fail(FaultCode::CallDepth, "maximum SBF call depth exceeded");
      return false;
    }
    CallFrame Frame;
    for (unsigned I = 0; I < kCalleeSavedRegisterCount; ++I)
      Frame.SavedRegisters[I] = Registers[kFirstCalleeSavedRegister + I];
    Frame.FramePointer = Registers[kFramePointerRegister];
    Frame.ReturnSlot = ReturnSlot;
    Frames.push_back(Frame);
    Result.MaxCallDepth = std::max(Result.MaxCallDepth, Frames.size());
    if (!versionHasFeature(Program.Low.TheVersion,
                           VersionFeature::ManualStackFrames)) {
      Registers[kFramePointerRegister] +=
          automaticFrameStride(Program.Low.TheVersion, Program.Config);
    }
    return true;
  };
  auto BranchTarget = [&](int64_t Target) -> std::optional<size_t> {
    if (Target < 0 || static_cast<uint64_t>(Target) >= InstructionCount) {
      Fail(FaultCode::InvalidBranch, "control-flow target is outside text");
      return std::nullopt;
    }
    const size_t Slot = static_cast<size_t>(Target);
    if (const LowInstruction *Low = findAnalyzedInstruction(Program, Slot);
        !Low) {
      Fail(FaultCode::InvalidBranch,
           "control-flow target is not a complete instruction");
      return std::nullopt;
    }
    return Slot;
  };

  while (Result.Status == ExecutionStatus::Running) {
    if (Result.Steps >= Options.MaxSteps) {
      Result.Status = ExecutionStatus::StepLimit;
      Result.Fault = FaultCode::ExecutionOverrun;
      Result.Error = "SBF execution step limit exceeded";
      Result.FinalSlot = PC;
      break;
    }
    if (PC >= InstructionCount) {
      Fail(FaultCode::ExecutionOverrun, "program counter is outside SBF text");
      break;
    }

    RawInstruction Instruction = decodeRaw(Program, PC);
    if (!Instruction.Info) {
      Fail(FaultCode::InvalidInstruction,
           "unknown or version-inactive SBF opcode");
      break;
    }
    if (Instruction.Dst >= kRegisterCount ||
        Instruction.Src >= kRegisterCount) {
      Fail(FaultCode::InvalidRegister,
           "SBF instruction references an invalid register");
      break;
    }
    if (Options.RecordTrace)
      Result.Trace.push_back(
          {PC, Program.Low.TextAddress + PC * kInstructionSize,
           Instruction.RawOpcode, Instruction.Info->ID, Frames.size()});
    ++Result.Steps;
    Result.FinalSlot = PC;
    size_t NextPC = PC + 1;
    const OpcodeInfo &Info = *Instruction.Info;
    const SemanticTraits Traits = semanticTraits(Info, Program.Low.TheVersion);
    const uint64_t Immediate = normalizeImmediate(
        static_cast<uint32_t>(Instruction.Immediate), Traits.Immediate);
    const uint64_t Source = Traits.Source == OperandSourceKind::SourceRegister
                                ? Registers[Instruction.Src]
                                : Immediate;

    if (Info.ID == Opcode::LDDW) {
      if (PC + 1 >= InstructionCount) {
        Fail(FaultCode::InvalidInstruction,
             "LDDW is missing its continuation slot");
        continue;
      }
      const uint8_t *Continuation = Text.data() + (PC + 1) * kInstructionSize;
      if (Continuation[kOpcodeOffset] != 0) {
        Fail(FaultCode::InvalidInstruction,
             "LDDW continuation has a non-zero opcode");
        continue;
      }
      const uint64_t High =
          llvm::support::endian::read32le(Continuation + kImmediateOffset);
      Registers[Instruction.Dst] =
          static_cast<uint32_t>(Instruction.Immediate) |
          (High << kWordBitWidth);
      NextPC = PC + kLDDWSlotCount;
    } else if (Info.Op == Operation::Load) {
      const uint64_t Address =
          Registers[Instruction.Src] +
          static_cast<uint64_t>(static_cast<int64_t>(Instruction.Offset));
      uint64_t Value = 0;
      if (Load(Address, Info.Width, Value))
        Registers[Instruction.Dst] = Value;
    } else if (Info.Op == Operation::Store) {
      const uint64_t Address =
          Registers[Instruction.Dst] +
          static_cast<uint64_t>(static_cast<int64_t>(Instruction.Offset));
      Store(Address, Info.Width, Source);
    } else if (Info.Op == Operation::Mov) {
      if (Info.Width == kWordBitWidth) {
        const uint32_t Value = static_cast<uint32_t>(Source);
        Registers[Instruction.Dst] = extendALU32Result(Value, Traits.Result);
      } else {
        Registers[Instruction.Dst] = Source;
      }
    } else if (Info.Op == Operation::Add || Info.Op == Operation::Sub ||
               Info.Op == Operation::Mul || Info.Op == Operation::Or ||
               Info.Op == Operation::And || Info.Op == Operation::Xor ||
               Info.Op == Operation::LSh || Info.Op == Operation::RSh ||
               Info.Op == Operation::ARSh || Info.Op == Operation::Neg) {
      if (Info.Width == kWordBitWidth) {
        const uint32_t Left = static_cast<uint32_t>(Registers[Instruction.Dst]);
        const uint32_t Right = static_cast<uint32_t>(Source);
        uint32_t Value = 0;
        switch (Info.Op) {
        case Operation::Add:
          Value = Left + Right;
          break;
        case Operation::Sub:
          Value = Traits.SwapOperands ? Right - Left : Left - Right;
          break;
        case Operation::Mul:
          Value = Left * Right;
          break;
        case Operation::Or:
          Value = Left | Right;
          break;
        case Operation::And:
          Value = Left & Right;
          break;
        case Operation::Xor:
          Value = Left ^ Right;
          break;
        case Operation::LSh:
          Value = Left << (Right & (kWordBitWidth - 1));
          break;
        case Operation::RSh:
          Value = Left >> (Right & (kWordBitWidth - 1));
          break;
        case Operation::ARSh:
          Value = arithmeticShiftRight32(Left, Right);
          break;
        case Operation::Neg:
          Value = uint32_t{0} - Left;
          break;
        default:
          llvm_unreachable("covered ALU32 operation");
        }
        Registers[Instruction.Dst] = extendALU32Result(Value, Traits.Result);
      } else {
        const uint64_t Left = Registers[Instruction.Dst];
        uint64_t Value = 0;
        switch (Info.Op) {
        case Operation::Add:
          Value = Left + Source;
          break;
        case Operation::Sub:
          Value = Traits.SwapOperands ? Source - Left : Left - Source;
          break;
        case Operation::Mul:
          Value = Left * Source;
          break;
        case Operation::Or:
          Value = Left | Source;
          break;
        case Operation::And:
          Value = Left & Source;
          break;
        case Operation::Xor:
          Value = Left ^ Source;
          break;
        case Operation::LSh:
          Value = Left << (Source & (kDoubleWordBitWidth - 1));
          break;
        case Operation::RSh:
          Value = Left >> (Source & (kDoubleWordBitWidth - 1));
          break;
        case Operation::ARSh:
          Value = arithmeticShiftRight64(Left, Source);
          break;
        case Operation::Neg:
          Value = uint64_t{0} - Left;
          break;
        default:
          llvm_unreachable("covered ALU64 operation");
        }
        Registers[Instruction.Dst] = Value;
      }
    } else if (Info.Op == Operation::UHighMul) {
      Registers[Instruction.Dst] =
          unsignedHighMultiply64(Registers[Instruction.Dst], Source);
    } else if (Info.Op == Operation::SHighMul) {
      Registers[Instruction.Dst] =
          signedHighMultiply64(Registers[Instruction.Dst], Source);
    } else if (Info.Op == Operation::UDiv || Info.Op == Operation::URem ||
               Info.Op == Operation::SDiv || Info.Op == Operation::SRem) {
      if (Info.Width == kWordBitWidth) {
        const uint32_t Left = static_cast<uint32_t>(Registers[Instruction.Dst]);
        const uint32_t Right = static_cast<uint32_t>(Source);
        if (Right == 0) {
          Fail(FaultCode::DivideByZero, "SBF division by zero");
        } else if (Info.Op == Operation::SDiv || Info.Op == Operation::SRem) {
          const int32_t SignedLeft = signed32(Left);
          const int32_t SignedRight = signed32(Right);
          if (SignedLeft == std::numeric_limits<int32_t>::min() &&
              SignedRight == -1) {
            Fail(FaultCode::DivideOverflow, "SBF signed division overflow");
          } else {
            const int32_t Value = Info.Op == Operation::SDiv
                                      ? SignedLeft / SignedRight
                                      : SignedLeft % SignedRight;
            Registers[Instruction.Dst] = static_cast<uint32_t>(Value);
          }
        } else {
          Registers[Instruction.Dst] =
              Info.Op == Operation::UDiv ? Left / Right : Left % Right;
        }
      } else {
        const uint64_t Left = Registers[Instruction.Dst];
        const uint64_t Right = Source;
        if (Right == 0) {
          Fail(FaultCode::DivideByZero, "SBF division by zero");
        } else if (Info.Op == Operation::SDiv || Info.Op == Operation::SRem) {
          const int64_t SignedLeft = signed64(Left);
          const int64_t SignedRight = signed64(Right);
          if (SignedLeft == std::numeric_limits<int64_t>::min() &&
              SignedRight == -1) {
            Fail(FaultCode::DivideOverflow, "SBF signed division overflow");
          } else {
            const int64_t Value = Info.Op == Operation::SDiv
                                      ? SignedLeft / SignedRight
                                      : SignedLeft % SignedRight;
            Registers[Instruction.Dst] = static_cast<uint64_t>(Value);
          }
        } else {
          Registers[Instruction.Dst] =
              Info.Op == Operation::UDiv ? Left / Right : Left % Right;
        }
      }
    } else if (Info.Op == Operation::EndianLE ||
               Info.Op == Operation::EndianBE) {
      uint64_t Value = Registers[Instruction.Dst];
      const bool Swap =
          (Info.Op == Operation::EndianBE &&
           llvm::endianness::native == llvm::endianness::little) ||
          (Info.Op == Operation::EndianLE &&
           llvm::endianness::native == llvm::endianness::big);
      switch (Instruction.Immediate) {
      case kHalfWordBitWidth: {
        uint16_t Narrow = static_cast<uint16_t>(Value);
        Registers[Instruction.Dst] = Swap ? llvm::byteswap(Narrow) : Narrow;
        break;
      }
      case kWordBitWidth: {
        uint32_t Narrow = static_cast<uint32_t>(Value);
        Registers[Instruction.Dst] = Swap ? llvm::byteswap(Narrow) : Narrow;
        break;
      }
      case kDoubleWordBitWidth:
        Registers[Instruction.Dst] = Swap ? llvm::byteswap(Value) : Value;
        break;
      default:
        Fail(FaultCode::InvalidInstruction,
             "invalid SBF endian-conversion width");
        break;
      }
    } else if (Info.Op == Operation::HighOr) {
      Registers[Instruction.Dst] |=
          static_cast<uint64_t>(static_cast<uint32_t>(Instruction.Immediate))
          << kWordBitWidth;
    } else if (Info.Op == Operation::Jump) {
      auto Target = BranchTarget(static_cast<int64_t>(PC) + 1 +
                                 static_cast<int64_t>(Instruction.Offset));
      if (Target)
        NextPC = *Target;
    } else if (Info.isConditionalBranch()) {
      const uint64_t Left64 = Registers[Instruction.Dst];
      const uint64_t Right64 = Source;
      bool Taken = false;
      if (Info.Width == kWordBitWidth) {
        const uint32_t Left = static_cast<uint32_t>(Left64);
        const uint32_t Right = static_cast<uint32_t>(Right64);
        switch (Info.Op) {
        case Operation::Eq:
          Taken = Left == Right;
          break;
        case Operation::Ne:
          Taken = Left != Right;
          break;
        case Operation::UGt:
          Taken = Left > Right;
          break;
        case Operation::UGe:
          Taken = Left >= Right;
          break;
        case Operation::ULt:
          Taken = Left < Right;
          break;
        case Operation::ULe:
          Taken = Left <= Right;
          break;
        case Operation::SGt:
          Taken = signed32(Left) > signed32(Right);
          break;
        case Operation::SGe:
          Taken = signed32(Left) >= signed32(Right);
          break;
        case Operation::SLt:
          Taken = signed32(Left) < signed32(Right);
          break;
        case Operation::SLe:
          Taken = signed32(Left) <= signed32(Right);
          break;
        case Operation::Set:
          Taken = (Left & Right) != 0;
          break;
        default:
          llvm_unreachable("covered JMP32 operation");
        }
      } else {
        switch (Info.Op) {
        case Operation::Eq:
          Taken = Left64 == Right64;
          break;
        case Operation::Ne:
          Taken = Left64 != Right64;
          break;
        case Operation::UGt:
          Taken = Left64 > Right64;
          break;
        case Operation::UGe:
          Taken = Left64 >= Right64;
          break;
        case Operation::ULt:
          Taken = Left64 < Right64;
          break;
        case Operation::ULe:
          Taken = Left64 <= Right64;
          break;
        case Operation::SGt:
          Taken = signed64(Left64) > signed64(Right64);
          break;
        case Operation::SGe:
          Taken = signed64(Left64) >= signed64(Right64);
          break;
        case Operation::SLt:
          Taken = signed64(Left64) < signed64(Right64);
          break;
        case Operation::SLe:
          Taken = signed64(Left64) <= signed64(Right64);
          break;
        case Operation::Set:
          Taken = (Left64 & Right64) != 0;
          break;
        default:
          llvm_unreachable("covered JMP64 operation");
        }
      }
      if (Taken) {
        auto Target = BranchTarget(static_cast<int64_t>(PC) + 1 +
                                   static_cast<int64_t>(Instruction.Offset));
        if (Target)
          NextPC = *Target;
      }
    } else if (Info.Op == Operation::Call) {
      const LowInstruction *Analyzed = findAnalyzedInstruction(Program, PC);
      CallKind Kind = Analyzed ? Analyzed->Call : CallKind::Unresolved;
      uint32_t Hash = static_cast<uint32_t>(Instruction.Immediate);
      std::optional<size_t> Target =
          Analyzed ? Analyzed->CallTarget : std::nullopt;
      if (versionHasFeature(Program.Low.TheVersion,
                            VersionFeature::StaticSyscalls)) {
        if (Instruction.Src == 0)
          Kind = CallKind::Syscall;
        else if (Instruction.Src == 1) {
          Kind = CallKind::Internal;
          const int64_t RawTarget = static_cast<int64_t>(PC) + 1 +
                                    static_cast<int64_t>(Instruction.Immediate);
          if (RawTarget >= 0)
            Target = static_cast<size_t>(RawTarget);
        }
      }
      if (Kind == CallKind::Syscall) {
        SyscallArguments Arguments{};
        for (unsigned I = 0; I < kArgumentRegisterCount; ++I)
          Arguments[I] = Registers[kFirstArgumentRegister + I];
        std::optional<uint64_t> Value =
            Environment.Syscall ? Environment.Syscall(Hash, Arguments)
                                : std::nullopt;
        if (!Value) {
          Fail(FaultCode::UnknownSyscall,
               "SBF syscall is not registered in the test environment");
        } else {
          Registers[kReturnRegister] = *Value;
          Result.Syscalls.push_back({PC, Hash, Arguments, *Value});
        }
      } else if (Kind == CallKind::Internal && Target) {
        auto ValidTarget = BranchTarget(static_cast<int64_t>(*Target));
        if (ValidTarget && PushFrame(NextPC))
          NextPC = *ValidTarget;
      } else {
        Fail(FaultCode::UnknownSyscall,
             "SBF immediate call target cannot be resolved");
      }
    } else if (Info.Op == Operation::CallX) {
      const int64_t Register =
          callxRegisterIndex(Program.Low.TheVersion, Instruction.Dst,
                             Instruction.Src, Instruction.Immediate);
      if (Register < 0 || Register >= kFramePointerRegister) {
        Fail(FaultCode::InvalidRegister,
             "SBF indirect call uses an invalid register");
      } else {
        const uint64_t TargetAddress =
            Registers[static_cast<unsigned>(Register)];
        const uint64_t TargetSlot =
            (TargetAddress - Program.Low.TextAddress) / kInstructionSize;
        // Upstream pushes the frame before checking the dynamic PC. This
        // determines fault precedence when both call depth and target are bad.
        if (PushFrame(NextPC)) {
          if (TargetSlot >= InstructionCount)
            Fail(FaultCode::UnknownIndirectCall,
                 "SBF indirect call target is outside program text");
          else
            // A target may be any in-range slot. Landing on an LDDW
            // continuation is observed as InvalidInstruction on the next step.
            NextPC = static_cast<size_t>(TargetSlot);
        }
      }
    } else if (Info.Op == Operation::Exit) {
      if (Frames.empty()) {
        Result.Status = ExecutionStatus::Returned;
        Result.ReturnValue = Registers[kReturnRegister];
        Result.FinalSlot = PC;
        break;
      }
      CallFrame Frame = Frames.back();
      Frames.pop_back();
      Registers[kFramePointerRegister] = Frame.FramePointer;
      for (unsigned I = 0; I < kCalleeSavedRegisterCount; ++I)
        Registers[kFirstCalleeSavedRegister + I] = Frame.SavedRegisters[I];
      NextPC = Frame.ReturnSlot;
    } else {
      Fail(FaultCode::InvalidInstruction,
           "unsupported operation in the raw SBF interpreter");
    }

    if (Result.Status == ExecutionStatus::Running)
      PC = NextPC;
  }
  return Finish();
}

} // namespace neverd::sbf
