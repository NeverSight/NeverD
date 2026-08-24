//===- SBFInterpreter.cpp - Deterministic SBF semantic oracle -------------===//
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

#include "neverd/sbf/runtime/SBFInterpreter.h"

#include "SBFInterpreterDetail.h"

#include "neverd/sbf/runtime/SBFSemantics.h"

#include "llvm/ADT/bit.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

namespace neverd::sbf {

using namespace interpreter_detail;

namespace {

constexpr llvm::StringLiteral kHostSyscallFaultMessage(
    "SBF host syscall callback reported an execution fault");

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
          validateMemory(Environment.Memory, Program.ExecutableImage.version(),
                         Program.Config))
    return std::move(Error);

  ExecutionResult Result;
  const Version TheVersion = Program.ExecutableImage.version();
  const RuntimeFeature RuntimeFeatures =
      Environment.RuntimeFeatures.value_or(Program.ActiveRuntimeFeatures);
  std::array<uint64_t, kRegisterCount> Registers{};
  Registers[kFirstArgumentRegister] = Environment.Input;
  Registers[kInstructionDataRegister] = Environment.InstructionData;
  Registers[kFramePointerRegister] =
      initialFramePointer(TheVersion, Program.Config);
  std::vector<CallFrame> Frames;
  const size_t MaxCallDepth =
      Options.MaxCallDepth.value_or(Program.Config.MaxCallDepth);
  Frames.reserve(MaxCallDepth);
  const llvm::ArrayRef<uint8_t> Text = Program.text();
  const size_t InstructionCount = Text.size() / kInstructionSize;
  size_t PC = Program.ExecutableImage.entrySlot();

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
    if (!versionHasFeature(TheVersion, VersionFeature::ManualStackFrames)) {
      Registers[kFramePointerRegister] +=
          automaticFrameStride(TheVersion, Program.Config);
    }
    return true;
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
    // Upstream meters and traces a successfully fetched raw slot before the
    // dynamic instruction check. This is observable when CALLX lands on an
    // LDDW continuation: the continuation fetch consumes one instruction,
    // while the top-of-loop meter and PC checks retain precedence.
    if (Options.RecordTrace)
      Result.Trace.push_back(
          {PC, Program.ExecutableImage.textAddress() + PC * kInstructionSize,
           Instruction.RawOpcode,
           Instruction.Info ? Instruction.Info->ID : Opcode::Unknown,
           Frames.size()});
    ++Result.Steps;
    Result.FinalSlot = PC;
    const validation_detail::InstructionValidation Validation =
        validateRawInstruction(Program, Instruction);
    if (!Validation.valid()) {
      const ValidationRuleInfo RuleInfo =
          getValidationRuleInfo(Validation.Rule);
      Fail(executionFaultForValidationRule(Validation.Rule), RuleInfo.Message);
      break;
    }
    size_t NextPC = PC + 1;
    const OpcodeInfo &Info = *Instruction.Info;
    const SemanticTraits Traits = semanticTraits(Info, TheVersion);
    const uint64_t Immediate = normalizeImmediate(
        static_cast<uint32_t>(Instruction.Immediate), Traits.Immediate);
    const uint64_t Source = Traits.Source == OperandSourceKind::SourceRegister
                                ? Registers[Instruction.Src]
                                : Immediate;

    if (Info.ID == Opcode::LDDW) {
      const uint8_t *Continuation = Text.data() + (PC + 1) * kInstructionSize;
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
      NextPC = *Validation.BranchTarget;
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
      if (Taken)
        NextPC = *Validation.BranchTarget;
    } else if (Info.Op == Operation::Call) {
      const uint32_t Hash = static_cast<uint32_t>(Instruction.Immediate);
      auto DispatchSyscall = [&] {
        SyscallArguments Arguments{};
        for (unsigned I = 0; I < kArgumentRegisterCount; ++I)
          Arguments[I] = Registers[kFirstArgumentRegister + I];
        SyscallOutcome Outcome = SyscallOutcome::unregistered();
        if (Environment.FeatureAwareSyscall) {
          Outcome = Environment.FeatureAwareSyscall(
              SyscallInvocation{Hash, Arguments, RuntimeFeatures});
        } else if (Environment.HostSyscall) {
          Outcome = Environment.HostSyscall(Hash, Arguments);
        } else if (Environment.Syscall) {
          std::optional<uint64_t> Value = Environment.Syscall(Hash, Arguments);
          Outcome = Value ? SyscallOutcome::returned(*Value)
                          : SyscallOutcome::unregistered();
        }
        if (Outcome.kind() == SyscallOutcome::Kind::Returned) {
          Registers[kReturnRegister] = Outcome.value();
          Result.Syscalls.push_back({PC, Hash, Arguments, Outcome.value()});
        }
        return Outcome;
      };

      const bool StaticSyscalls =
          versionHasFeature(TheVersion, VersionFeature::StaticSyscalls);
      if (StaticSyscalls && Instruction.Src == 0) {
        const SyscallOutcome Outcome = DispatchSyscall();
        if (Outcome.kind() == SyscallOutcome::Kind::Unregistered) {
          Fail(FaultCode::UnknownSyscall,
               "SBF syscall is not registered in the test environment");
        } else if (Outcome.kind() == SyscallOutcome::Kind::Fault) {
          Fail(Outcome.faultCode(), kHostSyscallFaultMessage);
        }
      } else if (StaticSyscalls && Instruction.Src == 1) {
        const int64_t Target = static_cast<int64_t>(PC) + 1 +
                               static_cast<int64_t>(Instruction.Immediate);
        if (Target < 0 || static_cast<uint64_t>(Target) >= InstructionCount) {
          Fail(FaultCode::InvalidInstruction,
               "unsupported static SBF immediate call");
        } else if (PushFrame(NextPC)) {
          // Any in-range slot is callable. If this lands on an LDDW
          // continuation, the next raw fetch reports the malformed opcode;
          // pushing first preserves the upstream call-depth precedence.
          NextPC = static_cast<size_t>(Target);
        }
      } else if (StaticSyscalls) {
        Fail(FaultCode::InvalidInstruction,
             "unsupported static SBF immediate call");
      } else {
        const SyscallOutcome Outcome = DispatchSyscall();
        if (Outcome.kind() == SyscallOutcome::Kind::Fault &&
            !Outcome.representsUnregisteredSyscall()) {
          Fail(Outcome.faultCode(), kHostSyscallFaultMessage);
        } else if (const ProgramFunctionEntry *Function =
                       Program.ExecutableImage.findFunction(Hash)) {
          if (PushFrame(NextPC))
            NextPC = Function->TargetSlot;
        } else if (Outcome.kind() != SyscallOutcome::Kind::Returned) {
          Fail(FaultCode::UnknownSyscall,
               "SBF immediate call target cannot be resolved");
        }
      }
    } else if (Info.Op == Operation::CallX) {
      const uint64_t TargetAddress = Registers[*Validation.CallXRegister];
      const uint64_t TargetSlot =
          (TargetAddress - Program.ExecutableImage.textAddress()) /
          kInstructionSize;
      // Upstream pushes the frame before checking the dynamic PC. This
      // determines fault precedence when both call depth and target are bad.
      if (PushFrame(NextPC)) {
        if (TargetSlot >= InstructionCount)
          Fail(FaultCode::UnknownIndirectCall,
               "SBF indirect call target is outside program text");
        else
          // A target may be any in-range slot. Landing on an LDDW continuation
          // is observed as InvalidInstruction on the next step.
          NextPC = static_cast<size_t>(TargetSlot);
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
