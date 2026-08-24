//===- EVMDecoder.cpp - EVM bytecode decoder ----------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/bytecode/EVMDecoder.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"

namespace neverd::evm {
namespace {

llvm::Error decoderError(uint64_t PC, llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      "evm: " + Message + " at pc 0x" + llvm::Twine(llvm::utohexstr(PC)),
      llvm::inconvertibleErrorCode());
}

Diagnostic invalidImmediateDiagnostic(const LowInstruction &Instruction) {
  const auto Encoded =
      static_cast<uint8_t>(Instruction.Immediate.getZExtValue());
  return {Instruction.PC, "invalid immediate " + formatOpcodeByte(Encoded) +
                              " for " + std::string(Instruction.Info.Name)};
}

llvm::Error appendDiagnostic(std::vector<Diagnostic> &Diagnostics,
                             Diagnostic Entry, size_t &DiagnosticBytes,
                             const DecodeOptions &Options) {
  if (Diagnostics.size() >= Options.MaxLowDiagnostics)
    return decoderError(Entry.PC, "LowIR diagnostic limit " +
                                      llvm::Twine(Options.MaxLowDiagnostics) +
                                      " exceeded");
  if (DiagnosticBytes > Options.MaxLowDiagnosticBytes ||
      Entry.Message.size() > Options.MaxLowDiagnosticBytes - DiagnosticBytes)
    return decoderError(
        Entry.PC, "LowIR diagnostic byte limit " +
                      llvm::Twine(Options.MaxLowDiagnosticBytes) + " exceeded");
  DiagnosticBytes += Entry.Message.size();
  Diagnostics.push_back(std::move(Entry));
  return llvm::Error::success();
}

void decodePushData(LowInstruction &Instruction, llvm::ArrayRef<uint8_t> Code,
                    size_t &PC) {
  llvm::APInt Value(kWordBits, 0);
  Instruction.ImmediateStatus = ImmediateDecodeStatus::Complete;
  for (uint8_t I = 0; I < Instruction.Info.ImmediateBytes; ++I) {
    Value <<= kBitsPerByte;
    if (PC < Code.size()) {
      Value |= Code[PC];
      Instruction.Encoding.push_back(Code[PC++]);
    } else {
      Instruction.ImmediateStatus = ImmediateDecodeStatus::Truncated;
    }
  }
  Instruction.Immediate = std::move(Value);
}

bool decodeEIP8024Immediate(LowInstruction &Instruction, uint8_t Encoded) {
  switch (Instruction.Info.Immediate) {
  case ImmediateKind::EIP8024Single:
    if (const auto Depth = decodeEIP8024Single(Encoded)) {
      Instruction.StackOperands[0] = *Depth;
      Instruction.StackOperandCount = 1;
      return true;
    }
    return false;
  case ImmediateKind::EIP8024Pair:
    if (const auto Pair = decodeEIP8024Pair(Encoded)) {
      Instruction.StackOperands[0] = Pair->First;
      Instruction.StackOperands[1] = Pair->Second;
      Instruction.StackOperandCount = 2;
      return true;
    }
    return false;
  case ImmediateKind::None:
  case ImmediateKind::PushData:
    return false;
  }
  return false;
}

void decodeConditionalImmediate(LowInstruction &Instruction,
                                llvm::ArrayRef<uint8_t> Code, size_t &PC,
                                std::vector<Diagnostic> *Diagnostics) {
  const bool HasEncodedByte = PC < Code.size();
  const uint8_t Encoded = HasEncodedByte ? Code[PC] : uint8_t{0};
  Instruction.Immediate = llvm::APInt(kWordBits, Encoded);
  Instruction.ImmediateStatus = HasEncodedByte
                                    ? ImmediateDecodeStatus::Complete
                                    : ImmediateDecodeStatus::Truncated;

  if (decodeEIP8024Immediate(Instruction, Encoded)) {
    if (HasEncodedByte) {
      Instruction.Encoding.push_back(Encoded);
      ++PC;
    }
    return;
  }

  Instruction.ImmediateStatus = ImmediateDecodeStatus::Invalid;
  if (Diagnostics)
    Diagnostics->push_back(invalidImmediateDiagnostic(Instruction));
}

} // namespace

llvm::StringRef opcodeDecodeStatusName(OpcodeDecodeStatus Status) {
  switch (Status) {
#define EVM_OPCODE_DECODE_STATUS(NAME, SPELLING)                               \
  case OpcodeDecodeStatus::NAME:                                               \
    return llvm::StringLiteral(SPELLING);
#include "neverd/evm/bytecode/EVMDecodeStatuses.def"
  }
  return kUnknownName;
}

llvm::StringRef immediateDecodeStatusName(ImmediateDecodeStatus Status) {
  switch (Status) {
#define EVM_IMMEDIATE_DECODE_STATUS(NAME, SPELLING)                            \
  case ImmediateDecodeStatus::NAME:                                            \
    return llvm::StringLiteral(SPELLING);
#include "neverd/evm/bytecode/EVMDecodeStatuses.def"
  }
  return kUnknownName;
}

std::string formatOpcodeByte(uint8_t Byte) {
  return "0x" + llvm::utohexstr(Byte, /*LowerCase=*/true, kHexDigitsPerByte);
}

std::string formatImmediate(const LowInstruction &Instruction) {
  if (Instruction.ImmediateStatus == ImmediateDecodeStatus::None)
    return {};

  llvm::SmallString<kWordBytes * kHexDigitsPerByte> Digits;
  Instruction.Immediate.toStringUnsigned(Digits, kHexRadix);
  const size_t MinimumDigits =
      Instruction.Info.ImmediateBytes * kHexDigitsPerByte;
  std::string Result = Digits.str().str();
  if (Result.size() < MinimumDigits)
    Result.insert(Result.begin(), MinimumDigits - Result.size(), '0');
  return "0x" + Result;
}

std::string formatDecodeAnnotation(const LowInstruction &Instruction) {
  if (Instruction.DecodeStatus != OpcodeDecodeStatus::Active)
    return "opcode=" + opcodeDecodeStatusName(Instruction.DecodeStatus).str();
  if (Instruction.ImmediateStatus == ImmediateDecodeStatus::Invalid ||
      Instruction.ImmediateStatus == ImmediateDecodeStatus::Truncated)
    return "immediate=" +
           immediateDecodeStatusName(Instruction.ImmediateStatus).str();
  return {};
}

LowInstruction decodeInstructionAt(llvm::ArrayRef<uint8_t> Code, size_t PC,
                                   Hardfork Fork,
                                   std::vector<Diagnostic> *Diagnostics) {
  const size_t Start = PC;
  const uint8_t Byte = Code[PC++];
  const auto AssignedInfo = assignedOpcodeInfo(Byte);
  const auto ActiveInfo = opcodeInfo(Byte, Fork);
  const OpcodeDecodeStatus DecodeStatus =
      ActiveInfo     ? OpcodeDecodeStatus::Active
      : AssignedInfo ? OpcodeDecodeStatus::Inactive
                     : OpcodeDecodeStatus::Unknown;
  LowInstruction Instruction{
      Start, 0,
      ActiveInfo ? *ActiveInfo : AssignedInfo.value_or(unknownOpcodeInfo(Byte)),
      DecodeStatus};
  Instruction.Encoding.push_back(Byte);

  // An inactive or unassigned byte consumes nothing beyond itself. Reading the
  // immediate its record declares would skip bytes the fork under analysis
  // executes as instructions.
  if (ActiveInfo) {
    switch (Instruction.Info.Immediate) {
    case ImmediateKind::None:
      break;
    case ImmediateKind::PushData:
      decodePushData(Instruction, Code, PC);
      break;
    case ImmediateKind::EIP8024Single:
    case ImmediateKind::EIP8024Pair:
      decodeConditionalImmediate(Instruction, Code, PC, Diagnostics);
      break;
    }
  }

  Instruction.NextPC = PC;
  return Instruction;
}

llvm::Expected<DecodedBytecode> decodeBytecode(llvm::ArrayRef<uint8_t> Code,
                                               DecodeOptions Options) {
  if (!isValidHardfork(Options.Fork))
    return decoderError(kEntryPC, "invalid hardfork value");
#define EVM_ANALYSIS_LIMIT_DECODE(NAME, DEFAULT_VALUE)                         \
  if (Options.NAME == 0)                                                       \
    return decoderError(kEntryPC, #NAME " must be greater than zero");
#define EVM_ANALYSIS_LIMIT_CONTROL_FLOW(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_MEDIUM_IR(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT_HIGH_IR(NAME, DEFAULT_VALUE)
#define EVM_ANALYSIS_LIMIT(STAGE, NAME, DEFAULT_VALUE)                         \
  EVM_ANALYSIS_LIMIT_##STAGE(NAME, DEFAULT_VALUE)
#include "neverd/evm/analysis/EVMAnalysisLimits.def"
#undef EVM_ANALYSIS_LIMIT_DECODE
#undef EVM_ANALYSIS_LIMIT_CONTROL_FLOW
#undef EVM_ANALYSIS_LIMIT_MEDIUM_IR
#undef EVM_ANALYSIS_LIMIT_HIGH_IR
  if (Code.size() > Options.MaxCodeSize)
    return decoderError(kEntryPC, "bytecode exceeds configured size limit");

  DecodedBytecode Result;
  Result.Fork = Options.Fork;
  Result.Strict = Options.Strict;
  Result.Code.assign(Code.begin(), Code.end());
  size_t DiagnosticBytes = 0;

  for (size_t PC = 0; PC < Code.size();) {
    if (Result.Instructions.size() >= Options.MaxInstructions)
      return decoderError(PC, "instruction limit " +
                                  llvm::Twine(Options.MaxInstructions) +
                                  " exceeded");
    LowInstruction Instruction =
        decodeInstructionAt(Code, PC, Options.Fork, /*Diagnostics=*/nullptr);
    PC = Instruction.NextPC;

    if (Instruction.ImmediateStatus == ImmediateDecodeStatus::Invalid)
      if (llvm::Error Error = appendDiagnostic(
              Result.Diagnostics, invalidImmediateDiagnostic(Instruction),
              DiagnosticBytes, Options))
        return std::move(Error);

    if (!Instruction.isActive()) {
      const std::string Reason =
          Instruction.isAssigned() ? "inactive opcode " : "unknown opcode ";
      const std::string Spelled =
          formatOpcodeByte(Instruction.Encoding.front());
      if (llvm::Error Error = appendDiagnostic(
              Result.Diagnostics, {Instruction.PC, Reason + Spelled},
              DiagnosticBytes, Options))
        return std::move(Error);
    }

    if (Instruction.is(Opcode::JUMPDEST))
      Result.JumpDestinations.insert(Instruction.PC);
    Result.Instructions.push_back(std::move(Instruction));
  }

  return Result;
}

} // namespace neverd::evm
