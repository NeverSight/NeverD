//===- SBFProgramImage.cpp - Canonical Solana SBF VM image ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/image/SBFProgramImage.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/sbf/SBFConstants.h"
#include "neverd/sbf/image/SBFRelocations.h"
#include "neverd/sbf/runtime/SBFOpcodes.h"
#include "neverd/sbf/runtime/SBFRuntimeEnvironment.h"
#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace neverd::sbf {
namespace {

llvm::Error imageError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      (llvm::Twine("sbf: program image: ") + Message).str(),
      llvm::inconvertibleErrorCode());
}

llvm::Error relocationError(const RelocationEntry &Relocation,
                            llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      (llvm::Twine("sbf: relocation ") + llvm::Twine(Relocation.Type) +
       " at 0x" + llvm::utohexstr(Relocation.Address) + ": " + Message)
          .str(),
      llvm::inconvertibleErrorCode());
}

bool checkedAdd(uint64_t Left, uint64_t Right, uint64_t &Result) {
  if (Right > std::numeric_limits<uint64_t>::max() - Left)
    return false;
  Result = Left + Right;
  return true;
}

uint64_t saturatingAdd(uint64_t Left, uint64_t Right) {
  if (Right > std::numeric_limits<uint64_t>::max() - Left)
    return std::numeric_limits<uint64_t>::max();
  return Left + Right;
}

uint64_t legacyRawSectionAddress(const Section &Section) {
  return Section.VA >= kBytecodeStart ? Section.VA - kBytecodeStart
                                      : Section.VA;
}

uint64_t legacyRuntimeSectionAddress(uint64_t RawAddress, bool OptimizeRodata) {
  if (OptimizeRodata && RawAddress >= kBytecodeStart)
    return RawAddress;
  return saturatingAdd(kBytecodeStart, RawAddress);
}

} // namespace

bool ProgramRegion::contains(va_t Start, size_t Size) const {
  if (Start < Address)
    return false;
  const uint64_t Offset = Start - Address;
  return Offset <= Bytes.size() && Size <= Bytes.size() - Offset;
}

llvm::ArrayRef<uint8_t> ProgramImage::text() const {
  if (TextRegion == NoRegion || TextRegion >= Regions.size() ||
      TextOffset > Regions[TextRegion].Bytes.size() ||
      TextSize > Regions[TextRegion].Bytes.size() - TextOffset)
    return {};
  return llvm::ArrayRef(Regions[TextRegion].Bytes).slice(TextOffset, TextSize);
}

const ProgramFunctionEntry *ProgramImage::findFunction(uint32_t Key) const {
  const auto It = std::lower_bound(
      Functions.begin(), Functions.end(), Key,
      [](const ProgramFunctionEntry &Entry, uint32_t Candidate) {
        return Entry.Key < Candidate;
      });
  return It != Functions.end() && It->Key == Key ? &*It : nullptr;
}

llvm::Error ProgramImage::registerFunction(uint32_t Key, size_t TargetSlot,
                                           llvm::StringRef Name) {
  if (TextVirtualSize == 0 ||
      TargetSlot > (TextVirtualSize - 1) / kInstructionSize)
    return imageError("function registry target is outside program text");
  const auto It = PendingFunctions.find(Key);
  if (It != PendingFunctions.end()) {
    if (It->second.TargetSlot != TargetSlot)
      return imageError(llvm::Twine("function registry key 0x") +
                        llvm::utohexstr(Key) + " collides");
    return llvm::Error::success();
  }
  PendingFunctions.insert({Key, {Key, TargetSlot, Name.str()}});
  return llvm::Error::success();
}

llvm::Error ProgramImage::registerLoaderFunction(
    uint32_t Key, size_t TargetSlot, llvm::StringRef Name,
    llvm::ArrayRef<uint32_t> RegisteredSyscalls) {
  if (std::binary_search(RegisteredSyscalls.begin(), RegisteredSyscalls.end(),
                         Key))
    return imageError(llvm::Twine("function registry key 0x") +
                      llvm::utohexstr(Key) +
                      " collides with a registered syscall");
  return registerFunction(Key, TargetSlot, Name);
}

llvm::Error
ProgramImage::registerEntrypoint(llvm::ArrayRef<uint32_t> RegisteredSyscalls) {
  const uint32_t Key = legacyFunctionKey(EntrySlot, kEntrySymbolName);
  // Upstream deliberately gives the ELF entrypoint final ownership of this
  // reserved name hash, even when a dynamic function symbol named entrypoint
  // was relocated to another slot earlier.
  PendingFunctions.erase(Key);
  return registerLoaderFunction(Key, EntrySlot, kEntrySymbolName,
                                RegisteredSyscalls);
}

void ProgramImage::finalizeFunctions() {
  Functions.reserve(PendingFunctions.size());
  for (auto &Entry : PendingFunctions)
    Functions.push_back(std::move(Entry.second));
  PendingFunctions.shrink_and_clear();
  std::sort(
      Functions.begin(), Functions.end(),
      [](const ProgramFunctionEntry &Left, const ProgramFunctionEntry &Right) {
        return Left.Key < Right.Key;
      });
}

llvm::Error ProgramImage::relocateLegacyRelativeCalls(
    llvm::MutableArrayRef<uint8_t> Text,
    llvm::ArrayRef<uint32_t> RegisteredSyscalls) {
  const OpcodeInfo *Call = getOpcodeInfo(Opcode::CALL_IMM);
  if (!Call)
    return imageError("CALL_IMM is absent from the opcode table");
  const size_t InstructionCount = Text.size() / kInstructionSize;
  for (size_t Slot = 0; Slot < InstructionCount; ++Slot) {
    uint8_t *Instruction = Text.data() + Slot * kInstructionSize;
    if (Instruction[kOpcodeOffset] != Call->Encoding)
      continue;
    const int32_t Immediate = static_cast<int32_t>(
        llvm::support::endian::read32le(Instruction + kImmediateOffset));
    if (Immediate == kLegacyUnresolvedCallImmediate)
      continue;
    const int64_t Target =
        static_cast<int64_t>(Slot) + 1 + static_cast<int64_t>(Immediate);
    if (Target < 0 || static_cast<uint64_t>(Target) >= InstructionCount)
      return imageError(llvm::Twine("relative CALL at slot ") +
                        llvm::Twine(Slot) + " targets outside program text");
    const size_t TargetSlot = static_cast<size_t>(Target);
    const uint32_t Key = legacyFunctionKey(TargetSlot, {});
    if (llvm::Error Error =
            registerLoaderFunction(Key, TargetSlot, {}, RegisteredSyscalls))
      return Error;
    llvm::support::endian::write32le(Instruction + kImmediateOffset, Key);
  }
  return llvm::Error::success();
}

const ProgramRegion *ProgramImage::findRegion(va_t Address, size_t Size,
                                              bool DataAccess) const {
  for (const ProgramRegion &Region : Regions)
    if ((!DataAccess || Region.DataVisible) && Region.contains(Address, Size))
      return &Region;
  return nullptr;
}

llvm::Expected<llvm::ArrayRef<uint8_t>>
ProgramImage::slice(va_t Address, size_t Size, bool DataAccess) const {
  const ProgramRegion *Region = findRegion(Address, Size, DataAccess);
  if (!Region)
    return imageError("requested VM range is not mapped");
  const size_t Offset = static_cast<size_t>(Address - Region->Address);
  return llvm::ArrayRef(Region->Bytes).slice(Offset, Size);
}

llvm::Error ProgramImage::finalize(va_t Address, size_t Size) {
  uint64_t TextEnd = 0;
  if (!checkedAdd(Address, Size, TextEnd))
    return imageError("text range overflows");

  std::sort(Regions.begin(), Regions.end(),
            [](const ProgramRegion &Left, const ProgramRegion &Right) {
              return Left.Address < Right.Address;
            });
  uint64_t PreviousEnd = 0;
  bool HavePrevious = false;
  for (size_t Index = 0; Index < Regions.size(); ++Index) {
    ProgramRegion &Region = Regions[Index];
    if (Region.Bytes.empty() && Size != 0)
      return imageError("runtime region is empty");
    uint64_t RegionEnd = 0;
    if (!checkedAdd(Region.Address, Region.Bytes.size(), RegionEnd))
      return imageError("runtime region range overflows");
    if (HavePrevious && Region.Address < PreviousEnd)
      return imageError("runtime regions overlap");
    PreviousEnd = RegionEnd;
    HavePrevious = true;

    std::sort(
        Region.Sections.begin(), Region.Sections.end(),
        [](const ProgramSectionSpan &Left, const ProgramSectionSpan &Right) {
          return Left.Offset < Right.Offset;
        });
    size_t PreviousSectionEnd = 0;
    for (const ProgramSectionSpan &Section : Region.Sections) {
      if ((!Section.Executable && Section.Size == 0) ||
          !rangeInBounds(Section.Offset, Section.Size, Region.Bytes.size()))
        return imageError("source section span is outside its runtime region");
      if (Section.Offset < PreviousSectionEnd &&
          Region.Kind != ProgramRegionKind::LegacyReadOnly)
        return imageError("source section spans overlap");
      PreviousSectionEnd =
          std::max(PreviousSectionEnd, Section.Offset + Section.Size);
      if (!Section.Executable)
        continue;
      if (Region.Address + Section.Offset != Address || Section.Size != Size)
        return imageError("executable section disagrees with text metadata");
      if (TextRegion != NoRegion)
        return imageError("multiple executable text sections are present");
      TextRegion = Index;
      TextOffset = Section.Offset;
      TextSize = Section.Size;
      TextAddress = Address;
    }
  }
  if (TextRegion == NoRegion)
    return imageError("text is absent from the canonical image");
  (void)TextEnd;
  return llvm::Error::success();
}

llvm::Expected<ProgramImage>
createProgramImage(llvm::ArrayRef<uint8_t> Text, va_t TextAddress,
                   llvm::ArrayRef<uint8_t> Rodata, va_t RodataAddress,
                   bool LegacyTextIsDataVisible, Version TheVersion,
                   size_t EntrySlot,
                   llvm::ArrayRef<ProgramFunctionEntry> Functions) {
  if (Text.empty())
    return imageError("synthetic text is empty");
  uint64_t TextEnd = 0;
  if (!checkedAdd(TextAddress, Text.size(), TextEnd))
    return imageError("synthetic text range overflows");
  (void)TextEnd;

  ProgramImage Result;
  Result.TheVersion = TheVersion;
  Result.EntrySlot = EntrySlot;
  Result.TextVirtualSize = Text.size();
  ProgramRegion TextRegion;
  TextRegion.Address = TextAddress;
  TextRegion.Bytes.assign(Text.begin(), Text.end());
  TextRegion.Kind = LegacyTextIsDataVisible ? ProgramRegionKind::LegacyReadOnly
                                            : ProgramRegionKind::Bytecode;
  TextRegion.DataVisible = LegacyTextIsDataVisible;
  TextRegion.Name = kTextSegmentName.str();
  TextRegion.Sections.push_back({kTextSectionName.str(), 0, Text.size(), true});
  Result.Regions.push_back(std::move(TextRegion));

  if (!Rodata.empty()) {
    uint64_t RodataEnd = 0;
    if (!checkedAdd(RodataAddress, Rodata.size(), RodataEnd))
      return imageError("synthetic rodata range overflows");
    if (RodataAddress < TextEnd && TextAddress < RodataEnd)
      return imageError("synthetic text and rodata overlap");
    ProgramRegion Region;
    Region.Address = RodataAddress;
    Region.Bytes.assign(Rodata.begin(), Rodata.end());
    Region.Kind = ProgramRegionKind::ReadOnly;
    Region.DataVisible = true;
    Region.Name = kRodataSegmentName.str();
    Region.Sections.push_back(
        {kRodataSectionName.str(), 0, Rodata.size(), false});
    Result.Regions.push_back(std::move(Region));
  }
  if (llvm::Error Error = Result.finalize(TextAddress, Text.size()))
    return std::move(Error);
  const size_t InstructionCount = Text.size() / kInstructionSize;
  if (EntrySlot >= InstructionCount)
    return imageError("synthetic entrypoint is outside program text");
  for (const ProgramFunctionEntry &Function : Functions) {
    if (Function.TargetSlot >= InstructionCount)
      return imageError("synthetic function target is outside program text");
    if (llvm::Error Error = Result.registerFunction(
            Function.Key, Function.TargetSlot, Function.Name))
      return std::move(Error);
  }
  Result.finalizeFunctions();
  return Result;
}

llvm::Expected<ProgramImage>
buildProgramImage(const BinaryImage &Image, const Metadata &Metadata,
                  const SBFVMConfig &Config,
                  llvm::ArrayRef<uint32_t> RegisteredSyscallHashes) {
  if (llvm::Error Error = validateVMConfig(Config))
    return std::move(Error);

  const Section *Text = Image.getSectionByName(kTextSectionName);
  if (!Text)
    Text = Image.getTextSection();
  if (!Text)
    return imageError("loaded image has no text section");
  if (Text->VA != Metadata.TextVM.Address ||
      Text->Size != Metadata.TextVM.Size ||
      Text->Data.size() != Metadata.TextFile.Size)
    return imageError("text section disagrees with loader metadata");
  if (Metadata.TextVM.Size > std::numeric_limits<size_t>::max() ||
      Metadata.TextFile.Size > std::numeric_limits<size_t>::max())
    return imageError("text size exceeds the host address space");

  ProgramImage Result;
  Result.TheVersion = Metadata.Version;
  Result.TextAddress = Metadata.TextVM.Address;
  Result.TextSize = static_cast<size_t>(Metadata.TextFile.Size);
  Result.TextVirtualSize = static_cast<size_t>(Metadata.TextVM.Size);

  const bool StaticSyscalls =
      versionHasFeature(Metadata.Version, VersionFeature::StaticSyscalls);
  const bool HasExactRelocationInventory =
      std::all_of(Image.Relocations.begin(), Image.Relocations.end(),
                  [](const RelocationEntry &Relocation) {
                    return Relocation.ELF.has_value();
                  });
  const bool UseRawRelocationImage =
      !Metadata.StrictLayout && !Image.Raw.empty() &&
      Metadata.TextFile.Size != 0 && HasExactRelocationInventory;
  std::vector<uint32_t> RegisteredSyscalls(RegisteredSyscallHashes.begin(),
                                           RegisteredSyscallHashes.end());
  std::sort(RegisteredSyscalls.begin(), RegisteredSyscalls.end());
  RegisteredSyscalls.erase(
      std::unique(RegisteredSyscalls.begin(), RegisteredSyscalls.end()),
      RegisteredSyscalls.end());
  if (!Metadata.StrictLayout && Config.RejectBrokenELFs) {
    if (Result.TextAddress < kBytecodeStart ||
        Result.TextAddress - kBytecodeStart != Metadata.TextFile.Offset)
      return imageError("legacy .text sh_addr does not match its file offset");
  }
  std::vector<uint8_t> RelocatedRaw;
  if (UseRawRelocationImage) {
    if (Text->FileOff != Metadata.TextFile.Offset ||
        Text->FileSz != Metadata.TextFile.Size ||
        Metadata.TextFile.Size != Metadata.TextVM.Size ||
        !rangeInBounds(Metadata.TextFile.Offset, Metadata.TextFile.Size,
                       Image.Raw.size()))
      return imageError("raw text range disagrees with loader metadata");
    RelocatedRaw = Image.Raw;
  }

  std::vector<const RelocationEntry *> Relocations;
  Relocations.reserve(Image.Relocations.size());
  for (const RelocationEntry &Relocation : Image.Relocations)
    Relocations.push_back(&Relocation);
  if (UseRawRelocationImage)
    std::stable_sort(
        Relocations.begin(), Relocations.end(),
        [](const RelocationEntry *Left, const RelocationEntry *Right) {
          return Left->ELF->Ordinal < Right->ELF->Ordinal;
        });

  auto ApplyRelocations = [&](auto &&GetField) -> llvm::Error {
    const va_t TextAddress = Metadata.TextVM.Address;
    const uint64_t TextSize = Metadata.TextVM.Size;
    for (const RelocationEntry *RelocationPointer : Relocations) {
      const RelocationEntry &Relocation = *RelocationPointer;
      const RelocationInfo *Info = getRelocationInfo(Relocation.Type);
      if (!Info)
        return relocationError(Relocation, "unsupported relocation type");
      if (!isRelocationAllowedForVersion(*Info, Metadata.Version))
        return relocationError(
            Relocation, "relocation is unavailable for this SBF version");
      if (Info->Width == 0 || Info->Width % kBitsPerByte != 0)
        return relocationError(Relocation,
                               "relocation metadata has an invalid bit width");
      const ELFRelocationSymbol *Symbol =
          Relocation.ELF && Relocation.ELF->Symbol ? &*Relocation.ELF->Symbol
                                                   : nullptr;
      bool IsText = Relocation.Address >= TextAddress &&
                    Relocation.Address - TextAddress < TextSize;
      if (UseRawRelocationImage) {
        const uint64_t RawOffset = Relocation.ELF->RawOffset;
        IsText = RawOffset >= Metadata.TextFile.Offset &&
                 RawOffset - Metadata.TextFile.Offset < Metadata.TextFile.Size;
      }
      auto Field = [&](uint64_t Offset, size_t Size) {
        return GetField(Relocation, Offset, Size);
      };

      switch (Info->ID) {
      case Relocation::Abs64: {
        auto LowField = Field(kImmediateOffset, sizeof(uint32_t));
        if (!LowField)
          return LowField.takeError();
        const uint32_t Low = llvm::support::endian::read32le(LowField->data());
        if (!Symbol)
          return relocationError(Relocation,
                                 "absolute relocation has no exact symbol");
        uint64_t Value = saturatingAdd(Symbol->Value, Low);
        if (Value < kMemoryRegionSize)
          Value = saturatingAdd(kBytecodeStart, Value);
        llvm::support::endian::write32le(LowField->data(),
                                         static_cast<uint32_t>(Value));
        auto HighField =
            Field(kInstructionSize + kImmediateOffset, sizeof(uint32_t));
        if (!HighField)
          return HighField.takeError();
        llvm::support::endian::write32le(
            HighField->data(), static_cast<uint32_t>(Value >> kWordBitWidth));
        break;
      }
      case Relocation::Relative64: {
        auto LowField = Field(kImmediateOffset, sizeof(uint32_t));
        if (!LowField)
          return LowField.takeError();
        uint64_t Value = llvm::support::endian::read32le(LowField->data());
        if (IsText) {
          auto HighField =
              Field(kInstructionSize + kImmediateOffset, sizeof(uint32_t));
          if (!HighField)
            return HighField.takeError();
          Value |= static_cast<uint64_t>(
                       llvm::support::endian::read32le(HighField->data()))
                   << kWordBitWidth;
          if (Value == 0)
            return relocationError(Relocation,
                                   "relative relocation has a zero value");
          if (Value < kMemoryRegionSize)
            Value = saturatingAdd(kBytecodeStart, Value);
          llvm::support::endian::write32le(LowField->data(),
                                           static_cast<uint32_t>(Value));
          llvm::support::endian::write32le(
              HighField->data(), static_cast<uint32_t>(Value >> kWordBitWidth));
        } else {
          Value = saturatingAdd(kBytecodeStart, Value);
          auto Destination = Field(0, sizeof(uint64_t));
          if (!Destination)
            return Destination.takeError();
          llvm::support::endian::write64le(Destination->data(), Value);
        }
        break;
      }
      case Relocation::Call32: {
        if (!Symbol || !Symbol->Name)
          return relocationError(Relocation,
                                 "call relocation has no exact named symbol");
        const llvm::StringRef Name = *Symbol->Name;
        uint32_t Key = hashSymbolName(Name);
        if (Symbol->isFunction() && Symbol->Value != 0) {
          if (TextAddress < kBytecodeStart)
            return relocationError(Relocation,
                                   "internal call text base is invalid");
          const uint64_t RawTextAddress = TextAddress - kBytecodeStart;
          uint64_t RawTextEnd = 0;
          if (!checkedAdd(RawTextAddress, TextSize, RawTextEnd) ||
              Symbol->Value < RawTextAddress || Symbol->Value >= RawTextEnd)
            return relocationError(
                Relocation, "internal call symbol is outside program text");
          const size_t TargetSlot = static_cast<size_t>(
              (Symbol->Value - RawTextAddress) / kInstructionSize);
          Key = legacyFunctionKey(TargetSlot, Name);
          if (llvm::Error Error = Result.registerLoaderFunction(
                  Key, TargetSlot, Name, RegisteredSyscalls))
            return Error;
        } else if (Config.RejectBrokenELFs &&
                   !std::binary_search(RegisteredSyscalls.begin(),
                                       RegisteredSyscalls.end(), Key)) {
          return relocationError(
              Relocation, llvm::Twine("unresolved external symbol: ") + Name);
        }
        auto Immediate = Field(kImmediateOffset, sizeof(uint32_t));
        if (!Immediate)
          return Immediate.takeError();
        llvm::support::endian::write32le(Immediate->data(), Key);
        break;
      }
      case Relocation::Unknown:
        llvm_unreachable("unknown relocation has no metadata entry");
      }
    }
    return llvm::Error::success();
  };

  auto FinalizeEntrypoint = [&]() -> llvm::Error {
    if (Image.Entry < Result.TextAddress ||
        (Image.Entry - Result.TextAddress) % kInstructionSize != 0)
      return imageError("entrypoint is outside or misaligned in program text");
    Result.EntrySlot = static_cast<size_t>((Image.Entry - Result.TextAddress) /
                                           kInstructionSize);
    if (Image.Entry - Result.TextAddress >= Result.TextVirtualSize)
      return imageError("entrypoint is outside program text");
    if (!StaticSyscalls)
      return Result.registerEntrypoint(RegisteredSyscalls);
    return llvm::Error::success();
  };

  if (UseRawRelocationImage) {
    llvm::MutableArrayRef<uint8_t> Raw(RelocatedRaw);
    auto RawText = Raw.slice(static_cast<size_t>(Metadata.TextFile.Offset),
                             static_cast<size_t>(Metadata.TextFile.Size));
    if (!StaticSyscalls)
      if (llvm::Error Error =
              Result.relocateLegacyRelativeCalls(RawText, RegisteredSyscalls))
        return std::move(Error);
    auto RawField =
        [&](const RelocationEntry &Relocation, uint64_t Offset,
            size_t Size) -> llvm::Expected<llvm::MutableArrayRef<uint8_t>> {
      uint64_t FileOffset = 0;
      if (!Relocation.ELF ||
          !checkedAdd(Relocation.ELF->RawOffset, Offset, FileOffset))
        return relocationError(Relocation, "raw relocation field overflows");
      if (!rangeInBounds(FileOffset, Size, RelocatedRaw.size()))
        return relocationError(Relocation,
                               "raw relocation field is outside the ELF");
      return llvm::MutableArrayRef<uint8_t>(RelocatedRaw)
          .slice(static_cast<size_t>(FileOffset), Size);
    };
    if (llvm::Error Error = ApplyRelocations(RawField))
      return std::move(Error);
    if (llvm::Error Error = FinalizeEntrypoint())
      return std::move(Error);
  }

  auto SectionBytes =
      [&](const Section &Section) -> llvm::Expected<llvm::ArrayRef<uint8_t>> {
    if (!UseRawRelocationImage)
      return llvm::ArrayRef<uint8_t>(Section.Data);
    if (Section.FileSz != Section.Data.size() ||
        !rangeInBounds(Section.FileOff, Section.FileSz, RelocatedRaw.size()))
      return imageError("section file range is invalid after relocation");
    return llvm::ArrayRef<uint8_t>(RelocatedRaw)
        .slice(static_cast<size_t>(Section.FileOff),
               static_cast<size_t>(Section.FileSz));
  };

  if (Metadata.StrictLayout) {
    if (Metadata.RodataVM.Size != 0) {
      const Section *Rodata = Image.getSectionByName(kRodataSectionName);
      if (!Rodata || Rodata->VA != Metadata.RodataVM.Address ||
          Rodata->Data.size() != Metadata.RodataVM.Size)
        return imageError("rodata section disagrees with loader metadata");
      ProgramRegion Region;
      Region.Address = Rodata->VA;
      auto Bytes = SectionBytes(*Rodata);
      if (!Bytes)
        return Bytes.takeError();
      Region.Bytes.assign(Bytes->begin(), Bytes->end());
      Region.Kind = ProgramRegionKind::ReadOnly;
      Region.DataVisible = true;
      Region.Name = kRodataSegmentName.str();
      Region.Sections.push_back({Rodata->Name, 0, Rodata->Data.size(), false});
      Result.Regions.push_back(std::move(Region));
    }

    ProgramRegion Region;
    Region.Address = Text->VA;
    auto Bytes = SectionBytes(*Text);
    if (!Bytes)
      return Bytes.takeError();
    Region.Bytes.assign(Bytes->begin(), Bytes->end());
    Region.Kind = ProgramRegionKind::Bytecode;
    Region.DataVisible = false;
    Region.Name = kTextSegmentName.str();
    Region.Sections.push_back({Text->Name, 0, Text->Data.size(), true});
    Result.Regions.push_back(std::move(Region));
  } else {
    std::vector<const Section *> Selected;
    uint64_t Lowest = std::numeric_limits<uint64_t>::max();
    uint64_t Highest = 0;
    for (const Section &Section : Image.Sections) {
      if (!isLegacyReadOnlySectionName(Section.Name) || Section.Data.empty())
        continue;
      Selected.push_back(&Section);
    }
    if (Selected.empty() && Metadata.LegacyReadOnlySections.empty())
      return imageError("legacy ELF has no runtime read-only sections");

    uint64_t LowestRawAddress = std::numeric_limits<uint64_t>::max();
    uint64_t HighestRawAddress = 0;
    uint64_t ReadOnlyFillSize = 0;
    auto AccountRawLayout = [&](uint64_t RawAddress, uint64_t FileOffset,
                                uint64_t FileSize) -> llvm::Error {
      // The lenient upstream parser treats every legacy sh_addr as an offset
      // in the bytecode region before it chooses a borrowed or owned rodata
      // representation.  In particular, optimize_rodata does not make an
      // already-based address legal: sh_addr + MM_REGION_SIZE must still end
      // no later than MM_STACK_START.
      if (saturatingAdd(RawAddress, kMemoryRegionSize) > kStackStart)
        return imageError(
            "legacy read-only VM range address exceeds bytecode offset domain");
      if (Config.RejectBrokenELFs && RawAddress != FileOffset)
        return imageError(
            "legacy read-only sh_addr does not match its file offset");
      const uint64_t RuntimeAddress =
          legacyRuntimeSectionAddress(RawAddress, Config.OptimizeRodata);
      LowestRawAddress = std::min(LowestRawAddress, RawAddress);
      HighestRawAddress =
          std::max(HighestRawAddress, saturatingAdd(RawAddress, FileSize));
      ReadOnlyFillSize = saturatingAdd(ReadOnlyFillSize, FileSize);
      Lowest = std::min(Lowest, RuntimeAddress);
      Highest = std::max(Highest, saturatingAdd(RuntimeAddress, FileSize));
      return llvm::Error::success();
    };
    if (!Metadata.LegacyReadOnlySections.empty()) {
      for (const LegacyReadOnlySectionLayout &Layout :
           Metadata.LegacyReadOnlySections)
        if (llvm::Error Error = AccountRawLayout(
                Layout.RawAddress, Layout.FileOffset, Layout.FileSize))
          return std::move(Error);
    } else {
      for (const Section *Section : Selected)
        if (llvm::Error Error =
                AccountRawLayout(legacyRawSectionAddress(*Section),
                                 Section->FileOff, Section->Data.size()))
          return std::move(Error);
    }
    if (Config.RejectBrokenELFs &&
        saturatingAdd(LowestRawAddress, ReadOnlyFillSize) > HighestRawAddress)
      return imageError("legacy read-only sections exceed their packed span");

    const uint64_t Base = Config.OptimizeRodata ? Lowest : kBytecodeStart;
    const uint64_t RegionSize = Highest >= Base ? Highest - Base : 0;
    if (Base > Lowest || Highest < Base || Highest > kStackStart ||
        RegionSize >= kMemoryRegionSize ||
        RegionSize > std::numeric_limits<size_t>::max())
      return imageError("legacy read-only VM range is invalid");
    if (!Image.Raw.empty() && RegionSize > Image.Raw.size())
      return imageError("legacy read-only VM range exceeds the ELF file");

    ProgramRegion Region;
    Region.Address = Base;
    Region.Bytes.resize(static_cast<size_t>(RegionSize));
    Region.Kind = ProgramRegionKind::LegacyReadOnly;
    Region.DataVisible = true;
    Region.Name = kRodataSegmentName.str();
    const bool MaterializeRawInventory =
        UseRawRelocationImage && !Metadata.LegacyReadOnlySections.empty();
    if (MaterializeRawInventory) {
      for (const LegacyReadOnlySectionLayout &Layout :
           Metadata.LegacyReadOnlySections) {
        if (Layout.FileSize == 0)
          continue;
        const uint64_t RuntimeAddress = legacyRuntimeSectionAddress(
            Layout.RawAddress, Config.OptimizeRodata);
        if (RuntimeAddress < Base ||
            !rangeInBounds(RuntimeAddress - Base, Layout.FileSize,
                           Region.Bytes.size()) ||
            !rangeInBounds(Layout.FileOffset, Layout.FileSize,
                           RelocatedRaw.size()))
          return imageError(
              "legacy raw section lies outside its materialized range");
        const size_t DestinationOffset =
            static_cast<size_t>(RuntimeAddress - Base);
        const auto Bytes = llvm::ArrayRef<uint8_t>(RelocatedRaw)
                               .slice(static_cast<size_t>(Layout.FileOffset),
                                      static_cast<size_t>(Layout.FileSize));
        std::copy(Bytes.begin(), Bytes.end(),
                  Region.Bytes.begin() +
                      static_cast<ptrdiff_t>(DestinationOffset));
      }
    }
    bool AddedTextSpan = false;
    for (const Section *Section : Selected) {
      const uint64_t RuntimeAddress = legacyRuntimeSectionAddress(
          legacyRawSectionAddress(*Section), Config.OptimizeRodata);
      if (RuntimeAddress < Base)
        return imageError("legacy section lies below merged read-only range");
      const size_t Offset = static_cast<size_t>(RuntimeAddress - Base);
      if (!rangeInBounds(Offset, Section->Data.size(), Region.Bytes.size()))
        return imageError("legacy section lies outside merged read-only range");
      if (!MaterializeRawInventory) {
        auto Bytes = SectionBytes(*Section);
        if (!Bytes)
          return Bytes.takeError();
        std::copy(Bytes->begin(), Bytes->end(),
                  Region.Bytes.begin() + static_cast<ptrdiff_t>(Offset));
      }
      const bool IsText = Section->Name == kTextSectionName;
      Region.Sections.push_back(
          {Section->Name, Offset, Section->Data.size(), IsText});
      AddedTextSpan |= IsText;
    }
    if (!AddedTextSpan) {
      if (Metadata.TextVM.Address < Base ||
          Metadata.TextVM.Address - Base > Region.Bytes.size())
        return imageError("empty legacy text lies outside read-only range");
      Region.Sections.push_back(
          {kTextSectionName.str(),
           static_cast<size_t>(Metadata.TextVM.Address - Base), 0, true});
    }
    Result.Regions.push_back(std::move(Region));
  }

  if (llvm::Error Error = Result.finalize(
          Metadata.TextVM.Address, static_cast<size_t>(Metadata.TextFile.Size)))
    return std::move(Error);

  if (Metadata.StrictLayout) {
    if (llvm::Error Error = FinalizeEntrypoint())
      return std::move(Error);
    Result.finalizeFunctions();
    return Result;
  }

  if (!UseRawRelocationImage) {
    llvm::MutableArrayRef<uint8_t> CanonicalText(
        Result.Regions[Result.TextRegion].Bytes);
    CanonicalText = CanonicalText.slice(Result.TextOffset, Result.TextSize);
    if (!StaticSyscalls)
      if (llvm::Error Error = Result.relocateLegacyRelativeCalls(
              CanonicalText, RegisteredSyscalls))
        return std::move(Error);
    auto CanonicalField =
        [&](const RelocationEntry &Relocation, uint64_t Offset,
            size_t Size) -> llvm::Expected<llvm::MutableArrayRef<uint8_t>> {
      uint64_t Address = 0;
      if (!checkedAdd(Relocation.Address, Offset, Address))
        return relocationError(Relocation, "relocation field overflows");
      for (ProgramRegion &Region : Result.Regions) {
        if (!Region.contains(Address, Size))
          continue;
        const size_t RegionOffset =
            static_cast<size_t>(Address - Region.Address);
        return llvm::MutableArrayRef<uint8_t>(Region.Bytes)
            .slice(RegionOffset, Size);
      }
      return imageError("relocation target is outside the runtime image");
    };
    if (llvm::Error Error = ApplyRelocations(CanonicalField))
      return std::move(Error);
    if (llvm::Error Error = FinalizeEntrypoint())
      return std::move(Error);
  }
  Result.finalizeFunctions();
  return Result;
}

llvm::Expected<ProgramImage>
buildProgramImage(const BinaryImage &Image, const Metadata &Metadata,
                  const ResolvedRuntimeEnvironment &Environment) {
  if (!Environment.supportsVersion(Metadata.Version))
    return imageError(llvm::Twine(versionDisplayName(Metadata.Version)) +
                      " is outside the runtime enabled version range [" +
                      versionName(Environment.minimumVersion()) + ", " +
                      versionName(Environment.maximumVersion()) + "]");
  return buildProgramImage(Image, Metadata, Environment.vmConfig(),
                           Environment.registeredSyscallHashes());
}

} // namespace neverd::sbf
