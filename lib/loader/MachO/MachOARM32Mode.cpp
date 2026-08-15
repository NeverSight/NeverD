//===- MachOARM32Mode.cpp - Authenticated AArch32 code modes -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/MachO/MachOARM32Mode.h"

#include "neverd/object/MachOLayout.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/Errc.h"

#include <limits>
#include <optional>
#include <vector>

namespace neverd::macho_arm32 {

namespace {

using namespace llvm::MachO;

llvm::Error modeError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::invalid_argument,
                                 "macho arm32 mode: " + Message);
}

bool checkedMultiply(uint64_t Left, uint64_t Right, uint64_t &Result) {
  if (Left != 0 && Right > std::numeric_limits<uint64_t>::max() / Left)
    return false;
  Result = Left * Right;
  return true;
}

struct SectionEvidence {
  uint64_t Address = 0;
  uint64_t Size = 0;
  bool IsInstructions = false;
};

} // namespace

llvm::Expected<ModeInfo> parseModeInfo(llvm::ArrayRef<uint8_t> Binary) {
  const MachOHeaderInfo Header = parseMachOHeader(Binary.data(), Binary.size());
  if (Header.HeaderSize == 0 || Header.Is64 ||
      Binary.size() < sizeof(mach_header))
    return modeError("input is not a little-endian 32-bit Mach-O image");

  const auto *MachHeader = reinterpret_cast<const mach_header *>(Binary.data());
  if (MachHeader->cputype != CPU_TYPE_ARM)
    return modeError("input CPU type is not ARM");

  const uint32_t RawCPUSubtype = static_cast<uint32_t>(MachHeader->cpusubtype);
  const uint32_t CapabilityBits =
      RawCPUSubtype & static_cast<uint32_t>(CPU_SUBTYPE_MASK);
  if (CapabilityBits != 0)
    return modeError("unsupported CPU subtype capability bits");

  ModeInfo Result;
  Result.CPUSubtype = RawCPUSubtype & ~static_cast<uint32_t>(CPU_SUBTYPE_MASK);

  uint64_t CommandsEnd = 0;
  if (Header.SizeOfCmds > Binary.size() - Header.HeaderSize)
    return modeError("load-command block is outside the input");
  CommandsEnd = static_cast<uint64_t>(Header.HeaderSize) + Header.SizeOfCmds;

  std::vector<SectionEvidence> SectionsByIndex(1);
  std::optional<symtab_command> SymbolTable;
  uint64_t CommandOffset = Header.HeaderSize;
  for (uint32_t Index = 0; Index < Header.NCmds; ++Index) {
    if (!rangeInBounds(CommandOffset, sizeof(load_command), CommandsEnd))
      return modeError("truncated load-command header");
    const auto *Command = reinterpret_cast<const load_command *>(
        Binary.data() + static_cast<size_t>(CommandOffset));
    if (Command->cmdsize < sizeof(load_command) ||
        !rangeInBounds(CommandOffset, Command->cmdsize, CommandsEnd))
      return modeError("malformed load-command size");

    if (Command->cmd == LC_SEGMENT) {
      if (Command->cmdsize < sizeof(segment_command))
        return modeError("truncated segment command");
      const auto *Segment = reinterpret_cast<const segment_command *>(Command);
      uint64_t SectionBytes = 0;
      if (!checkedMultiply(Segment->nsects, sizeof(section), SectionBytes) ||
          SectionBytes > Command->cmdsize - sizeof(segment_command))
        return modeError("segment section table is malformed");
      const auto *Sections = reinterpret_cast<const section *>(Segment + 1);
      for (uint32_t SectionIndex = 0; SectionIndex < Segment->nsects;
           ++SectionIndex) {
        const uint32_t Flags = Sections[SectionIndex].flags;
        SectionEvidence Evidence;
        Evidence.Address = Sections[SectionIndex].addr;
        Evidence.Size = Sections[SectionIndex].size;
        Evidence.IsInstructions =
            (Flags & (S_ATTR_PURE_INSTRUCTIONS | S_ATTR_SOME_INSTRUCTIONS)) !=
            0;
        SectionsByIndex.push_back(Evidence);
      }
    } else if (Command->cmd == LC_SYMTAB) {
      if (Command->cmdsize < sizeof(symtab_command) || SymbolTable)
        return modeError("symbol-table command is malformed or duplicated");
      SymbolTable = *reinterpret_cast<const symtab_command *>(Command);
    }
    CommandOffset += Command->cmdsize;
  }
  if (CommandOffset != CommandsEnd)
    return modeError("load-command count does not match its byte size");
  if (!SymbolTable)
    return modeError("function modes require an LC_SYMTAB command");

  uint64_t SymbolBytes = 0;
  if (!checkedMultiply(SymbolTable->nsyms, sizeof(nlist), SymbolBytes) ||
      !rangeInBounds(SymbolTable->symoff, SymbolBytes, Binary.size()) ||
      !rangeInBounds(SymbolTable->stroff, SymbolTable->strsize, Binary.size()))
    return modeError("symbol or string table is outside the input");

  for (uint32_t Index = 0; Index < SymbolTable->nsyms; ++Index) {
    const uint64_t EntryOffset =
        static_cast<uint64_t>(SymbolTable->symoff) + Index * sizeof(nlist);
    const auto *Entry = reinterpret_cast<const nlist *>(
        Binary.data() + static_cast<size_t>(EntryOffset));
    if ((Entry->n_type & N_STAB) != 0 || (Entry->n_type & N_TYPE) != N_SECT ||
        Entry->n_sect == NO_SECT || Entry->n_sect >= SectionsByIndex.size() ||
        Entry->n_value == 0)
      continue;

    const va_t Address = clearThumbBit(Entry->n_value);
    const SectionEvidence &Section = SectionsByIndex[Entry->n_sect];
    if (!Section.IsInstructions || Address < Section.Address ||
        Address - Section.Address >= Section.Size)
      continue;
    if ((static_cast<uint16_t>(Entry->n_desc) & N_ARM_THUMB_DEF) == 0)
      continue;
    const InstructionMode Mode = InstructionMode::Thumb;
    if ((Address & 1u) != 0)
      return modeError("a Thumb code symbol is not two-byte aligned");
    auto [It, Inserted] = Result.CodeSymbolModes.emplace(Address, Mode);
    if (!Inserted && It->second != Mode)
      return modeError("one code address has conflicting ARM/Thumb symbols");
  }

  for (const auto &[Address, Mode] : Result.CodeSymbolModes) {
    (void)Address;
    if (Result.UniformMode == InstructionMode::Default)
      Result.UniformMode = Mode;
    else if (Result.UniformMode != Mode) {
      Result.UniformMode = InstructionMode::Default;
      break;
    }
  }
  return Result;
}

llvm::Expected<InstructionMode>
requireUniformFunctionMode(const ModeInfo &Info,
                           llvm::ArrayRef<va_t> FunctionEntries) {
  if (FunctionEntries.empty())
    return modeError("no source-function entries were provided");

  InstructionMode Result = InstructionMode::Default;
  for (va_t RawEntry : FunctionEntries) {
    const va_t Entry = clearThumbBit(RawEntry);
    const auto It = Info.CodeSymbolModes.find(Entry);
    if (It == Info.CodeSymbolModes.end())
      return modeError("a source function has no exact nlist mode");
    if (Result == InstructionMode::Default)
      Result = It->second;
    else if (Result != It->second)
      return modeError("one placement cannot combine ARM and Thumb functions");
  }
  return Result;
}

llvm::Expected<uint64_t> serializeCodePointer(const ModeInfo &Info,
                                              uint64_t Address) {
  const va_t Normalized = clearThumbBit(Address);
  const auto It = Info.CodeSymbolModes.find(Normalized);
  if (It == Info.CodeSymbolModes.end())
    return modeError("a code target has no exact nlist mode");
  return neverd::serializeCodePointer(Normalized, Arch::ARM, It->second);
}

} // namespace neverd::macho_arm32
