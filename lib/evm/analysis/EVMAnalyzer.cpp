//===- EVMAnalyzer.cpp - Staged EVM bytecode analysis -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/analysis/EVMAnalyzer.h"

#include "EVMControlFlow.h"
#include "EVMHighAnalysis.h"
#include "EVMMedAnalysis.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <set>

namespace neverd::evm {
namespace {

std::string wordHexDigits(const llvm::APInt &Value, unsigned MinDigits = 1) {
  llvm::SmallString<kWordBytes * kHexDigitsPerByte> Digits;
  Value.toStringUnsigned(Digits, kHexRadix);
  std::string Result = Digits.str().str();
  if (Result.size() < MinDigits)
    Result.insert(Result.begin(), MinDigits - Result.size(), '0');
  return Result;
}

std::string wordHex(const llvm::APInt &Value, unsigned MinDigits = 1) {
  return "0x" + wordHexDigits(Value, MinDigits);
}

llvm::Error lowIRAnalysisError(uint64_t PC, llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      "evm: " + Message + " at pc 0x" + llvm::Twine(llvm::utohexstr(PC)),
      llvm::inconvertibleErrorCode());
}

} // namespace

llvm::Expected<EVMLowIR> decodeLowIR(llvm::ArrayRef<uint8_t> Code,
                                     AnalyzeOptions Options) {
  if (Options.MaxBlocks == 0)
    return lowIRAnalysisError(kEntryPC, "MaxBlocks must be greater than zero");
  auto Decoded = decodeBytecode(Code, Options);
  if (!Decoded)
    return Decoded.takeError();

  EVMLowIR Low;
  Low.Fork = Decoded->Fork;
  Low.Strict = Decoded->Strict;
  Low.Code = std::move(Decoded->Code);
  Low.Instructions = std::move(Decoded->Instructions);
  Low.JumpDestinations = std::move(Decoded->JumpDestinations);
  Low.Diagnostics = std::move(Decoded->Diagnostics);

  std::set<uint64_t> Starts;
  const auto InsertBlockStart = [&](uint64_t PC) -> llvm::Error {
    if (Starts.contains(PC))
      return llvm::Error::success();
    if (Starts.size() >= Options.MaxBlocks)
      return lowIRAnalysisError(PC, "basic block limit " +
                                        llvm::Twine(Options.MaxBlocks) +
                                        " exceeded");
    Starts.insert(PC);
    return llvm::Error::success();
  };
  if (llvm::Error Error = InsertBlockStart(kEntryPC))
    return std::move(Error);
  for (const auto &Instruction : Low.Instructions) {
    if (Instruction.is(Opcode::JUMPDEST)) {
      if (llvm::Error Error = InsertBlockStart(Instruction.PC))
        return std::move(Error);
    }
    if (Instruction.isTerminator() && Instruction.NextPC < Low.Code.size()) {
      if (llvm::Error Error = InsertBlockStart(Instruction.NextPC))
        return std::move(Error);
    }
  }

  llvm::DenseMap<uint64_t, size_t> InstructionIndex;
  for (size_t I = 0; I < Low.Instructions.size(); ++I)
    InstructionIndex[Low.Instructions[I].PC] = I;
  for (auto It = Starts.begin(); It != Starts.end(); ++It) {
    auto Found = InstructionIndex.find(*It);
    if (Found == InstructionIndex.end())
      continue;
    LowBlock Block;
    Block.StartPC = *It;
    Block.FirstInstruction = Found->second;
    auto Next = std::next(It);
    Block.EndPC = Next == Starts.end() ? Low.Code.size() : *Next;
    size_t EndIndex = Block.FirstInstruction;
    while (EndIndex < Low.Instructions.size() &&
           Low.Instructions[EndIndex].PC < Block.EndPC)
      ++EndIndex;
    Block.InstructionCount = EndIndex - Block.FirstInstruction;
    if (Low.Blocks.size() >= Options.MaxBlocks)
      return lowIRAnalysisError(
          Block.StartPC,
          "basic block limit " + llvm::Twine(Options.MaxBlocks) + " exceeded");
    Low.Blocks.push_back(std::move(Block));
  }

  if (llvm::Error E = analyzeControlFlow(Low, Options))
    return std::move(E);
  return Low;
}

llvm::Expected<EVMProgram> analyze(llvm::ArrayRef<uint8_t> Code,
                                   AnalyzeOptions Options) {
  auto Low = decodeLowIR(Code, Options);
  if (!Low)
    return Low.takeError();
  auto Med = detail::lowerCanonicalLowToMedIR(*Low, Options);
  if (!Med)
    return Med.takeError();
  EVMProgram Program;
  Program.Low = std::move(*Low);
  Program.Med = std::move(*Med);
  if (Options.RecoverHighLevel) {
    auto High =
        detail::recoverCanonicalHighIR(Program.Low, Program.Med, Options);
    if (!High)
      return High.takeError();
    Program.High = std::move(*High);
  }
  return Program;
}

std::string dumpLowIR(const EVMLowIR &Low) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << "evm.low hardfork=" << hardforkName(Low.Fork)
     << " strict=" << (Low.Strict ? "true" : "false") << "\n";
  for (const auto &Block : Low.Blocks) {
    OS << "block 0x" << llvm::utohexstr(Block.StartPC)
       << (Block.Reachable ? " reachable" : " unreachable") << "\n";
    for (size_t I = Block.FirstInstruction;
         I < Block.FirstInstruction + Block.InstructionCount; ++I) {
      const auto &Instruction = Low.Instructions[I];
      OS << "  0x" << llvm::utohexstr(Instruction.PC) << ": "
         << Instruction.Info.Name;
      if (const std::string Immediate = formatImmediate(Instruction);
          !Immediate.empty())
        OS << " " << Immediate;
      if (const std::string Annotation = formatDecodeAnnotation(Instruction);
          !Annotation.empty())
        OS << " ; " << Annotation;
      OS << "\n";
    }
    for (const auto &Edge : Block.Successors) {
      OS << "    -> ";
      if (Edge.Target)
        OS << "0x" << llvm::utohexstr(*Edge.Target);
      else
        OS << "indirect";
      OS << "\n";
    }
  }
  return Text;
}

std::string dumpMedIR(const EVMMedIR &Med) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << "evm.med word=i" << kWordBits << "\n";
  for (const auto &Block : Med.Blocks) {
    OS << "block 0x" << llvm::utohexstr(Block.StartPC) << "\n";
    for (const auto &Operation : Block.Operations) {
      for (ValueID Output : Operation.Outputs)
        OS << "  %" << Output << " = ";
      if (Operation.Outputs.empty())
        OS << "  ";
      OS << Operation.Name;
      for (ValueID Input : Operation.Inputs)
        OS << " %" << Input;
      bool HasAnnotation = false;
      const auto EmitAnnotation = [&](llvm::StringRef Annotation) {
        OS << (HasAnnotation ? ", " : " ; ") << Annotation;
        HasAnnotation = true;
      };
      if (Operation.Effect != EffectKind::None)
        EmitAnnotation(effectName(Operation.Effect));
      if (Operation.MemoryAccess != MemoryAccessKind::None)
        EmitAnnotation(memoryAccessName(Operation.MemoryAccess));
      if (Operation.StateAccess != StateAccessKind::None)
        EmitAnnotation(stateAccessName(Operation.StateAccess));
      if (Operation.CallValueAccess != CallValueAccessKind::None)
        EmitAnnotation(callValueAccessName(Operation.CallValueAccess));
      OS << "\n";
    }
  }
  return Text;
}

std::string dumpHighIR(const EVMHighIR &High) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  OS << "evm.high\n";
  for (KnownStandard Standard : High.Standards)
    OS << "standard " << getKnownStandardInfo(Standard).Name << "\n";
  for (const auto &Function : High.Functions) {
    OS << "function " << Function.Name << " selector "
       << wordHex(llvm::APInt(kSelectorBits, Function.Selector),
                  kSelectorHexDigits)
       << " entry 0x" << llvm::utohexstr(Function.EntryPC);
    if (Function.Known)
      OS << " signature " << Function.Known->Signature;
    if (Function.KnownVariant)
      OS << " standard "
         << getKnownStandardInfo(Function.KnownVariant->Standard).Name;
    OS << "\n";
    for (const auto &Argument : Function.Arguments)
      OS << "  argument " << Argument.Index << " " << Argument.Type << " "
         << abiTypeSourceName(Argument.TypeSource)
         << (Argument.Read ? "" : " unread") << "\n";
    for (const auto &Return : Function.Returns)
      OS << "  return " << Return << " "
         << abiTypeSourceName(Function.ReturnSource) << "\n";
  }
  for (const auto &Storage : High.Storage) {
    OS << (Storage.IsTransient ? "transient" : "storage") << " "
       << (Storage.IsWrite ? "write" : "read") << " "
       << storageKeyKindName(Storage.KeyKind) << " "
       << (Storage.Slot ? wordHex(*Storage.Slot) : "dynamic");
    if (Storage.Known)
      OS << " " << Storage.Known->Name;
    OS << "\n";
  }
  for (const auto &Proxy : High.Proxies) {
    OS << "delegate 0x" << llvm::utohexstr(Proxy.PC) << " "
       << calleeKindName(Proxy.Kind);
    if (Proxy.Known)
      OS << " " << Proxy.Known->Name;
    else if (Proxy.Slot)
      OS << " slot " << wordHex(*Proxy.Slot);
    if (Proxy.Implementation)
      OS << " implementation " << wordHex(*Proxy.Implementation);
    OS << "\n";
  }
  for (const auto &Call : High.Calls) {
    OS << "call 0x" << llvm::utohexstr(Call.PC) << " " << opcodeName(Call.Op)
       << " " << calleeKindName(Call.TargetKind) << " " << Call.SuggestedName;
    if (Call.Precompiled)
      OS << " precompile " << Call.Precompiled->Name;
    else if (Call.Target)
      OS << " target " << wordHex(*Call.Target);
    if (Call.NamedSlot)
      OS << " " << Call.NamedSlot->Name;
    else if (Call.Slot)
      OS << " slot " << wordHex(*Call.Slot);
    if (Call.Known)
      OS << " signature " << Call.Known->Signature;
    else if (Call.Selector)
      OS << " selector "
         << wordHex(llvm::APInt(kSelectorBits, *Call.Selector),
                    kSelectorHexDigits);
    if (Call.Value && !Call.Value->isZero())
      OS << " value " << wordHex(*Call.Value);
    OS << "\n";
  }
  for (const auto &Event : High.Events)
    OS << "event " << Event.SuggestedName << " topics=" << Event.Topics << "\n";
  for (const auto &Error : High.Errors) {
    OS << "error " << Error.SuggestedName << " " << revertKindName(Error.Kind);
    if (Error.Panic)
      OS << " " << Error.Panic->Name;
    OS << "\n";
  }
  return Text;
}

} // namespace neverd::evm
