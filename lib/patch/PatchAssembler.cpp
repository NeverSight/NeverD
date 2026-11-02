//===- PatchAssembler.cpp - LLVM MC interactive assembler -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Single instruction assembly with explicit PC-relative branch fixups.
//===----------------------------------------------------------------------===//

#include "neverd/decode/Decoder.h"
#include "neverd/decode/NativeOperands.h"
#include "neverd/patch/Limits.h"
#include "neverd/patch/PatchActions.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/MC/MCAsmBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCCodeEmitter.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCObjectWriter.h"
#include "llvm/MC/MCParser/AsmLexer.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"

#include <mutex>

using namespace neverd;
using namespace neverd::patch;
namespace patchLimits = neverd::limits::patch;

static llvm::Error invalid(const llvm::Twine &Message) {
  return llvm::make_error<llvm::StringError>(Message,
                                             llvm::inconvertibleErrorCode());
}

llvm::Expected<std::vector<uint8_t>> patch::assemble(Arch A,
                                                     const AssemblyRequest &R) {
  auto Text = llvm::StringRef(R.Text).trim();
  if (Text.empty() || Text.size() > patchLimits::kMaxAssemblyTextBytes ||
      Text.contains('\n') || Text.contains('\r') || Text.contains(';') ||
      Text.contains(':') || Text.front() == '.' || Text.contains('\0'))
    return invalid(
        "assemble accepts one instruction, without labels or directives");
  const auto DisplayText = Text.lower();
  for (llvm::StringRef Word : {"offset", "short"})
    for (size_t At = DisplayText.find(Word); At != std::string::npos;
         At = DisplayText.find(Word, At + Word.size())) {
      const auto Identifier = [](char C) {
        return llvm::isAlnum(C) || C == '_';
      };
      if ((!At || !Identifier(DisplayText[At - 1])) &&
          (At + Word.size() == DisplayText.size() ||
           !Identifier(DisplayText[At + Word.size()])))
        return invalid(
            "listing-only offset/short annotations are not assembly operands");
    }
  const bool X86 = A == Arch::X64 || A == Arch::X86;
  std::string TripleName;
  switch (A) {
  case Arch::X64:
    TripleName = "x86_64-unknown-linux-gnu";
    break;
  case Arch::X86:
    TripleName = "i386-unknown-linux-gnu";
    break;
  case Arch::AArch64:
    TripleName = "aarch64-unknown-linux-gnu";
    break;
  case Arch::ARM:
    if (R.Mode != InstructionMode::ARM && R.Mode != InstructionMode::Thumb)
      return invalid("ARM assembly requires an explicit ARM or Thumb mode");
    TripleName = R.Mode == InstructionMode::Thumb
                     ? "thumbv7-unknown-linux-gnueabi"
                     : "armv7-unknown-linux-gnueabi";
    break;
  default:
    return invalid("unsupported architecture for assembly");
  }
  static std::once_flag Initialized;
  std::call_once(Initialized, [] {
    LLVMInitializeX86TargetInfo();
    LLVMInitializeX86TargetMC();
    LLVMInitializeX86AsmParser();
    LLVMInitializeARMTargetInfo();
    LLVMInitializeARMTargetMC();
    LLVMInitializeARMAsmParser();
    LLVMInitializeAArch64TargetInfo();
    LLVMInitializeAArch64TargetMC();
    LLVMInitializeAArch64AsmParser();
  });
  llvm::Triple Triple(TripleName);
  std::string Error;
  const auto *Target = llvm::TargetRegistry::lookupTarget(Triple, Error);
  if (!Target)
    return invalid(Error);
  llvm::MCTargetOptions Options;
  std::unique_ptr<llvm::MCRegisterInfo> Registers(
      Target->createMCRegInfo(Triple));
  std::unique_ptr<llvm::MCAsmInfo> Info(
      Target->createMCAsmInfo(*Registers, Triple, Options));
  std::unique_ptr<llvm::MCInstrInfo> Instructions(Target->createMCInstrInfo());
  std::unique_ptr<llvm::MCSubtargetInfo> Subtarget(
      Target->createMCSubtargetInfo(Triple, "", ""));
  llvm::SourceMgr Sources;
  Sources.setDiagHandler(
      [](const llvm::SMDiagnostic &D, void *Context) {
        auto &Message = *static_cast<std::string *>(Context);
        if (!Message.empty())
          Message += "\n";
        Message += D.getMessage();
      },
      &Error);
  llvm::MCContext Context(Triple, *Info, *Registers, *Subtarget, &Sources);
  std::unique_ptr<llvm::MCObjectFileInfo> ObjectInfo(
      Target->createMCObjectFileInfo(Context, false));
  Context.setObjectFileInfo(ObjectInfo.get());
  std::string Source = X86 && !R.ATT ? ".intel_syntax noprefix\n" : "";
  if (A == Arch::ARM)
    Source += ".syntax unified\n";
  if (R.Symbols.size() > patchLimits::kMaxAssemblySymbols)
    return invalid("too many assembler symbols");
  for (const auto &[Name, Value] : R.Symbols) {
    if (Name == "__neverd_patch_target" || Name.empty() ||
        Name.size() > patchLimits::kMaxAssemblySymbolBytes ||
        !(llvm::isAlpha(Name[0]) || Name[0] == '_') ||
        !llvm::all_of(Name,
                      [](char C) { return llvm::isAlnum(C) || C == '_'; }))
      return invalid("invalid assembler symbol name");
    Source += ".set " + Name + ", " + std::to_string(Value) + "\n";
  }
  // Use an unresolved target occurrence for numeric branches. LLVM then emits
  // an architecture relocation rather than interpreting an ARM number as a
  // displacement or an x86 target as a zero-based section address.
  std::string Instruction = Text.str();
  const auto Mnemonic =
      Text.take_until([](char C) { return llvm::isSpace(C); }).lower();
  auto ARMName = llvm::StringRef(Mnemonic);
  if (ARMName.ends_with(".w") || ARMName.ends_with(".n"))
    ARMName = ARMName.drop_back(2);
  bool ARMBranch = ARMName == "b" || ARMName == "bl" || ARMName == "blx";
  if (A == Arch::ARM)
    for (llvm::StringRef Condition :
         {"eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs", "vc", "hi",
          "ls", "ge", "lt", "gt", "le", "al"})
      ARMBranch |= ARMName == ("b" + Condition).str() ||
                   ARMName == ("bl" + Condition).str();
  const bool Branch =
      X86 ? (llvm::StringRef(Mnemonic).starts_with("j") ||
             (Mnemonic == "call" || Mnemonic == "callq") ||
             llvm::StringRef(Mnemonic).starts_with("loop"))
          : ((A == Arch::ARM && ARMBranch) || Mnemonic == "b" ||
             Mnemonic == "bl" || llvm::StringRef(Mnemonic).starts_with("b.") ||
             Mnemonic == "cbz" || Mnemonic == "cbnz" || Mnemonic == "tbz" ||
             Mnemonic == "tbnz");
  std::optional<uint64_t> BranchTarget;
  if (Branch) {
    size_t At = Instruction.find_last_of(" ,\t");
    if (At != std::string::npos) {
      auto Operand = llvm::StringRef(Instruction).drop_front(At + 1);
      Operand.consume_front("#");
      if (Operand.starts_with("-") || Operand.starts_with("+"))
        return invalid(
            "branch targets must be explicit unsigned absolute addresses");
      uint64_t Number = 0;
      bool Numeric = !Operand.getAsInteger(0, Number);
      if (!Numeric && Operand.ends_with_insensitive("h"))
        Numeric = !Operand.drop_back().getAsInteger(16, Number);
      if (!Numeric) {
        auto It = R.Symbols.find(Operand.str());
        if (It != R.Symbols.end()) {
          Number = uint64_t(It->second);
          Numeric = true;
        }
      }
      if (Numeric) {
        BranchTarget = Number;
        Instruction.resize(At + 1);
        Instruction += "__neverd_patch_target";
      }
    }
  }
  Source += Instruction + "\n";
  Sources.AddNewSourceBuffer(llvm::MemoryBuffer::getMemBufferCopy(Source),
                             llvm::SMLoc());
  llvm::SmallVector<char, 0> ObjectBytes;
  llvm::raw_svector_ostream Output(ObjectBytes);
  std::unique_ptr<llvm::MCCodeEmitter> Emitter(
      Target->createMCCodeEmitter(*Instructions, Context));
  std::unique_ptr<llvm::MCAsmBackend> Backend(
      Target->createMCAsmBackend(*Subtarget, *Registers, Options));
  auto Writer = Backend->createObjectWriter(Output);
  std::unique_ptr<llvm::MCStreamer> Stream(Target->createMCObjectStreamer(
      Triple, Context, std::move(Backend), std::move(Writer),
      std::move(Emitter), *Subtarget));
  std::unique_ptr<llvm::MCAsmParser> Parser(
      llvm::createMCAsmParser(Sources, Context, *Stream, *Info));
  std::unique_ptr<llvm::MCTargetAsmParser> TargetParser(
      Target->createMCAsmParser(*Subtarget, *Parser, *Instructions));
  Parser->setTargetParser(*TargetParser);
  if (X86)
    Parser->getLexer().setLexMasmIntegers(true);
  if (Parser->Run(false))
    return invalid(Error.empty() ? "assembly failed" : Error);
  if (Context.hadError())
    return invalid(Error);
  auto Object =
      llvm::object::ObjectFile::createObjectFile(llvm::MemoryBufferRef(
          llvm::StringRef(ObjectBytes.data(), ObjectBytes.size()), "assembly"));
  if (!Object)
    return Object.takeError();
  std::vector<uint8_t> Bytes;
  for (const auto &Section : (*Object)->sections()) {
    if (!Section.isText())
      continue;
    auto Content = Section.getContents();
    if (!Content)
      return Content.takeError();
    Bytes.assign(Content->begin(), Content->end());
  }
  bool FixedTarget = false;
  for (const auto &Section : (*Object)->sections()) {
    for (const auto &Reloc : Section.relocations()) {
      const auto Symbol = Reloc.getSymbol();
      if (!BranchTarget || Symbol == (*Object)->symbol_end())
        return invalid("unresolved assembly relocation");
      auto Name = Symbol->getName();
      if (!Name)
        return Name.takeError();
      if (*Name != "__neverd_patch_target" || FixedTarget)
        return invalid("unresolved or ambiguous assembly symbol");
      const auto At = Reloc.getOffset();
      const uint64_t Type = Reloc.getType();
      if (X86) {
        const unsigned Width =
            (A == Arch::X64 && Type == llvm::ELF::R_X86_64_PC8) ||
                    (A == Arch::X86 && Type == llvm::ELF::R_386_PC8)
                ? 1
                : 4;
        const bool Supported = A == Arch::X64
                                   ? (Type == llvm::ELF::R_X86_64_PC8 ||
                                      Type == llvm::ELF::R_X86_64_PC32 ||
                                      Type == llvm::ELF::R_X86_64_PLT32)
                                   : (Type == llvm::ELF::R_386_PC8 ||
                                      Type == llvm::ELF::R_386_PC32);
        if (!Supported || At > Bytes.size() || Width > Bytes.size() - At)
          return invalid("unsupported branch relocation");
        const int64_t Delta = int64_t(*BranchTarget - R.Address - Bytes.size());
        if ((Width == 1 && (Delta < INT8_MIN || Delta > INT8_MAX)) ||
            (Width == 4 && (Delta < INT32_MIN || Delta > INT32_MAX)))
          return invalid("branch target is outside the encoding range");
        for (unsigned B = 0; B < Width; ++B)
          Bytes[At + B] = uint64_t(Delta) >> (B * 8);
      } else if (A == Arch::AArch64 && Bytes.size() == 4 && At == 0) {
        unsigned Bits = 0, Shift = 0;
        if (Type == llvm::ELF::R_AARCH64_JUMP26 ||
            Type == llvm::ELF::R_AARCH64_CALL26)
          Bits = 26;
        else if (Type == llvm::ELF::R_AARCH64_CONDBR19) {
          Bits = 19;
          Shift = 5;
        } else if (Type == llvm::ELF::R_AARCH64_TSTBR14) {
          Bits = 14;
          Shift = 5;
        } else
          return invalid("unsupported AArch64 assembly relocation");
        const int64_t Delta = int64_t(*BranchTarget - R.Address);
        if ((Delta & 3) || Delta < -(int64_t(1) << (Bits + 1)) ||
            Delta >= (int64_t(1) << (Bits + 1)))
          return invalid("AArch64 branch target is unaligned or out of range");
        const uint32_t Mask = ((1u << Bits) - 1) << Shift;
        uint32_t W = llvm::support::endian::read32le(Bytes.data());
        W = (W & ~Mask) | ((uint32_t(Delta / 4) << Shift) & Mask);
        llvm::support::endian::write32le(Bytes.data(), W);
      } else if (A == Arch::ARM && At == 0) {
        if (R.Mode == InstructionMode::ARM && Bytes.size() == 4 &&
            (Type == llvm::ELF::R_ARM_CALL || Type == llvm::ELF::R_ARM_JUMP24 ||
             Type == llvm::ELF::R_ARM_PC24)) {
          const int64_t Delta = int64_t(*BranchTarget - R.Address - 8);
          auto W = llvm::support::endian::read32le(Bytes.data());
          if ((W >> 28) == 15 || (Delta & 3) || Delta < -(int64_t(1) << 25) ||
              Delta >= (int64_t(1) << 25))
            return invalid("ARM branch target is unaligned, out of range or "
                           "changes instruction mode");
          llvm::support::endian::write32le(
              Bytes.data(),
              (W & 0xff000000) | (uint32_t(Delta / 4) & 0xffffff));
        } else if (R.Mode == InstructionMode::Thumb) {
          const int64_t Delta = int64_t(*BranchTarget - R.Address - 4);
          if (Delta & 1)
            return invalid("Thumb branch target is not halfword aligned");
          if (Bytes.size() == 2 && (Type == llvm::ELF::R_ARM_THM_JUMP11 ||
                                    Type == llvm::ELF::R_ARM_THM_JUMP8)) {
            const unsigned Bits = Type == llvm::ELF::R_ARM_THM_JUMP11 ? 11 : 8;
            if (Delta < -(int64_t(1) << Bits) || Delta >= (int64_t(1) << Bits))
              return invalid("Thumb branch target is out of range");
            const uint16_t Mask = (1u << Bits) - 1;
            auto W = llvm::support::endian::read16le(Bytes.data());
            llvm::support::endian::write16le(
                Bytes.data(), (W & ~Mask) | (uint16_t(Delta / 2) & Mask));
          } else if (Bytes.size() == 4 &&
                     (Type == llvm::ELF::R_ARM_THM_JUMP24 ||
                      Type == llvm::ELF::R_ARM_THM_CALL ||
                      Type == llvm::ELF::R_ARM_THM_JUMP19)) {
            const bool Conditional = Type == llvm::ELF::R_ARM_THM_JUMP19;
            const unsigned Bits = Conditional ? 20 : 24;
            if (Delta < -(int64_t(1) << Bits) || Delta >= (int64_t(1) << Bits))
              return invalid("Thumb-2 branch target is out of range");
            uint16_t First = llvm::support::endian::read16le(Bytes.data());
            uint16_t Second = llvm::support::endian::read16le(Bytes.data() + 2);
            if (Type == llvm::ELF::R_ARM_THM_CALL && !(Second & 0x1000))
              return invalid(
                  "Thumb interworking branch requires a target mode proof");
            const uint32_t D = uint32_t(Delta);
            const unsigned S = (D >> Bits) & 1;
            const unsigned J1 =
                Conditional ? ((D >> 18) & 1) : (1 ^ ((D >> 23) & 1) ^ S);
            const unsigned J2 =
                Conditional ? ((D >> 19) & 1) : (1 ^ ((D >> 22) & 1) ^ S);
            const uint16_t LowMask = Conditional ? 0x3f : 0x3ff;
            First = (First & ~(0x400 | LowMask)) | (S << 10) |
                    ((D >> 12) & LowMask);
            Second = (Second & ~0x2fff) | (J1 << 13) | (J2 << 11) |
                     ((D >> 1) & 0x7ff);
            llvm::support::endian::write16le(Bytes.data(), First);
            llvm::support::endian::write16le(Bytes.data() + 2, Second);
          } else
            return invalid("unsupported Thumb branch relocation");
        } else
          return invalid("unsupported ARM branch relocation");
      } else
        return invalid(
            "this architecture's branch relocation is not supported");
      FixedTarget = true;
    }
  }
  if (BranchTarget && !FixedTarget)
    return invalid("assembler did not retain the requested branch target");
  if (X86 && BranchTarget) {
    const int64_t ShortDelta = int64_t(*BranchTarget - R.Address - 2);
    if (ShortDelta >= INT8_MIN && ShortDelta <= INT8_MAX) {
      if (Bytes.size() == 5 && Bytes[0] == 0xe9)
        Bytes = {0xeb, uint8_t(ShortDelta)};
      else if (Bytes.size() == 6 && Bytes[0] == 0x0f &&
               (Bytes[1] & 0xf0) == 0x80)
        Bytes = {uint8_t(0x70 | (Bytes[1] & 15)), uint8_t(ShortDelta)};
    }
  }
  Decoder Dec;
  DecodedInsn I{};
  if (!Dec.init(A, R.Mode) || Bytes.empty() ||
      !Dec.decodeOne(Bytes.data(), Bytes.size(), R.Address, I) ||
      I.Size != Bytes.size())
    return invalid("assembly must decode as exactly one instruction");
  if (BranchTarget) {
    const auto Operands = describeNativeOperands(Dec, I, A, false);
    if (Operands.Immediates.empty() ||
        Operands.Immediates.back().Value != *BranchTarget)
      return invalid(
          "decoded branch target differs from the requested address");
  }
  return Bytes;
}
