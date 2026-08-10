//===- Decoder.cpp - EVM bytecode decoder -------------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/Decoder.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"

namespace neverd::evm {
namespace {

llvm::Error decoderError(uint64_t PC, llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      "evm: " + Message + " at pc 0x" + llvm::Twine(llvm::utohexstr(PC)),
      llvm::inconvertibleErrorCode());
}

std::string byteHex(uint8_t Byte) {
  return "0x" + llvm::utohexstr(Byte, /*LowerCase=*/true, kHexDigitsPerByte);
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
                                std::vector<Diagnostic> &Diagnostics) {
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
  Diagnostics.push_back(
      {Instruction.PC, "invalid immediate " + byteHex(Encoded) + " for " +
                           std::string(Instruction.Info.Name)});
}

} // namespace

llvm::StringRef opcodeDecodeStatusName(OpcodeDecodeStatus Status) {
  switch (Status) {
#define EVM_OPCODE_DECODE_STATUS(NAME, SPELLING)                               \
  case OpcodeDecodeStatus::NAME:                                               \
    return llvm::StringLiteral(SPELLING);
#include "neverd/evm/EVMDecodeStatuses.def"
  }
  return kUnknownName;
}

llvm::StringRef immediateDecodeStatusName(ImmediateDecodeStatus Status) {
  switch (Status) {
#define EVM_IMMEDIATE_DECODE_STATUS(NAME, SPELLING)                            \
  case ImmediateDecodeStatus::NAME:                                            \
    return llvm::StringLiteral(SPELLING);
#include "neverd/evm/EVMDecodeStatuses.def"
  }
  return kUnknownName;
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

llvm::Expected<DecodedBytecode> decodeBytecode(llvm::ArrayRef<uint8_t> Code,
                                               DecodeOptions Options) {
  if (!isValidHardfork(Options.Fork))
    return decoderError(kEntryPC, "invalid hardfork value");
  if (Code.empty())
    return decoderError(kEntryPC, "empty bytecode");
  if (Code.size() > Options.MaxCodeSize)
    return decoderError(kEntryPC, "bytecode exceeds configured size limit");

  DecodedBytecode Result;
  Result.Fork = Options.Fork;
  Result.Strict = Options.Strict;
  Result.Code.assign(Code.begin(), Code.end());

  for (size_t PC = 0; PC < Code.size();) {
    const size_t Start = PC;
    const uint8_t Byte = Code[PC++];
    const auto AssignedInfo = assignedOpcodeInfo(Byte);
    const auto ActiveInfo = opcodeInfo(Byte, Options.Fork);
    const OpcodeDecodeStatus DecodeStatus =
        ActiveInfo     ? OpcodeDecodeStatus::Active
        : AssignedInfo ? OpcodeDecodeStatus::Inactive
                       : OpcodeDecodeStatus::Unknown;
    LowInstruction Instruction{
        Start, 0,
        ActiveInfo ? *ActiveInfo
                   : AssignedInfo.value_or(unknownOpcodeInfo(Byte)),
        DecodeStatus};
    Instruction.Encoding.push_back(Byte);

    if (!ActiveInfo) {
      const std::string Reason =
          AssignedInfo ? "inactive opcode " : "unknown opcode ";
      if (Options.Strict)
        return decoderError(Start, Reason + llvm::Twine(byteHex(Byte)));
      Result.Diagnostics.push_back({Start, Reason + byteHex(Byte)});
    }

    if (ActiveInfo) {
      switch (Instruction.Info.Immediate) {
      case ImmediateKind::None:
        break;
      case ImmediateKind::PushData:
        decodePushData(Instruction, Code, PC);
        break;
      case ImmediateKind::EIP8024Single:
      case ImmediateKind::EIP8024Pair:
        decodeConditionalImmediate(Instruction, Code, PC, Result.Diagnostics);
        break;
      }
    }

    Instruction.NextPC = PC;
    if (Instruction.is(Opcode::JUMPDEST))
      Result.JumpDestinations.insert(Start);
    Result.Instructions.push_back(std::move(Instruction));
  }

  return Result;
}

} // namespace neverd::evm
