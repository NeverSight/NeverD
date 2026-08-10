//===- Analyzer.cpp - Solana SBF staged analysis ------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/Analyzer.h"

#include "neverd/sbf/Relocations.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <deque>
#include <limits>
#include <map>
#include <set>

namespace neverd::sbf {
namespace {

llvm::Error analysisError(size_t Slot, va_t Address, llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      (llvm::Twine("sbf: instruction ") + llvm::Twine(Slot) + " at 0x" +
       llvm::utohexstr(Address) + ": " + Message)
          .str(),
      llvm::inconvertibleErrorCode());
}

bool isStore(const OpcodeInfo &Info) { return Info.Op == Operation::Store; }

bool isALUResult(const OpcodeInfo &Info) {
  switch (Info.Op) {
  case Operation::LoadImm:
  case Operation::Load:
  case Operation::Add:
  case Operation::Sub:
  case Operation::Mul:
  case Operation::UHighMul:
  case Operation::SHighMul:
  case Operation::UDiv:
  case Operation::URem:
  case Operation::SDiv:
  case Operation::SRem:
  case Operation::Or:
  case Operation::And:
  case Operation::Xor:
  case Operation::LSh:
  case Operation::RSh:
  case Operation::ARSh:
  case Operation::Neg:
  case Operation::Mov:
  case Operation::EndianLE:
  case Operation::EndianBE:
  case Operation::HighOr:
    return true;
  default:
    return false;
  }
}

bool isImmediateDivision(const OpcodeInfo &Info) {
  if (Info.Form != OperandForm::DstImm)
    return false;
  return Info.Op == Operation::UDiv || Info.Op == Operation::URem ||
         Info.Op == Operation::SDiv || Info.Op == Operation::SRem;
}

size_t nextInstructionSlot(const LowInstruction &Instruction) {
  return Instruction.Slot + Instruction.SlotWidth;
}

const RelocationEntry *findRelocation(const BinaryImage &Image, va_t Address) {
  for (const RelocationEntry &Relocation : Image.Relocations)
    if (Relocation.Address == Address)
      return &Relocation;
  return nullptr;
}

llvm::Error relocationError(const RelocationEntry &Relocation,
                            llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      (llvm::Twine("sbf: relocation ") + llvm::Twine(Relocation.Type) +
       " at 0x" + llvm::utohexstr(Relocation.Address) + ": " + Message)
          .str(),
      llvm::inconvertibleErrorCode());
}

uint32_t legacyInternalCallHash(size_t TargetSlot, llvm::StringRef Name) {
  if (Name == kEntrySymbolName)
    return hashSymbolName(Name);
  std::array<uint8_t, sizeof(uint64_t)> Bytes{};
  llvm::support::endian::write64le(Bytes.data(), TargetSlot);
  return hashSymbolName(llvm::StringRef(
      reinterpret_cast<const char *>(Bytes.data()), Bytes.size()));
}

llvm::Error applyLegacyTextRelocations(const BinaryImage &Image,
                                       SBFProgram &Program) {
  const va_t TextAddress = Program.Image.TextVM.Address;
  const uint64_t TextSize = Program.Text.size();
  for (const RelocationEntry &Relocation : Image.Relocations) {
    const RelocationInfo *Info = getRelocationInfo(Relocation.Type);
    if (!Info)
      return relocationError(Relocation, "unsupported relocation type");

    if (Relocation.Address < TextAddress ||
        Relocation.Address - TextAddress >= TextSize)
      continue;
    const uint64_t TextOffset = Relocation.Address - TextAddress;
    if (TextOffset % kInstructionSize != 0)
      return relocationError(Relocation,
                             "text relocation is not instruction-aligned");
    if (TextSize - TextOffset < kInstructionSize)
      return relocationError(Relocation, "text relocation is out of bounds");

    if (Info->ID == Relocation::Call32) {
      const OpcodeInfo *Opcode =
          getOpcodeInfo(Program.Text[TextOffset], Program.Image.Version);
      if (!Opcode || Opcode->ID != Opcode::CALL_IMM)
        return relocationError(Relocation,
                               "R_BPF_64_32 does not reference a CALL");
      if (Relocation.SymbolName.empty())
        return relocationError(Relocation,
                               "R_BPF_64_32 has no resolvable symbol name");
      uint32_t Key = hashSymbolName(Relocation.SymbolName);
      if (const Symbol *Symbol = Image.findSymbol(Relocation.SymbolName)) {
        if (Symbol->IsFunc && Symbol->Addr >= TextAddress &&
            Symbol->Addr - TextAddress < TextSize &&
            (Symbol->Addr - TextAddress) % kInstructionSize == 0) {
          const size_t TargetSlot = static_cast<size_t>(
              (Symbol->Addr - TextAddress) / kInstructionSize);
          Key = legacyInternalCallHash(TargetSlot, Symbol->Name);
        }
      }
      llvm::support::endian::write32le(
          Program.Text.data() + TextOffset + kImmediateOffset, Key);
      continue;
    }

    if (TextSize - TextOffset < 2 * kInstructionSize)
      return relocationError(Relocation,
                             "64-bit text relocation is missing an LDDW slot");
    const OpcodeInfo *Opcode =
        getOpcodeInfo(Program.Text[TextOffset], Program.Image.Version);
    if (!Opcode || Opcode->ID != Opcode::LDDW ||
        Program.Text[TextOffset + kInstructionSize + kOpcodeOffset] != 0)
      return relocationError(Relocation,
                             "64-bit text relocation does not reference LDDW");

    const uint32_t Low = llvm::support::endian::read32le(
        Program.Text.data() + TextOffset + kImmediateOffset);
    const uint32_t High = llvm::support::endian::read32le(
        Program.Text.data() + TextOffset + kInstructionSize + kImmediateOffset);
    uint64_t Value =
        static_cast<uint64_t>(Low) | (static_cast<uint64_t>(High) << 32);

    if (Info->ID == Relocation::Abs64) {
      const Symbol *Symbol = Image.findSymbol(Relocation.SymbolName);
      if (!Symbol)
        return relocationError(Relocation,
                               "R_BPF_64_64 has no resolvable symbol");
      const uint64_t RawSymbol = Symbol->Addr >= kBytecodeStart
                                     ? Symbol->Addr - kBytecodeStart
                                     : Symbol->Addr;
      if (RawSymbol > std::numeric_limits<uint64_t>::max() - Low)
        return relocationError(Relocation, "relocated address overflows");
      Value = RawSymbol + Low;
      if (Value < kMemoryRegionSize) {
        if (Value > std::numeric_limits<uint64_t>::max() - kBytecodeStart)
          return relocationError(Relocation, "relocated address overflows");
        Value += kBytecodeStart;
      }
    } else if (Info->ID == Relocation::Relative64) {
      if (Value == 0)
        return relocationError(Relocation,
                               "R_BPF_64_RELATIVE has a zero value");
      if (Value < kMemoryRegionSize) {
        if (Value > std::numeric_limits<uint64_t>::max() - kBytecodeStart)
          return relocationError(Relocation, "relative address overflows");
        Value += kBytecodeStart;
      }
    } else {
      return relocationError(Relocation,
                             "relocation cannot be applied to program text");
    }

    llvm::support::endian::write32le(Program.Text.data() + TextOffset +
                                         kImmediateOffset,
                                     static_cast<uint32_t>(Value));
    llvm::support::endian::write32le(Program.Text.data() + TextOffset +
                                         kInstructionSize + kImmediateOffset,
                                     static_cast<uint32_t>(Value >> 32));
  }
  return llvm::Error::success();
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

struct DecodeContext {
  const BinaryImage &Image;
  const AnalyzeOptions &Options;
  SBFProgram &Program;

  llvm::Error report(size_t Slot, llvm::Twine Message,
                     DiagnosticSeverity Severity = DiagnosticSeverity::Error) {
    const va_t Address = Program.Image.TextVM.Address + Slot * kInstructionSize;
    Program.Low.Diagnostics.push_back({Severity, Slot, Address, Message.str()});
    if (Options.Strict && Severity == DiagnosticSeverity::Error)
      return analysisError(Slot, Address, Message);
    return llvm::Error::success();
  }
};

llvm::Error decodeInstructions(DecodeContext &Context) {
  LowIR &Low = Context.Program.Low;
  const std::vector<uint8_t> &Text = Context.Program.Text;
  if (Text.empty())
    return Context.report(0, "program text is empty");
  if (Text.size() % kInstructionSize != 0)
    return Context.report(0, "program length is not a multiple of 8 bytes");
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
                                (static_cast<uint64_t>(High) << 32);
        Instruction.SlotWidth = 2;
      }
    }

    if (Instruction.Src >= kRegisterCount) {
      if (llvm::Error Error =
              Context.report(Slot, "source register is outside r0-r10"))
        return Error;
    }
    const bool Store = isStore(*Instruction.Info);
    const bool ManualFrameBump =
        Instruction.Info->ID == Opcode::ADD64_IMM &&
        versionHasFeature(Low.TheVersion, VersionFeature::ManualStackFrames);
    if (Instruction.Dst >= kRegisterCount ||
        (Instruction.Dst == kFramePointerRegister && !Store &&
         !ManualFrameBump)) {
      if (llvm::Error Error = Context.report(
              Slot, Instruction.Dst == kFramePointerRegister
                        ? "instruction cannot write frame pointer r10"
                        : "destination register is outside r0-r10"))
        return Error;
    }
    if (ManualFrameBump && Instruction.Dst == kFramePointerRegister &&
        Instruction.RawImmediate %
                static_cast<int32_t>(kDynamicStackFrameAlignment) !=
            0) {
      if (llvm::Error Error = Context.report(
              Slot, "dynamic stack-frame adjustment is not 64-byte aligned"))
        return Error;
    }
    if (isImmediateDivision(*Instruction.Info) &&
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
        Instruction.RawImmediate != 16 && Instruction.RawImmediate != 32 &&
        Instruction.RawImmediate != 64) {
      if (llvm::Error Error = Context.report(
              Slot, "endianness conversion width must be 16, 32, or 64"))
        return Error;
    }

    if (Instruction.Info->ID == Opcode::CALL_REG) {
      int64_t Register = Instruction.RawImmediate;
      if (versionHasFeature(Low.TheVersion, VersionFeature::CallXSource))
        Register = Instruction.Src;
      else if (versionHasFeature(Low.TheVersion,
                                 VersionFeature::CallXDestination))
        Register = Instruction.Dst;
      if (Register < 0 || Register >= kFramePointerRegister) {
        if (llvm::Error Error =
                Context.report(Slot, "CALLX register must be in r0-r9"))
          return Error;
      }
      Instruction.Call = CallKind::Indirect;
      Instruction.CallRegister = static_cast<uint8_t>(Register);
    }

    Low.Instructions.push_back(std::move(Instruction));
    if (Low.Instructions.back().SlotWidth == 2) {
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
      if (!Target || legacyInternalCallHash(*Target, Symbol.Name) != Hash)
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

void addUnique(std::vector<size_t> &Values, size_t Value) {
  if (std::find(Values.begin(), Values.end(), Value) == Values.end())
    Values.push_back(Value);
}

void buildCFG(LowIR &Low) {
  std::set<size_t> Leaders{0, Low.EntrySlot};
  for (const LowInstruction &Instruction : Low.Instructions) {
    if (Instruction.IsContinuation || !Instruction.Info)
      continue;
    if (Instruction.BranchTarget)
      Leaders.insert(*Instruction.BranchTarget);
    if (Instruction.CallTarget)
      Leaders.insert(*Instruction.CallTarget);
    if (Instruction.Info->isBranch() || Instruction.Info->isCall() ||
        Instruction.Info->isExit()) {
      const size_t Next = nextInstructionSlot(Instruction);
      if (Next < Low.Instructions.size())
        Leaders.insert(Next);
    }
  }

  std::vector<size_t> Ordered(Leaders.begin(), Leaders.end());
  Ordered.erase(std::remove_if(Ordered.begin(), Ordered.end(),
                               [&](size_t Slot) {
                                 return Slot >= Low.Instructions.size() ||
                                        Low.Instructions[Slot].IsContinuation;
                               }),
                Ordered.end());
  std::map<size_t, size_t> SlotToBlock;
  for (size_t I = 0; I < Ordered.size(); ++I) {
    BasicBlock Block;
    Block.ID = I;
    Block.StartSlot = Ordered[I];
    Block.EndSlot =
        I + 1 < Ordered.size() ? Ordered[I + 1] : Low.Instructions.size();
    Low.Blocks.push_back(std::move(Block));
    for (size_t Slot = Low.Blocks.back().StartSlot;
         Slot < Low.Blocks.back().EndSlot; ++Slot)
      SlotToBlock[Slot] = I;
  }

  auto AddEdge = [&](size_t From, std::optional<size_t> To, EdgeKind Kind) {
    Low.Edges.push_back({From, To, Kind});
    if (!To)
      return;
    addUnique(Low.Blocks[From].Successors, *To);
    addUnique(Low.Blocks[*To].Predecessors, From);
  };

  for (BasicBlock &Block : Low.Blocks) {
    const LowInstruction *Last = nullptr;
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      if (!Low.Instructions[Slot].IsContinuation)
        Last = &Low.Instructions[Slot];
    if (!Last || !Last->Info) {
      if (Block.ID + 1 < Low.Blocks.size())
        AddEdge(Block.ID, Block.ID + 1, EdgeKind::Fallthrough);
      continue;
    }
    auto TargetBlock =
        [&](std::optional<size_t> Slot) -> std::optional<size_t> {
      if (!Slot)
        return std::nullopt;
      auto It = SlotToBlock.find(*Slot);
      return It == SlotToBlock.end() ? std::nullopt
                                     : std::optional<size_t>(It->second);
    };
    if (Last->Info->Op == Operation::Jump) {
      AddEdge(Block.ID, TargetBlock(Last->BranchTarget), EdgeKind::Branch);
    } else if (Last->Info->isConditionalBranch()) {
      AddEdge(Block.ID, TargetBlock(Last->BranchTarget), EdgeKind::BranchTaken);
      if (Block.ID + 1 < Low.Blocks.size())
        AddEdge(Block.ID, Block.ID + 1, EdgeKind::Fallthrough);
    } else if (Last->Info->isCall()) {
      if (Last->Call == CallKind::Internal)
        AddEdge(Block.ID, TargetBlock(Last->CallTarget), EdgeKind::Call);
      else if (Last->Call == CallKind::Indirect)
        AddEdge(Block.ID, std::nullopt, EdgeKind::IndirectCall);
      if (Block.ID + 1 < Low.Blocks.size())
        AddEdge(Block.ID, Block.ID + 1, EdgeKind::Fallthrough);
    } else if (Last->Info->isExit()) {
      AddEdge(Block.ID, std::nullopt, EdgeKind::Return);
    } else if (Block.ID + 1 < Low.Blocks.size()) {
      AddEdge(Block.ID, Block.ID + 1, EdgeKind::Fallthrough);
    }
  }

  const auto EntryIt = SlotToBlock.find(Low.EntrySlot);
  if (EntryIt != SlotToBlock.end()) {
    std::deque<size_t> Work{EntryIt->second};
    while (!Work.empty()) {
      const size_t ID = Work.front();
      Work.pop_front();
      if (Low.Blocks[ID].Reachable)
        continue;
      Low.Blocks[ID].Reachable = true;
      for (size_t Successor : Low.Blocks[ID].Successors)
        Work.push_back(Successor);
    }
  }
}

ResultExtension resultExtension(const LowInstruction &Instruction,
                                Version Version) {
  if (!Instruction.Info || Instruction.Info->Width != 32 ||
      !isALUResult(*Instruction.Info))
    return ResultExtension::None;
  if (Instruction.Info->ID == Opcode::MOV32_REG &&
      versionHasFeature(Version, VersionFeature::ExplicitSignExtension))
    return ResultExtension::Sign32;
  if ((Instruction.Info->Op == Operation::Add ||
       Instruction.Info->Op == Operation::Sub ||
       Instruction.Info->Op == Operation::Mul) &&
      !versionHasFeature(Version, VersionFeature::ExplicitSignExtension))
    return ResultExtension::Sign32;
  return ResultExtension::Zero32;
}

ImmediateExtension immediateExtension(const LowInstruction &Instruction,
                                      Version Version) {
  if (!Instruction.Info)
    return ImmediateExtension::Sign32;
  if (Instruction.Info->ID == Opcode::LDDW)
    return ImmediateExtension::Full64;
  if (versionHasFeature(Version, VersionFeature::PQR) &&
      (Instruction.Info->Op == Operation::UHighMul ||
       Instruction.Info->Op == Operation::UDiv ||
       Instruction.Info->Op == Operation::URem ||
       Instruction.Info->Op == Operation::HighOr))
    return ImmediateExtension::Zero32;
  return ImmediateExtension::Sign32;
}

void buildMedIR(SBFProgram &Program) {
  Program.Med.TheVersion = Program.Low.TheVersion;
  for (const LowInstruction &Instruction : Program.Low.Instructions) {
    if (Instruction.IsContinuation || !Instruction.Info)
      continue;
    MedInstruction Med;
    Med.Slot = Instruction.Slot;
    Med.Address = Instruction.Address;
    Med.SourceOpcode = Instruction.Info->ID;
    Med.Op = Instruction.Info->Op;
    Med.Form = Instruction.Info->Form;
    Med.Width = Instruction.Info->Width;
    Med.Dst = Instruction.Dst;
    Med.Src = Instruction.Src;
    Med.Offset = Instruction.Offset;
    Med.Immediate = Instruction.Immediate;
    Med.ImmediateMode = immediateExtension(Instruction, Program.Low.TheVersion);
    Med.Extension = resultExtension(Instruction, Program.Low.TheVersion);
    Med.SwapOperands = Instruction.Info->Op == Operation::Sub &&
                       Instruction.Info->Form == OperandForm::DstImm &&
                       versionHasFeature(Program.Low.TheVersion,
                                         VersionFeature::SwapSubImmediate);
    Med.BranchTarget = Instruction.BranchTarget;
    Med.Call = Instruction.Call;
    Med.CallTarget = Instruction.CallTarget;
    Med.SyscallHash = Instruction.SyscallHash;
    Med.Syscall = Instruction.Syscall;
    if (Instruction.Info->ID == Opcode::CALL_REG) {
      if (versionHasFeature(Program.Low.TheVersion,
                            VersionFeature::CallXSource))
        Med.CallRegister = Instruction.CallRegister;
      else if (versionHasFeature(Program.Low.TheVersion,
                                 VersionFeature::CallXDestination))
        Med.CallRegister = Instruction.CallRegister;
      else
        Med.CallRegister = Instruction.CallRegister;
    }
    Program.Med.Instructions.push_back(std::move(Med));
  }
  for (const BasicBlock &Block : Program.Low.Blocks) {
    MedBlock MedBlock;
    MedBlock.ID = Block.ID;
    MedBlock.StartSlot = Block.StartSlot;
    MedBlock.EndSlot = Block.EndSlot;
    Program.Med.Blocks.push_back(std::move(MedBlock));
  }
}

bool sameValue(const RegisterValue &L, const RegisterValue &R) {
  return L.ValueKind == R.ValueKind && L.Value == R.Value &&
         L.Offset == R.Offset;
}

RegisterValue mergeValue(const std::vector<const MedBlock *> &Predecessors,
                         unsigned Register) {
  if (Predecessors.empty())
    return {};
  RegisterValue Result = Predecessors.front()->Outputs[Register];
  for (const MedBlock *Block : Predecessors)
    if (!sameValue(Result, Block->Outputs[Register]))
      return {};
  return Result;
}

void runRegisterDataflow(SBFProgram &Program) {
  if (Program.Med.Blocks.empty())
    return;
  std::map<size_t, const MedInstruction *> BySlot;
  for (const MedInstruction &Instruction : Program.Med.Instructions)
    BySlot[Instruction.Slot] = &Instruction;

  size_t EntryBlock = 0;
  for (const BasicBlock &Block : Program.Low.Blocks)
    if (Program.Low.EntrySlot >= Block.StartSlot &&
        Program.Low.EntrySlot < Block.EndSlot) {
      EntryBlock = Block.ID;
      break;
    }
  Program.Med.Blocks[EntryBlock].Inputs[kFramePointerRegister] = {
      RegisterValue::Kind::StackAddress, kStackStart, 0};
  const size_t IterationLimit = Program.Med.Blocks.size() * 4 + 1;
  for (size_t Iteration = 0; Iteration < IterationLimit; ++Iteration) {
    bool Changed = false;
    for (MedBlock &Block : Program.Med.Blocks) {
      if (Block.ID != EntryBlock) {
        std::vector<const MedBlock *> Predecessors;
        for (size_t ID : Program.Low.Blocks[Block.ID].Predecessors)
          Predecessors.push_back(&Program.Med.Blocks[ID]);
        for (unsigned Register = 0; Register < kRegisterCount; ++Register) {
          RegisterValue Merged = mergeValue(Predecessors, Register);
          if (!sameValue(Block.Inputs[Register], Merged)) {
            Block.Inputs[Register] = Merged;
            Changed = true;
          }
        }
      }

      auto State = Block.Inputs;
      for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
        auto It = BySlot.find(Slot);
        if (It == BySlot.end())
          continue;
        const MedInstruction &Instruction = *It->second;
        auto Constant = [&](uint64_t Value) {
          State[Instruction.Dst] = {RegisterValue::Kind::Constant, Value, 0};
        };
        if (Instruction.Op == Operation::LoadImm) {
          Constant(Instruction.Immediate);
        } else if (Instruction.Op == Operation::Mov) {
          if (Instruction.Form == OperandForm::DstImm)
            Constant(Instruction.Immediate);
          else
            State[Instruction.Dst] = State[Instruction.Src];
        } else if ((Instruction.Op == Operation::Add ||
                    Instruction.Op == Operation::Sub) &&
                   Instruction.Form == OperandForm::DstImm) {
          RegisterValue &Value = State[Instruction.Dst];
          const int64_t Delta = static_cast<int64_t>(Instruction.Immediate);
          if (Value.ValueKind == RegisterValue::Kind::Constant)
            Value.Value = Instruction.Op == Operation::Add
                              ? Value.Value + Instruction.Immediate
                              : Value.Value - Instruction.Immediate;
          else if (Value.ValueKind == RegisterValue::Kind::StackAddress ||
                   Value.ValueKind == RegisterValue::Kind::RodataAddress)
            Value.Offset += Instruction.Op == Operation::Add ? Delta : -Delta;
          else
            Value = {};
        } else if (Instruction.Op == Operation::Load ||
                   (isALUResult(*getOpcodeInfo(Instruction.SourceOpcode)) &&
                    Instruction.Op != Operation::HighOr)) {
          State[Instruction.Dst] = {};
        }
        if (Instruction.Call != CallKind::None) {
          for (unsigned Register = 0; Register < kFirstCalleeSavedRegister;
               ++Register)
            State[Register] = {};
        }
      }
      if (State != Block.Outputs) {
        Block.Outputs = State;
        Changed = true;
      }
    }
    if (!Changed)
      break;
  }
}

constexpr size_t kNoBlock = std::numeric_limits<size_t>::max();

// Immediate-dominator tree. The previous representation kept one
// std::set<size_t> of (post)dominators per block, which needs O(blocks^2)
// set nodes — real programs with tens of thousands of blocks exhaust the
// host's memory. This is the classic iterative algorithm over reverse
// postorder (Cooper, Harvey, Kennedy, "A Simple, Fast Dominance
// Algorithm"), which is O(blocks) in space and near-linear in practice.
struct DominatorTree {
  size_t Root = kNoBlock;
  std::vector<size_t> IDom;   // immediate dominator, kNoBlock when unreachable
  std::vector<size_t> Depth;  // depth in the dominator tree
  std::vector<size_t> RPONum; // block -> reverse postorder number, kNoBlock
                              // when unreachable from Root
};

template <typename SuccessorsFn, typename PredecessorsFn>
DominatorTree buildDominatorTree(size_t Count, size_t Root,
                                 SuccessorsFn &&Successors,
                                 PredecessorsFn &&Predecessors) {
  DominatorTree Tree;
  Tree.Root = Root;
  Tree.IDom.assign(Count, kNoBlock);
  Tree.Depth.assign(Count, 0);
  Tree.RPONum.assign(Count, kNoBlock);

  // Iterative postorder walk from Root; reverse postorder numbers entry = 0.
  std::vector<size_t> Postorder;
  Postorder.reserve(Count);
  std::vector<std::pair<size_t, size_t>> Stack{{Root, 0}};
  std::vector<size_t> SuccCache;
  std::vector<std::vector<size_t>> Adjacent(Count);
  while (!Stack.empty()) {
    auto &[Node, Next] = Stack.back();
    const std::vector<size_t> &Succs = Adjacent[Node].empty() && Next == 0
                                           ? (Adjacent[Node] = Successors(Node))
                                           : Adjacent[Node];
    if (Next < Succs.size()) {
      const size_t Succ = Succs[Next++];
      if (Tree.RPONum[Succ] == kNoBlock && Succ != Root) {
        Tree.RPONum[Succ] = 0; // mark visited
        Stack.push_back({Succ, 0});
      }
      continue;
    }
    Postorder.push_back(Node);
    Stack.pop_back();
  }
  for (size_t I = 0; I < Postorder.size(); ++I)
    Tree.RPONum[Postorder[Postorder.size() - 1 - I]] = I;

  auto Intersect = [&](size_t B1, size_t B2) {
    while (B1 != B2) {
      while (Tree.RPONum[B1] > Tree.RPONum[B2])
        B1 = Tree.IDom[B1];
      while (Tree.RPONum[B2] > Tree.RPONum[B1])
        B2 = Tree.IDom[B2];
    }
    return B1;
  };

  Tree.IDom[Root] = Root;
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (size_t I = Postorder.size(); I-- > 0;) {
      const size_t ID = Postorder[I];
      if (ID == Root)
        continue;
      size_t NewIDom = kNoBlock;
      for (size_t Pred : Predecessors(ID)) {
        if (Tree.RPONum[Pred] == kNoBlock || Tree.IDom[Pred] == kNoBlock)
          continue;
        NewIDom = NewIDom == kNoBlock ? Pred : Intersect(NewIDom, Pred);
      }
      if (NewIDom != kNoBlock && NewIDom != Tree.IDom[ID]) {
        Tree.IDom[ID] = NewIDom;
        Changed = true;
      }
    }
  }
  // Depths in RPO order: the immediate dominator always has a smaller
  // reverse postorder number than the block itself.
  std::vector<size_t> RPO(Postorder.size());
  for (size_t I = 0; I < Postorder.size(); ++I)
    RPO[Tree.RPONum[Postorder[I]]] = Postorder[I];
  for (size_t ID : RPO)
    if (ID != Root)
      Tree.Depth[ID] = Tree.Depth[Tree.IDom[ID]] + 1;
  return Tree;
}

// Whether A dominates B under Tree (both must be reachable from the root).
bool dominates(const DominatorTree &Tree, size_t A, size_t B) {
  if (A >= Tree.IDom.size() || B >= Tree.IDom.size())
    return false;
  if (Tree.RPONum[A] == kNoBlock || Tree.RPONum[B] == kNoBlock)
    return false;
  size_t Cur = B;
  while (Tree.Depth[Cur] >= Tree.Depth[A]) {
    if (Cur == A)
      return true;
    if (Cur == Tree.Root)
      return false;
    Cur = Tree.IDom[Cur];
  }
  return false;
}

// Deepest common ancestor of A and B in Tree, i.e. the (post)dominator
// candidate with the most (post)dominators. A node that is unreachable from
// the root stands for the full node set, so the other side wins outright.
std::optional<size_t> nearestCommonDominator(const DominatorTree &Tree,
                                             size_t A, size_t B) {
  const bool AReachable = A < Tree.IDom.size() && Tree.RPONum[A] != kNoBlock;
  const bool BReachable = B < Tree.IDom.size() && Tree.RPONum[B] != kNoBlock;
  if (!AReachable && !BReachable)
    return std::nullopt;
  if (!AReachable)
    return B;
  if (!BReachable)
    return A;
  while (Tree.Depth[A] > Tree.Depth[B]) {
    A = Tree.IDom[A];
    if (A == kNoBlock)
      return std::nullopt;
  }
  while (Tree.Depth[B] > Tree.Depth[A]) {
    B = Tree.IDom[B];
    if (B == kNoBlock)
      return std::nullopt;
  }
  while (A != B) {
    if (A == Tree.Root || B == Tree.Root)
      return Tree.Root;
    A = Tree.IDom[A];
    B = Tree.IDom[B];
    if (A == kNoBlock || B == kNoBlock)
      return std::nullopt;
  }
  return A;
}

void recoverRegions(SBFProgram &Program) {
  const size_t Count = Program.Low.Blocks.size();
  if (Count == 0)
    return;
  std::set<size_t> Reachable;
  for (const BasicBlock &Block : Program.Low.Blocks)
    if (Block.Reachable)
      Reachable.insert(Block.ID);
  size_t EntryBlock = 0;
  for (const BasicBlock &Block : Program.Low.Blocks)
    if (Program.Low.EntrySlot >= Block.StartSlot &&
        Program.Low.EntrySlot < Block.EndSlot) {
      EntryBlock = Block.ID;
      break;
    }

  auto CFGSuccessors = [&](size_t ID) {
    std::vector<size_t> Result;
    for (size_t Successor : Program.Low.Blocks[ID].Successors)
      if (Program.Low.Blocks[Successor].Reachable)
        Result.push_back(Successor);
    return Result;
  };
  auto CFGPredecessors = [&](size_t ID) {
    std::vector<size_t> Result;
    for (size_t Pred : Program.Low.Blocks[ID].Predecessors)
      if (Program.Low.Blocks[Pred].Reachable)
        Result.push_back(Pred);
    return Result;
  };
  const DominatorTree Dominators =
      buildDominatorTree(Count, EntryBlock, CFGSuccessors, CFGPredecessors);

  // Post-dominators are dominators of the reversed graph. A virtual root
  // (index Count) links every exit block so multiple exits share one tree.
  std::set<size_t> Exits;
  for (const BasicBlock &Block : Program.Low.Blocks) {
    if (!Block.Reachable)
      continue;
    const bool HasReachableSuccessor = std::any_of(
        Block.Successors.begin(), Block.Successors.end(),
        [&](size_t Successor) { return Reachable.contains(Successor); });
    if (!HasReachableSuccessor)
      Exits.insert(Block.ID);
  }
  const size_t VirtualRoot = Count;
  auto RevSuccessors = [&](size_t ID) {
    if (ID == VirtualRoot)
      return std::vector<size_t>(Exits.begin(), Exits.end());
    return CFGPredecessors(ID);
  };
  auto RevPredecessors = [&](size_t ID) {
    std::vector<size_t> Result;
    if (ID == VirtualRoot)
      return Result;
    Result = CFGSuccessors(ID);
    if (Exits.contains(ID))
      Result.push_back(VirtualRoot); // virtual root -> exit edge
    return Result;
  };
  const DominatorTree PostDominators = buildDominatorTree(
      Count + 1, VirtualRoot, RevSuccessors, RevPredecessors);

  std::set<std::pair<size_t, size_t>> SeenLoops;
  for (const BasicBlock &Source : Program.Low.Blocks) {
    if (!Source.Reachable)
      continue;
    for (size_t Target : Source.Successors) {
      if (!dominates(Dominators, Target, Source.ID) ||
          !SeenLoops.insert({Target, Source.ID}).second)
        continue;
      std::set<size_t> Loop{Target, Source.ID};
      std::deque<size_t> Work{Source.ID};
      while (!Work.empty()) {
        const size_t ID = Work.front();
        Work.pop_front();
        for (size_t Pred : Program.Low.Blocks[ID].Predecessors)
          if (Loop.insert(Pred).second && Pred != Target)
            Work.push_back(Pred);
      }
      Region Region;
      Region.Kind = RegionKind::Loop;
      Region.HeaderBlock = Target;
      Region.Blocks.assign(Loop.begin(), Loop.end());
      for (size_t ID : Loop)
        for (size_t Successor : Program.Low.Blocks[ID].Successors)
          if (!Loop.contains(Successor)) {
            Region.ExitBlock = Successor;
            break;
          }
      Program.High.Regions.push_back(std::move(Region));
    }
  }

  for (const BasicBlock &Block : Program.Low.Blocks) {
    if (Block.Successors.size() != 2)
      continue;
    const size_t Left = Block.Successors[0];
    const size_t Right = Block.Successors[1];
    std::optional<size_t> Join =
        nearestCommonDominator(PostDominators, Left, Right);
    if (Join && *Join == VirtualRoot)
      Join = std::nullopt; // the branches reach disjoint exits
    Region Region;
    Region.Kind = RegionKind::If;
    Region.HeaderBlock = Block.ID;
    Region.ExitBlock = Join;
    std::set<size_t> Members{Block.ID};
    std::deque<size_t> Work{Left, Right};
    while (!Work.empty()) {
      const size_t ID = Work.front();
      Work.pop_front();
      if ((Join && ID == *Join) || !Members.insert(ID).second)
        continue;
      for (size_t Successor : Program.Low.Blocks[ID].Successors)
        if (Reachable.contains(Successor))
          Work.push_back(Successor);
    }
    Region.Blocks.assign(Members.begin(), Members.end());
    Program.High.Regions.push_back(std::move(Region));
  }
}

void recoverHighIR(const BinaryImage &Image, SBFProgram &Program) {
  std::set<size_t> Entries{Program.Low.EntrySlot};
  for (const LowInstruction &Instruction : Program.Low.Instructions)
    if (Instruction.CallTarget)
      Entries.insert(*Instruction.CallTarget);
  for (const Symbol &Symbol : Image.Symbols)
    if (Symbol.IsFunc)
      if (auto Slot = addressToSlot(Program.Image, Symbol.Addr))
        Entries.insert(*Slot);

  // The slot→block map and the call-free successor adjacency are identical
  // for every entry. Build them once; rebuilding them per entry (and
  // rescanning every edge per visited block) is quadratic and does not
  // terminate in reasonable time on large production programs.
  constexpr size_t kNoBlock = std::numeric_limits<size_t>::max();
  std::vector<size_t> SlotToBlock(Program.Low.Instructions.size(), kNoBlock);
  for (const BasicBlock &Block : Program.Low.Blocks)
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot)
      SlotToBlock[Slot] = Block.ID;
  std::vector<std::vector<size_t>> CallFreeSuccessors(Program.Low.Blocks.size());
  for (const CFGEdge &Edge : Program.Low.Edges)
    if (Edge.To && Edge.Kind != EdgeKind::Call)
      CallFreeSuccessors[Edge.From].push_back(*Edge.To);

  for (size_t EntrySlot : Entries) {
    Function Function;
    Function.EntrySlot = EntrySlot;
    Function.Address = Program.Low.TextAddress + EntrySlot * kInstructionSize;
    if (const Symbol *Symbol = findFunctionSymbol(Image, Function.Address))
      Function.Name = Symbol->Name;
    else if (EntrySlot == Program.Low.EntrySlot)
      Function.Name = kEntrySymbolName.str();
    else
      Function.Name = syntheticFunctionName(Function.Address);

    const size_t EntryBlock =
        EntrySlot < SlotToBlock.size() ? SlotToBlock[EntrySlot] : kNoBlock;
    if (EntryBlock != kNoBlock) {
      std::deque<size_t> Work{EntryBlock};
      std::vector<bool> Seen(Program.Low.Blocks.size(), false);
      while (!Work.empty()) {
        size_t ID = Work.front();
        Work.pop_front();
        if (Seen[ID])
          continue;
        Seen[ID] = true;
        Function.Blocks.push_back(ID);
        for (size_t To : CallFreeSuccessors[ID])
          Work.push_back(To);
      }
    }
    Program.High.Functions.push_back(std::move(Function));
  }
  std::sort(Program.High.Functions.begin(), Program.High.Functions.end(),
            [](const Function &L, const Function &R) {
              return L.EntrySlot < R.EntrySlot;
            });

  for (const LowInstruction &Instruction : Program.Low.Instructions) {
    if (Instruction.Info &&
        (Instruction.Info->Op == Operation::Load ||
         Instruction.Info->Op == Operation::Store) &&
        (Instruction.Src == kFirstArgumentRegister ||
         Instruction.Dst == kFirstArgumentRegister))
      Program.High.UsesAccounts = true;
    if (Instruction.Call == CallKind::None)
      continue;
    Program.High.Calls.push_back({Instruction.Slot, Instruction.CallTarget,
                                  Instruction.Call, Instruction.ResolvedName});
    if (Instruction.Call == CallKind::Syscall) {
      Program.High.Syscalls.push_back(
          {Instruction.Slot, Instruction.SyscallHash, Instruction.Syscall});
      if (Instruction.Syscall &&
          Instruction.Syscall->Category == SyscallCategory::CPI)
        Program.High.UsesCPI = true;
    }
  }

  const uint64_t RodataBase = Program.Image.RodataVM.Address;
  size_t Start = 0;
  while (Start < Program.Rodata.size()) {
    while (Start < Program.Rodata.size() &&
           !std::isprint(static_cast<unsigned char>(Program.Rodata[Start])))
      ++Start;
    size_t End = Start;
    while (End < Program.Rodata.size() &&
           std::isprint(static_cast<unsigned char>(Program.Rodata[End])))
      ++End;
    if (End - Start >= 4)
      Program.High.Strings.push_back(
          {RodataBase + Start, std::string(reinterpret_cast<const char *>(
                                               Program.Rodata.data() + Start),
                                           End - Start)});
    Start = End + (End < Program.Rodata.size());
  }
  recoverRegions(Program);
}

llvm::StringRef edgeKindName(EdgeKind Kind) {
  switch (Kind) {
  case EdgeKind::Fallthrough:
    return "fallthrough";
  case EdgeKind::BranchTaken:
    return "taken";
  case EdgeKind::Branch:
    return "branch";
  case EdgeKind::Call:
    return "call";
  case EdgeKind::IndirectCall:
    return "callx";
  case EdgeKind::Return:
    return "return";
  case EdgeKind::Invalid:
    return "invalid";
  }
  return "invalid";
}

llvm::StringRef operationName(Operation Op) {
  switch (Op) {
#define SBF_OPERATION_CASE(NAME)                                               \
  case Operation::NAME:                                                        \
    return #NAME
    SBF_OPERATION_CASE(LoadImm);
    SBF_OPERATION_CASE(Load);
    SBF_OPERATION_CASE(Store);
    SBF_OPERATION_CASE(Add);
    SBF_OPERATION_CASE(Sub);
    SBF_OPERATION_CASE(Mul);
    SBF_OPERATION_CASE(UHighMul);
    SBF_OPERATION_CASE(SHighMul);
    SBF_OPERATION_CASE(UDiv);
    SBF_OPERATION_CASE(URem);
    SBF_OPERATION_CASE(SDiv);
    SBF_OPERATION_CASE(SRem);
    SBF_OPERATION_CASE(Or);
    SBF_OPERATION_CASE(And);
    SBF_OPERATION_CASE(Xor);
    SBF_OPERATION_CASE(LSh);
    SBF_OPERATION_CASE(RSh);
    SBF_OPERATION_CASE(ARSh);
    SBF_OPERATION_CASE(Neg);
    SBF_OPERATION_CASE(Mov);
    SBF_OPERATION_CASE(EndianLE);
    SBF_OPERATION_CASE(EndianBE);
    SBF_OPERATION_CASE(HighOr);
    SBF_OPERATION_CASE(Jump);
    SBF_OPERATION_CASE(Eq);
    SBF_OPERATION_CASE(Ne);
    SBF_OPERATION_CASE(UGt);
    SBF_OPERATION_CASE(UGe);
    SBF_OPERATION_CASE(ULt);
    SBF_OPERATION_CASE(ULe);
    SBF_OPERATION_CASE(SGt);
    SBF_OPERATION_CASE(SGe);
    SBF_OPERATION_CASE(SLt);
    SBF_OPERATION_CASE(SLe);
    SBF_OPERATION_CASE(Set);
    SBF_OPERATION_CASE(Call);
    SBF_OPERATION_CASE(CallX);
    SBF_OPERATION_CASE(Exit);
    SBF_OPERATION_CASE(Invalid);
#undef SBF_OPERATION_CASE
  }
  return "Invalid";
}

} // namespace

llvm::Expected<SBFProgram> analyze(const BinaryImage &Image,
                                   const AnalyzeOptions &Options) {
  if (Image.Arch != Arch::SBF || !Image.SBF)
    return llvm::make_error<llvm::StringError>(
        "sbf: input image is not a loaded SBF ELF",
        llvm::inconvertibleErrorCode());
  SBFProgram Program;
  Program.Image = *Image.SBF;
  if (Options.VersionOverride != Version::Auto) {
    if (!isConcreteVersion(Options.VersionOverride))
      return llvm::make_error<llvm::StringError>(
          "sbf: invalid explicit version override",
          llvm::inconvertibleErrorCode());
    Program.Image.Version = Options.VersionOverride;
  }

  const Section *Text = Image.getSectionByName(kTextSectionName);
  if (!Text)
    Text = Image.getTextSection();
  if (!Text)
    return llvm::make_error<llvm::StringError>(
        "sbf: loaded image has no executable text section",
        llvm::inconvertibleErrorCode());
  Program.Text = Text->Data;
  if (Program.Image.RodataFile.Size != 0) {
    if (const Section *Rodata = Image.getSectionByName(kRodataSectionName))
      Program.Rodata = Rodata->Data;
  }
  Program.Low.TheVersion = Program.Image.Version;
  Program.Low.TextAddress = Program.Image.TextVM.Address;
  if (llvm::Error Error = applyLegacyTextRelocations(Image, Program))
    return std::move(Error);
  if (Image.Entry < Program.Low.TextAddress ||
      (Image.Entry - Program.Low.TextAddress) % kInstructionSize != 0)
    return llvm::make_error<llvm::StringError>(
        "sbf: entry point is not instruction-aligned in text",
        llvm::inconvertibleErrorCode());
  Program.Low.EntrySlot = static_cast<size_t>(
      (Image.Entry - Program.Low.TextAddress) / kInstructionSize);

  DecodeContext Context{Image, Options, Program};
  if (llvm::Error Error = decodeInstructions(Context))
    return std::move(Error);
  if (Program.Low.EntrySlot >= Program.Low.Instructions.size() ||
      Program.Low.Instructions[Program.Low.EntrySlot].IsContinuation)
    return analysisError(Program.Low.EntrySlot, Image.Entry,
                         "entry point is not a complete instruction");
  if (llvm::Error Error = resolveControlFlow(Context))
    return std::move(Error);
  buildCFG(Program.Low);
  buildMedIR(Program);
  runRegisterDataflow(Program);
  if (Options.RecoverHighIR)
    recoverHighIR(Image, Program);
  return Program;
}

const Function *findFunction(const SBFProgram &Program,
                             llvm::StringRef Identifier) {
  if (Identifier.empty()) {
    for (const Function &Candidate : Program.High.Functions)
      if (Candidate.EntrySlot == Program.Low.EntrySlot)
        return &Candidate;
    return nullptr;
  }

  for (const Function &Candidate : Program.High.Functions)
    if (Candidate.Name == Identifier)
      return &Candidate;

  if (Identifier.front() == '-')
    return nullptr;
  llvm::StringRef AddressText = Identifier;
  if ((AddressText.consume_front("0x") || AddressText.consume_front("0X")) &&
      AddressText.empty())
    return nullptr;
  va_t Address = 0;
  if (AddressText.getAsInteger(16, Address))
    return nullptr;
  for (const Function &Candidate : Program.High.Functions)
    if (Candidate.Address == Address)
      return &Candidate;
  return nullptr;
}

std::string formatInstruction(const LowInstruction &Instruction) {
  if (Instruction.IsContinuation)
    return ".lddw.cont";
  if (!Instruction.Info)
    return ".byte 0x" + llvm::utohexstr(Instruction.RawOpcode);
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  OS << Instruction.Info->Mnemonic;
  switch (Instruction.Info->Form) {
  case OperandForm::None:
    break;
  case OperandForm::Dst:
    OS << " r" << unsigned(Instruction.Dst);
    break;
  case OperandForm::DstImm:
    OS << " r" << unsigned(Instruction.Dst) << ", "
       << static_cast<int64_t>(Instruction.RawImmediate);
    break;
  case OperandForm::DstSrc:
    OS << " r" << unsigned(Instruction.Dst) << ", r"
       << unsigned(Instruction.Src);
    break;
  case OperandForm::LDDW:
    OS << " r" << unsigned(Instruction.Dst) << ", 0x"
       << llvm::utohexstr(Instruction.Immediate);
    break;
  case OperandForm::Load:
    OS << " r" << unsigned(Instruction.Dst) << ", [r"
       << unsigned(Instruction.Src) << (Instruction.Offset < 0 ? " - " : " + ")
       << std::abs(static_cast<int>(Instruction.Offset)) << "]";
    break;
  case OperandForm::StoreImm:
    OS << " [r" << unsigned(Instruction.Dst)
       << (Instruction.Offset < 0 ? " - " : " + ")
       << std::abs(static_cast<int>(Instruction.Offset)) << "], "
       << static_cast<int64_t>(Instruction.RawImmediate);
    break;
  case OperandForm::StoreReg:
    OS << " [r" << unsigned(Instruction.Dst)
       << (Instruction.Offset < 0 ? " - " : " + ")
       << std::abs(static_cast<int>(Instruction.Offset)) << "], r"
       << unsigned(Instruction.Src);
    break;
  case OperandForm::Endian:
    OS << " r" << unsigned(Instruction.Dst) << ", " << Instruction.RawImmediate;
    break;
  case OperandForm::Branch:
    OS << " block_" << Instruction.BranchTarget.value_or(0);
    break;
  case OperandForm::BranchImm:
    OS << " r" << unsigned(Instruction.Dst) << ", "
       << static_cast<int64_t>(Instruction.RawImmediate) << ", block_"
       << Instruction.BranchTarget.value_or(0);
    break;
  case OperandForm::BranchReg:
    OS << " r" << unsigned(Instruction.Dst) << ", r"
       << unsigned(Instruction.Src) << ", block_"
       << Instruction.BranchTarget.value_or(0);
    break;
  case OperandForm::CallImm:
    OS << " "
       << (Instruction.ResolvedName.empty()
               ? std::to_string(Instruction.RawImmediate)
               : Instruction.ResolvedName);
    break;
  case OperandForm::CallReg:
    OS << " r" << unsigned(Instruction.CallRegister);
    break;
  }
  return Buffer;
}

std::string dumpLowIR(const LowIR &IR) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  OS << "; SBF LowIR " << versionDisplayName(IR.TheVersion) << " text=0x"
     << llvm::utohexstr(IR.TextAddress) << " entry=" << IR.EntrySlot << "\n";
  for (const BasicBlock &Block : IR.Blocks) {
    OS << "block_" << Block.ID << ": ; slots [" << Block.StartSlot << ", "
       << Block.EndSlot << ")" << (Block.Reachable ? "" : " unreachable")
       << "\n";
    for (size_t Slot = Block.StartSlot; Slot < Block.EndSlot; ++Slot) {
      const LowInstruction &Instruction = IR.Instructions[Slot];
      OS << "  " << llvm::format_hex(Instruction.Address, 18) << "  "
         << formatInstruction(Instruction) << "\n";
    }
    OS << "  successors:";
    for (size_t Successor : Block.Successors)
      OS << " block_" << Successor;
    OS << "\n";
  }
  for (const CFGEdge &Edge : IR.Edges) {
    OS << "; edge block_" << Edge.From << " " << edgeKindName(Edge.Kind);
    if (Edge.To)
      OS << " block_" << *Edge.To;
    OS << "\n";
  }
  for (const Diagnostic &Diagnostic : IR.Diagnostics)
    OS << "; "
       << (Diagnostic.Severity == DiagnosticSeverity::Error ? "error"
                                                            : "warning")
       << " slot " << Diagnostic.Slot << ": " << Diagnostic.Message << "\n";
  return Buffer;
}

std::string dumpMedIR(const MedIR &IR) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  OS << "; SBF MedIR " << versionDisplayName(IR.TheVersion) << "\n";
  for (const MedBlock &Block : IR.Blocks) {
    OS << "block_" << Block.ID << ":\n";
    for (const MedInstruction &Instruction : IR.Instructions) {
      if (Instruction.Slot < Block.StartSlot ||
          Instruction.Slot >= Block.EndSlot)
        continue;
      OS << "  %pc" << Instruction.Slot << " = "
         << operationName(Instruction.Op) << "." << unsigned(Instruction.Width)
         << " r" << unsigned(Instruction.Dst);
      if (Instruction.Form == OperandForm::DstSrc ||
          Instruction.Form == OperandForm::Load ||
          Instruction.Form == OperandForm::StoreReg ||
          Instruction.Form == OperandForm::BranchReg)
        OS << ", r" << unsigned(Instruction.Src);
      else if (Instruction.Form == OperandForm::DstImm ||
               Instruction.Form == OperandForm::StoreImm ||
               Instruction.Form == OperandForm::BranchImm ||
               Instruction.Form == OperandForm::LDDW)
        OS << ", 0x" << llvm::utohexstr(Instruction.Immediate);
      if (Instruction.SwapOperands)
        OS << " [swapped]";
      if (Instruction.BranchTarget)
        OS << " -> slot " << *Instruction.BranchTarget;
      if (Instruction.Syscall)
        OS << " @" << Instruction.Syscall->Name;
      OS << "\n";
    }
  }
  return Buffer;
}

std::string dumpHighIR(const HighIR &IR) {
  std::string Buffer;
  llvm::raw_string_ostream OS(Buffer);
  OS << "; SBF HighIR\n";
  for (const Function &Function : IR.Functions) {
    OS << "function " << Function.Name << " @ 0x"
       << llvm::utohexstr(Function.Address) << " {";
    for (size_t Block : Function.Blocks)
      OS << " block_" << Block;
    OS << " }\n";
  }
  for (const Region &Region : IR.Regions) {
    OS << (Region.Kind == RegionKind::Loop ? "loop"
           : Region.Kind == RegionKind::If ? "if"
                                           : "irreducible")
       << " block_" << Region.HeaderBlock;
    if (Region.ExitBlock)
      OS << " -> block_" << *Region.ExitBlock;
    OS << "\n";
  }
  for (const SyscallUse &Use : IR.Syscalls)
    OS << "syscall slot " << Use.Slot << " "
       << (Use.Info ? Use.Info->Name : kUnknownSyscallName) << " (0x"
       << llvm::utohexstr(Use.Hash) << ")\n";
  for (const RecoveredString &String : IR.Strings)
    OS << "string 0x" << llvm::utohexstr(String.Address) << " \""
       << String.Value << "\"\n";
  OS << "; cpi=" << IR.UsesCPI << " account-memory=" << IR.UsesAccounts << "\n";
  return Buffer;
}

} // namespace neverd::sbf
