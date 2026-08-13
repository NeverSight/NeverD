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

#include "neverd/sbf/image/SBFRelocations.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <optional>
#include <string>

namespace neverd::sbf {
namespace {

const RelocationEntry *findRelocation(const BinaryImage &Image, va_t Address) {
  for (const RelocationEntry &Relocation : Image.Relocations)
    if (Relocation.Address == Address)
      return &Relocation;
  return nullptr;
}

} // namespace

namespace analyzer_detail {

llvm::Error analysisError(size_t Slot, va_t Address, llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      (llvm::Twine("sbf: instruction ") + llvm::Twine(Slot) + " at 0x" +
       llvm::utohexstr(Address) + ": " + Message)
          .str(),
      llvm::inconvertibleErrorCode());
}

const Symbol *findFunctionSymbol(const BinaryImage &Image, va_t Address) {
  for (const Symbol &Symbol : Image.Symbols)
    if (Symbol.IsFunc && Symbol.Addr == Address)
      return &Symbol;
  return nullptr;
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

    if (!Instruction.Info) {
      if (llvm::Error Error = Context.report(
              Slot, llvm::Twine("unknown or version-inactive opcode 0x") +
                        llvm::utohexstr(Instruction.RawOpcode)))
        return Error;
      Low.Instructions.push_back(std::move(Instruction));
      continue;
    }

    if (Instruction.Info->ID == Opcode::LDDW) {
      if (Slot + 1 >= Count) {
        if (llvm::Error Error =
                Context.report(Slot, "LDDW is missing its continuation slot"))
          return Error;
      } else {
        const uint8_t *Continuation = Bytes + kInstructionSize;
        if (Continuation[kOpcodeOffset] != 0) {
          if (llvm::Error Error = Context.report(
                  Slot, "LDDW continuation has a non-zero opcode"))
            return Error;
        }
        const uint32_t High =
            llvm::support::endian::read32le(Continuation + kImmediateOffset);
        Instruction.Immediate = static_cast<uint64_t>(static_cast<uint32_t>(
                                    Instruction.RawImmediate)) |
                                (static_cast<uint64_t>(High) << kWordBitWidth);
        Instruction.SlotWidth = kLDDWSlotCount;
      }
    }

    if (Instruction.Src >= kRegisterCount) {
      if (llvm::Error Error = Context.report(
              Slot, llvm::Twine("source register is outside r0-r") +
                        llvm::Twine(kRegisterCount - 1)))
        return Error;
    }
    const bool Store = Instruction.Info->writesMemory();
    const bool ManualFrameBump =
        Instruction.Info->ID == Opcode::ADD64_IMM &&
        versionHasFeature(Low.TheVersion, VersionFeature::ManualStackFrames);
    if (Instruction.Dst >= kRegisterCount ||
        (Instruction.Dst == kFramePointerRegister && !Store &&
         !ManualFrameBump)) {
      if (llvm::Error Error = Context.report(
              Slot,
              Instruction.Dst == kFramePointerRegister
                  ? llvm::Twine("instruction cannot write frame pointer r") +
                        llvm::Twine(kFramePointerRegister)
                  : llvm::Twine("destination register is outside r0-r") +
                        llvm::Twine(kRegisterCount - 1)))
        return Error;
    }
    if (ManualFrameBump && Instruction.Dst == kFramePointerRegister &&
        Instruction.RawImmediate %
                static_cast<int32_t>(kDynamicStackFrameAlignment) !=
            0) {
      if (llvm::Error Error = Context.report(
              Slot, llvm::Twine("dynamic stack-frame adjustment is not ") +
                        llvm::Twine(kDynamicStackFrameAlignment) +
                        "-byte aligned"))
        return Error;
    }
    const SemanticTraits Traits =
        semanticTraits(*Instruction.Info, Low.TheVersion);
    if (Instruction.Info->Form == OperandForm::DstImm &&
        hasFaultPolicy(Traits.Faults, FaultPolicy::DivideByZero) &&
        Instruction.RawImmediate == 0) {
      if (llvm::Error Error =
              Context.report(Slot, "immediate division or remainder by zero"))
        return Error;
    }
    if ((Instruction.Info->Op == Operation::LSh ||
         Instruction.Info->Op == Operation::RSh ||
         Instruction.Info->Op == Operation::ARSh) &&
        Instruction.Info->Form == OperandForm::DstImm &&
        (Instruction.RawImmediate < 0 ||
         Instruction.RawImmediate >= Instruction.Info->Width)) {
      if (llvm::Error Error =
              Context.report(Slot, "immediate shift exceeds operand width"))
        return Error;
    }
    if ((Instruction.Info->Op == Operation::EndianLE ||
         Instruction.Info->Op == Operation::EndianBE) &&
        Instruction.RawImmediate != kHalfWordBitWidth &&
        Instruction.RawImmediate != kWordBitWidth &&
        Instruction.RawImmediate != kDoubleWordBitWidth) {
      if (llvm::Error Error = Context.report(
              Slot, llvm::Twine("endianness conversion width must be ") +
                        llvm::Twine(kHalfWordBitWidth) + ", " +
                        llvm::Twine(kWordBitWidth) + ", or " +
                        llvm::Twine(kDoubleWordBitWidth)))
        return Error;
    }

    if (Instruction.Info->ID == Opcode::CALL_REG) {
      const int64_t Register =
          callxRegisterIndex(Low.TheVersion, Instruction.Dst, Instruction.Src,
                             Instruction.RawImmediate);
      if (Register < 0 || Register >= kFramePointerRegister) {
        if (llvm::Error Error = Context.report(
                Slot, llvm::Twine("CALLX register must be in r0-r") +
                          llvm::Twine(kFramePointerRegister - 1)))
          return Error;
      }
      Instruction.Call = CallKind::Indirect;
      Instruction.CallRegister = static_cast<uint8_t>(Register);
    }

    Low.Instructions.push_back(std::move(Instruction));
    if (Low.Instructions.back().SlotWidth == kLDDWSlotCount) {
      ++Slot;
      const uint8_t *ContinuationBytes = Text.data() + Slot * kInstructionSize;
      LowInstruction Continuation;
      Continuation.Slot = Slot;
      Continuation.Address = Low.TextAddress + Slot * kInstructionSize;
      std::copy_n(ContinuationBytes, kInstructionSize,
                  Continuation.Encoding.begin());
      Continuation.RawOpcode = ContinuationBytes[kOpcodeOffset];
      Continuation.IsContinuation = true;
      Low.Instructions.push_back(std::move(Continuation));
    }
  }
  return llvm::Error::success();
}

llvm::Error resolveControlFlow(DecodeContext &Context) {
  LowIR &Low = Context.Program.Low;
  const size_t Count = Low.Instructions.size();
  for (LowInstruction &Instruction : Low.Instructions) {
    if (Instruction.IsContinuation || !Instruction.Info)
      continue;
    if (Instruction.Info->isBranch()) {
      const int64_t Target = static_cast<int64_t>(Instruction.Slot) + 1 +
                             static_cast<int64_t>(Instruction.Offset);
      if (Target < 0 || static_cast<uint64_t>(Target) >= Count) {
        if (llvm::Error Error = Context.report(
                Instruction.Slot, "branch target is outside program text"))
          return Error;
      } else if (Low.Instructions[static_cast<size_t>(Target)].IsContinuation) {
        if (llvm::Error Error = Context.report(
                Instruction.Slot, "branch targets an LDDW continuation"))
          return Error;
      } else {
        Instruction.BranchTarget = static_cast<size_t>(Target);
      }
    }

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
        if (Target < 0 || static_cast<uint64_t>(Target) >= Count ||
            Low.Instructions[static_cast<size_t>(Target)].IsContinuation) {
          if (llvm::Error Error = Context.report(
                  Instruction.Slot,
                  "static internal call target is outside program text"))
            return Error;
          Instruction.Call = CallKind::Unresolved;
        } else {
          Instruction.Call = CallKind::Internal;
          Instruction.CallTarget = static_cast<size_t>(Target);
          const va_t Address = Low.TextAddress + Target * kInstructionSize;
          if (const Symbol *Symbol = findFunctionSymbol(Context.Image, Address))
            Instruction.ResolvedName = Symbol->Name;
          else
            Instruction.ResolvedName = syntheticFunctionName(Address);
        }
      } else {
        if (llvm::Error Error = Context.report(
                Instruction.Slot,
                "static CALL source discriminator must be zero or one"))
          return Error;
        Instruction.Call = CallKind::Unresolved;
      }
      continue;
    }

    const RelocationEntry *Relocation =
        findRelocation(Context.Image, Instruction.Address);
    if (Relocation && !Relocation->SymbolName.empty()) {
      if (const Symbol *Symbol =
              Context.Image.findSymbol(Relocation->SymbolName)) {
        if (auto Target = addressToSlot(Context.Program.Image, Symbol->Addr)) {
          Instruction.Call = CallKind::Internal;
          Instruction.CallTarget = *Target;
          Instruction.ResolvedName = Symbol->Name;
          continue;
        }
      }
      Instruction.Call = CallKind::Syscall;
      Instruction.SyscallHash = hashSymbolName(Relocation->SymbolName);
      Instruction.Syscall = getSyscallInfo(Instruction.SyscallHash);
      Instruction.ResolvedName = Relocation->SymbolName;
      if (!Instruction.Syscall)
        if (llvm::Error Error = Context.report(
                Instruction.Slot,
                "legacy CALL relocation names an unaudited runtime syscall",
                DiagnosticSeverity::Warning))
          return Error;
      continue;
    }

    const uint32_t Hash = static_cast<uint32_t>(Instruction.RawImmediate);
    if (const SyscallInfo *Syscall = getSyscallInfo(Hash)) {
      Instruction.Call = CallKind::Syscall;
      Instruction.Syscall = Syscall;
      Instruction.SyscallHash = Hash;
      Instruction.ResolvedName = Syscall->Name.str();
      continue;
    }

    // A deployed legacy ELF may have had R_BPF_64_32 applied and its
    // relocation records stripped.  Rebuild the same function-registry keys
    // as the Anza loader so those calls remain recoverable from symbols.
    std::optional<size_t> HashedTarget;
    const Symbol *HashedSymbol = nullptr;
    bool HashCollision = false;
    for (const Symbol &Symbol : Context.Image.Symbols) {
      if (!Symbol.IsFunc)
        continue;
      const auto Target = addressToSlot(Context.Program.Image, Symbol.Addr);
      if (!Target || legacyFunctionKey(*Target, Symbol.Name) != Hash)
        continue;
      if (HashedTarget && *HashedTarget != *Target) {
        HashCollision = true;
        break;
      }
      HashedTarget = *Target;
      HashedSymbol = &Symbol;
    }
    if (HashCollision) {
      if (llvm::Error Error = Context.report(
              Instruction.Slot,
              "legacy CALL key collides between internal functions"))
        return Error;
      Instruction.Call = CallKind::Unresolved;
      Instruction.ResolvedName = kUnknownFunctionName.str();
      continue;
    }
    if (HashedTarget) {
      Instruction.Call = CallKind::Internal;
      Instruction.CallTarget = *HashedTarget;
      Instruction.ResolvedName =
          HashedSymbol ? HashedSymbol->Name : kUnknownFunctionName.str();
      continue;
    }

    if (Instruction.RawImmediate != -1) {
      const int64_t Target = static_cast<int64_t>(Instruction.Slot) + 1 +
                             static_cast<int64_t>(Instruction.RawImmediate);
      if (Target >= 0 && static_cast<uint64_t>(Target) < Count &&
          !Low.Instructions[static_cast<size_t>(Target)].IsContinuation) {
        Instruction.Call = CallKind::Internal;
        Instruction.CallTarget = static_cast<size_t>(Target);
        const va_t Address = Low.TextAddress + Target * kInstructionSize;
        if (const Symbol *Symbol = findFunctionSymbol(Context.Image, Address))
          Instruction.ResolvedName = Symbol->Name;
        else
          Instruction.ResolvedName = syntheticFunctionName(Address);
        continue;
      }
    }
    Instruction.Call = CallKind::Unresolved;
    Instruction.ResolvedName = kUnknownFunctionName.str();
    if (llvm::Error Error = Context.report(
            Instruction.Slot, "legacy CALL target cannot be resolved",
            DiagnosticSeverity::Warning))
      return Error;
  }
  return llvm::Error::success();
}

} // namespace analyzer_detail
} // namespace neverd::sbf
