//===- SBFAnalyzerDecode.cpp - SBF text decoding and call resolution ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Decodes SBF program text into LowIR and resolves each branch, internal
/// call, and syscall against relocations, symbols, and the audited runtime
/// syscall table.
///
//===----------------------------------------------------------------------===//

#include "SBFAnalyzerDetail.h"
#include "SBFInstructionValidation.h"

#include "neverd/sbf/image/SBFRelocations.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <optional>
#include <string>

namespace neverd::sbf {
namespace {

constexpr llvm::StringLiteral kEntryInsideWideLoad =
    "entry point starts inside a wide load and cannot begin a function";
constexpr llvm::StringLiteral kSymbolInsideWideLoad =
    " starts inside a wide load and cannot begin a function";

llvm::Error quarantine(analyzer_detail::DecodeContext &Context,
                       LowInstruction &Instruction, ValidationRule Rule,
                       llvm::Twine Detail = {},
                       std::optional<size_t> DiagnosticSlot = std::nullopt) {
  Instruction.InvalidReason = Rule;
  const ValidationRuleInfo RuleInfo = getValidationRuleInfo(Rule);
  const llvm::Twine Message = Detail.isTriviallyEmpty()
                                  ? llvm::Twine(RuleInfo.Message)
                                  : llvm::Twine(RuleInfo.Message) + Detail;
  return Context.report(DiagnosticSlot.value_or(Instruction.Slot), Message,
                        DiagnosticSeverity::Error, Rule);
}

std::string
validationDetail(const LowInstruction &Instruction, Version TheVersion,
                 const validation_detail::InstructionValidation &Validation) {
  switch (Validation.Rule) {
  case ValidationRule::UnknownOpcode:
    return (llvm::Twine(" 0x") + llvm::utohexstr(Instruction.RawOpcode)).str();
  case ValidationRule::ImmediateShiftOutOfRange:
    return (llvm::Twine(" (") + llvm::Twine(Instruction.RawImmediate) +
            " for " + llvm::Twine(Instruction.Info->Width) + "-bit operand)")
        .str();
  case ValidationRule::InvalidEndianImmediate:
    return (llvm::Twine(" (") + llvm::Twine(Instruction.RawImmediate) + ")")
        .str();
  case ValidationRule::MisalignedFrameAdjustment:
    return (llvm::Twine(" (required alignment ") +
            llvm::Twine(kDynamicStackFrameAlignment) + ")")
        .str();
  case ValidationRule::InvalidCallXRegister:
    return (llvm::Twine(" (") +
            llvm::Twine(callxRegisterIndex(TheVersion, Instruction.Dst,
                                           Instruction.Src,
                                           Instruction.RawImmediate)) +
            ")")
        .str();
  case ValidationRule::InvalidSourceRegister:
    return (llvm::Twine(" (r") + llvm::Twine(Instruction.Src) + ")").str();
  case ValidationRule::FramePointerWrite:
  case ValidationRule::InvalidDestinationRegister:
    return (llvm::Twine(" (r") + llvm::Twine(Instruction.Dst) + ")").str();
  case ValidationRule::None:
  case ValidationRule::MissingLDDWContinuation:
  case ValidationRule::NonZeroLDDWContinuation:
  case ValidationRule::ImmediateDivisionByZero:
  case ValidationRule::BranchOutOfRange:
  case ValidationRule::BranchToLDDWContinuation:
    return {};
  }
  llvm_unreachable("unknown SBF validation rule");
}

void appendDecodedInstruction(LowIR &Low, llvm::ArrayRef<uint8_t> Text,
                              size_t &Slot, LowInstruction Instruction) {
  const bool HasContinuation = Instruction.SlotWidth == kLDDWSlotCount;
  Low.Instructions.push_back(std::move(Instruction));
  if (!HasContinuation)
    return;

  ++Slot;
  const uint8_t *Bytes = Text.data() + Slot * kInstructionSize;
  LowInstruction Continuation;
  Continuation.Slot = Slot;
  Continuation.Address = Low.TextAddress + Slot * kInstructionSize;
  std::copy_n(Bytes, kInstructionSize, Continuation.Encoding.begin());
  Continuation.RawOpcode = Bytes[kOpcodeOffset];
  Continuation.IsContinuation = true;
  Low.Instructions.push_back(std::move(Continuation));
}

} // namespace

namespace analyzer_detail {

DecodeContext::DecodeContext(const BinaryImage &Image,
                             const AnalyzeOptions &Options, SBFProgram &Program)
    : Image(Image), Options(Options), Program(Program) {
  const size_t FunctionCount =
      std::count_if(Image.Symbols.begin(), Image.Symbols.end(),
                    [](const Symbol &Symbol) { return Symbol.IsFunc; });
  FunctionSymbols.reserve(FunctionCount);
  for (const Symbol &Symbol : Image.Symbols)
    if (Symbol.IsFunc)
      FunctionSymbols.try_emplace(Symbol.Addr, &Symbol);

  const size_t CallRelocationCount = std::count_if(
      Image.Relocations.begin(), Image.Relocations.end(),
      [](const RelocationEntry &Entry) {
        return Entry.Type == static_cast<uint32_t>(Relocation::Call32);
      });
  CallRelocations.reserve(CallRelocationCount);
  for (const RelocationEntry &Entry : Image.Relocations)
    if (Entry.Type == static_cast<uint32_t>(Relocation::Call32))
      CallRelocations[Entry.Address] = &Entry;
}

const Symbol *DecodeContext::findFunctionSymbol(va_t Address) const {
  const auto It = FunctionSymbols.find(Address);
  return It == FunctionSymbols.end() ? nullptr : It->second;
}

const RelocationEntry *DecodeContext::findCallRelocation(va_t Address) const {
  const auto It = CallRelocations.find(Address);
  return It == CallRelocations.end() ? nullptr : It->second;
}

llvm::Error analysisError(size_t Slot, va_t Address, llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      (llvm::Twine("sbf: instruction ") + llvm::Twine(Slot) + " at 0x" +
       llvm::utohexstr(Address) + ": " + Message)
          .str(),
      llvm::inconvertibleErrorCode());
}

std::optional<size_t> addressToSlot(const Metadata &Metadata, va_t Address) {
  if (Address < Metadata.TextVM.Address ||
      Address - Metadata.TextVM.Address >= Metadata.TextVM.Size)
    return std::nullopt;
  const uint64_t Offset = Address - Metadata.TextVM.Address;
  if (Offset % kInstructionSize != 0)
    return std::nullopt;
  return static_cast<size_t>(Offset / kInstructionSize);
}

std::string syntheticFunctionName(va_t Address) {
  return (kAutoFuncPrefix + llvm::utohexstr(Address)).str();
}

llvm::Error decodeInstructions(DecodeContext &Context) {
  LowIR &Low = Context.Program.Low;
  const llvm::ArrayRef<uint8_t> Text = Context.Program.text();
  if (Text.empty())
    return Context.report(0, "program text is empty");
  if (Text.size() % kInstructionSize != 0)
    return Context.report(0,
                          llvm::Twine("program length is not a multiple of ") +
                              llvm::Twine(kInstructionSize) + " bytes");
  const size_t Count = Text.size() / kInstructionSize;
  if (Count > kMaxInstructions)
    return Context.report(0, "program exceeds the SBF instruction limit");

  Low.Instructions.reserve(Count);
  for (size_t Slot = 0; Slot < Count; ++Slot) {
    const uint8_t *Bytes = Text.data() + Slot * kInstructionSize;
    LowInstruction Instruction;
    Instruction.Slot = Slot;
    Instruction.Address = Low.TextAddress + Slot * kInstructionSize;
    std::copy_n(Bytes, kInstructionSize, Instruction.Encoding.begin());
    Instruction.RawOpcode = Bytes[kOpcodeOffset];
    const uint8_t Registers = Bytes[kRegisterByteOffset];
    Instruction.Dst = Registers & kRegisterEncodingMask;
    Instruction.Src = Registers >> kRegisterEncodingBits;
    Instruction.Offset = static_cast<int16_t>(
        llvm::support::endian::read16le(Bytes + kBranchOffsetOffset));
    Instruction.RawImmediate = static_cast<int32_t>(
        llvm::support::endian::read32le(Bytes + kImmediateOffset));
    Instruction.Immediate =
        static_cast<uint64_t>(static_cast<int64_t>(Instruction.RawImmediate));
    Instruction.Info = getOpcodeInfo(Instruction.RawOpcode, Low.TheVersion);
    const validation_detail::InstructionValidation Validation =
        validation_detail::validateInstruction(
            Text, Low.TheVersion,
            {Instruction.Slot, Instruction.RawOpcode, Instruction.Dst,
             Instruction.Src, Instruction.Offset, Instruction.RawImmediate,
             Instruction.Info});
    if (Validation.HasLDDWContinuation) {
      const uint8_t *Continuation = Bytes + kInstructionSize;
      const uint32_t High =
          llvm::support::endian::read32le(Continuation + kImmediateOffset);
      Instruction.Immediate = static_cast<uint64_t>(static_cast<uint32_t>(
                                  Instruction.RawImmediate)) |
                              (static_cast<uint64_t>(High) << kWordBitWidth);
      Instruction.SlotWidth = kLDDWSlotCount;
    }
    if (!Validation.valid()) {
      if (llvm::Error Error = quarantine(
              Context, Instruction, Validation.Rule,
              validationDetail(Instruction, Low.TheVersion, Validation),
              Validation.DiagnosticSlot))
        return Error;
      appendDecodedInstruction(Low, Text, Slot, std::move(Instruction));
      continue;
    }

    Instruction.BranchTarget = Validation.BranchTarget;
    if (Validation.CallXRegister) {
      Instruction.Call = CallKind::Indirect;
      Instruction.CallRegister = *Validation.CallXRegister;
    }
    appendDecodedInstruction(Low, Text, Slot, std::move(Instruction));
  }
  return llvm::Error::success();
}

llvm::Error resolveControlFlow(DecodeContext &Context) {
  LowIR &Low = Context.Program.Low;
  const size_t Count = Low.Instructions.size();
  for (LowInstruction &Instruction : Low.Instructions) {
    if (Instruction.IsContinuation || Instruction.isInvalid() ||
        !Instruction.Info)
      continue;
    if (Instruction.Info->ID != Opcode::CALL_IMM)
      continue;
    if (versionHasFeature(Low.TheVersion, VersionFeature::StaticSyscalls)) {
      if (Instruction.Src == 0) {
        Instruction.Call = CallKind::Syscall;
        Instruction.SyscallHash =
            static_cast<uint32_t>(Instruction.RawImmediate);
        Instruction.Syscall = getSyscallInfo(Instruction.SyscallHash);
        Instruction.ResolvedName = Instruction.Syscall
                                       ? Instruction.Syscall->Name.str()
                                       : kUnknownSyscallName.str();
        if (!Instruction.Syscall)
          if (llvm::Error Error = Context.report(
                  Instruction.Slot,
                  "static syscall hash is absent from the audited runtime ABI",
                  DiagnosticSeverity::Warning))
            return Error;
      } else if (Instruction.Src == 1) {
        const int64_t Target = static_cast<int64_t>(Instruction.Slot) + 1 +
                               static_cast<int64_t>(Instruction.RawImmediate);
        if (Target >= 0 && static_cast<uint64_t>(Target) < Count) {
          Instruction.Call = CallKind::Internal;
          Instruction.CallTarget = static_cast<size_t>(Target);
          const va_t Address = Low.TextAddress + Target * kInstructionSize;
          if (const Symbol *Symbol = Context.findFunctionSymbol(Address))
            Instruction.ResolvedName = Symbol->Name;
          else
            Instruction.ResolvedName = syntheticFunctionName(Address);
        } else {
          Instruction.Call = CallKind::Unsupported;
          Instruction.ResolvedName = kUnknownFunctionName.str();
          if (llvm::Error Error = Context.report(
                  Instruction.Slot,
                  "static internal call target is outside program text",
                  DiagnosticSeverity::Warning))
            return Error;
        }
      } else {
        Instruction.Call = CallKind::Unsupported;
        Instruction.ResolvedName = kUnknownFunctionName.str();
        if (llvm::Error Error = Context.report(
                Instruction.Slot,
                "static CALL source discriminator is unsupported at "
                "execution",
                DiagnosticSeverity::Warning))
          return Error;
      }
      continue;
    }

    const uint32_t Hash = static_cast<uint32_t>(Instruction.RawImmediate);
    Instruction.Dispatch = CallDispatchPolicy::LegacyRuntimeThenFunction;
    Instruction.SyscallHash = Hash;
    Instruction.Syscall = getSyscallInfo(Hash);
    if (const ProgramFunctionEntry *Function =
            Context.Program.ExecutableImage.findFunction(Hash)) {
      Instruction.Call = CallKind::Internal;
      Instruction.CallTarget = Function->TargetSlot;
      const va_t Address =
          Low.TextAddress + Function->TargetSlot * kInstructionSize;
      if (!Function->Name.empty())
        Instruction.ResolvedName = Function->Name;
      else if (const Symbol *Symbol = Context.findFunctionSymbol(Address))
        Instruction.ResolvedName = Symbol->Name;
      else
        Instruction.ResolvedName = syntheticFunctionName(Address);
      continue;
    }

    if (Instruction.Syscall) {
      Instruction.Call = CallKind::Syscall;
      Instruction.ResolvedName = Instruction.Syscall->Name.str();
      continue;
    }

    // Runtime identity is the already-relocated call key above.  Relocation
    // provenance is presentation-only here: consulting a global name-keyed
    // symbol table can turn an unrelated same-named debug symbol into an
    // executable target.
    const RelocationEntry *Relocation =
        Context.findCallRelocation(Instruction.Address);
    if (Relocation && Relocation->ELF && Relocation->ELF->Symbol &&
        Relocation->ELF->Symbol->Name) {
      Instruction.Call = CallKind::Syscall;
      Instruction.ResolvedName = *Relocation->ELF->Symbol->Name;
      if (llvm::Error Error = Context.report(
              Instruction.Slot,
              "legacy CALL relocation names an unaudited runtime syscall",
              DiagnosticSeverity::Warning))
        return Error;
      continue;
    }
    Instruction.Call = CallKind::Syscall;
    Instruction.ResolvedName = kUnknownFunctionName.str();
    if (llvm::Error Error = Context.report(
            Instruction.Slot,
            "legacy CALL key is absent from the audited syscall and function "
            "registries; preserving runtime registry dispatch",
            DiagnosticSeverity::Warning))
      return Error;
  }
  return llvm::Error::success();
}

void collectFunctionEntries(DecodeContext &Context) {
  const LowIR &Low = Context.Program.Low;
  Context.FunctionEntrySlots.resize(Low.Instructions.size());
  Context.FunctionEntrySlots.reset();

  auto IsCompleteEntry = [&](size_t Slot) {
    return Slot < Low.Instructions.size() &&
           !Low.Instructions[Slot].IsContinuation;
  };
  auto AddEntry = [&](size_t Slot) {
    if (IsCompleteEntry(Slot))
      Context.FunctionEntrySlots.set(Slot);
  };
  auto DiagnoseContinuation = [&](size_t Slot, llvm::Twine Message) {
    Context.Program.Low.Diagnostics.push_back(
        {DiagnosticSeverity::Warning, Slot,
         Low.TextAddress + Slot * kInstructionSize, Message.str()});
  };

  if (IsCompleteEntry(Low.EntrySlot))
    AddEntry(Low.EntrySlot);
  else if (Low.EntrySlot < Low.Instructions.size())
    DiagnoseContinuation(Low.EntrySlot, kEntryInsideWideLoad);

  llvm::BitVector DiagnosedCallTargets(Low.Instructions.size());
  for (const LowInstruction &Instruction : Low.Instructions) {
    if (!Instruction.CallTarget ||
        *Instruction.CallTarget >= Low.Instructions.size())
      continue;
    const size_t Target = *Instruction.CallTarget;
    if (IsCompleteEntry(Target)) {
      AddEntry(Target);
      continue;
    }
    if (!DiagnosedCallTargets.test(Target)) {
      DiagnoseContinuation(Target, kCallTargetInsideWideLoadDiagnostic);
      DiagnosedCallTargets.set(Target);
    }
  }

  for (const Symbol &Symbol : Context.Image.Symbols) {
    if (!Symbol.IsFunc)
      continue;
    const auto Slot = addressToSlot(Context.Program.Image, Symbol.Addr);
    if (!Slot || *Slot >= Low.Instructions.size())
      continue;
    if (IsCompleteEntry(*Slot)) {
      AddEntry(*Slot);
      continue;
    }
    DiagnoseContinuation(*Slot, llvm::Twine("function symbol '") + Symbol.Name +
                                    "'" + kSymbolInsideWideLoad);
  }
}

} // namespace analyzer_detail
} // namespace neverd::sbf
