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
#include "neverd/sbf/runtime/SBFSyscalls.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Endian.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>

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

bool isLegacyReadOnlySection(llvm::StringRef Name) {
  return Name == kTextSectionName || Name == kRodataSectionName ||
         Name == kDataRelROSectionName || Name == kEhFrameSectionName;
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
  if (Size == 0)
    return imageError("text is empty");
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
    if (Region.Bytes.empty())
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
      if (Section.Size == 0 ||
          !rangeInBounds(Section.Offset, Section.Size, Region.Bytes.size()))
        return imageError("source section span is outside its runtime region");
      if (Section.Offset < PreviousSectionEnd)
        return imageError("source section spans overlap");
      PreviousSectionEnd = Section.Offset + Section.Size;
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

llvm::Expected<ProgramImage> createProgramImage(llvm::ArrayRef<uint8_t> Text,
                                                va_t TextAddress,
                                                llvm::ArrayRef<uint8_t> Rodata,
                                                va_t RodataAddress,
                                                bool LegacyTextIsDataVisible) {
  if (Text.empty())
    return imageError("synthetic text is empty");
  uint64_t TextEnd = 0;
  if (!checkedAdd(TextAddress, Text.size(), TextEnd))
    return imageError("synthetic text range overflows");
  (void)TextEnd;

  ProgramImage Result;
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
  return Result;
}

llvm::Expected<ProgramImage> buildProgramImage(const BinaryImage &Image,
                                               const Metadata &Metadata,
                                               const SBFVMConfig &Config) {
  if (llvm::Error Error = validateVMConfig(Config))
    return std::move(Error);

  const Section *Text = Image.getSectionByName(kTextSectionName);
  if (!Text)
    Text = Image.getTextSection();
  if (!Text || Text->Data.empty())
    return imageError("loaded image has no non-empty text section");
  if (Text->VA != Metadata.TextVM.Address ||
      Text->Data.size() != Metadata.TextVM.Size)
    return imageError("text section disagrees with loader metadata");

  ProgramImage Result;
  if (Metadata.StrictLayout) {
    if (Metadata.RodataVM.Size != 0) {
      const Section *Rodata = Image.getSectionByName(kRodataSectionName);
      if (!Rodata || Rodata->VA != Metadata.RodataVM.Address ||
          Rodata->Data.size() != Metadata.RodataVM.Size)
        return imageError("rodata section disagrees with loader metadata");
      ProgramRegion Region;
      Region.Address = Rodata->VA;
      Region.Bytes = Rodata->Data;
      Region.Kind = ProgramRegionKind::ReadOnly;
      Region.DataVisible = true;
      Region.Name = kRodataSegmentName.str();
      Region.Sections.push_back({Rodata->Name, 0, Rodata->Data.size(), false});
      Result.Regions.push_back(std::move(Region));
    }

    ProgramRegion Region;
    Region.Address = Text->VA;
    Region.Bytes = Text->Data;
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
      if (!isLegacyReadOnlySection(Section.Name) || Section.Data.empty())
        continue;
      uint64_t End = 0;
      if (!checkedAdd(Section.VA, Section.Data.size(), End))
        return imageError("legacy section range overflows");
      Lowest = std::min<uint64_t>(Lowest, Section.VA);
      Highest = std::max(Highest, End);
      Selected.push_back(&Section);
    }
    if (Selected.empty() || Lowest == std::numeric_limits<uint64_t>::max())
      return imageError("legacy ELF has no runtime read-only sections");

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
    for (const Section *Section : Selected) {
      const size_t Offset = static_cast<size_t>(Section->VA - Base);
      if (!rangeInBounds(Offset, Section->Data.size(), Region.Bytes.size()))
        return imageError("legacy section lies outside merged read-only range");
      std::copy(Section->Data.begin(), Section->Data.end(),
                Region.Bytes.begin() + static_cast<ptrdiff_t>(Offset));
      Region.Sections.push_back({Section->Name, Offset, Section->Data.size(),
                                 Section->isExecutable()});
    }
    Result.Regions.push_back(std::move(Region));
  }

  if (Metadata.TextVM.Size > std::numeric_limits<size_t>::max())
    return imageError("text size exceeds the host address space");
  if (llvm::Error Error = Result.finalize(
          Metadata.TextVM.Address, static_cast<size_t>(Metadata.TextVM.Size)))
    return std::move(Error);

  if (Metadata.StrictLayout)
    return Result;

  auto MutableSlice =
      [&](va_t Address,
          size_t Size) -> llvm::Expected<llvm::MutableArrayRef<uint8_t>> {
    for (ProgramRegion &Region : Result.Regions) {
      if (!Region.contains(Address, Size))
        continue;
      const size_t Offset = static_cast<size_t>(Address - Region.Address);
      return llvm::MutableArrayRef(Region.Bytes).slice(Offset, Size);
    }
    return imageError("relocation target is outside the runtime image");
  };

  const va_t TextAddress = Metadata.TextVM.Address;
  const uint64_t TextSize = Metadata.TextVM.Size;
  std::map<uint32_t, std::string> CallKeys;
  for (const RelocationEntry &Relocation : Image.Relocations) {
    const RelocationInfo *Info = getRelocationInfo(Relocation.Type);
    if (!Info)
      return relocationError(Relocation, "unsupported relocation type");
    if (!isRelocationAllowedForVersion(*Info, Metadata.Version))
      return relocationError(Relocation,
                             "relocation is unavailable for this SBF version");
    if (Info->SymbolRequirement == RelocationSymbolRequirement::Required &&
        Relocation.SymbolName.empty())
      return relocationError(Relocation, "required symbol name is missing");
    if (Info->Width == 0 || Info->Width % kBitsPerByte != 0)
      return relocationError(Relocation,
                             "relocation metadata has an invalid bit width");
    const size_t FieldWidth = Info->Width / kBitsPerByte;
    const bool IsText = Relocation.Address >= TextAddress &&
                        Relocation.Address - TextAddress < TextSize;

    if (!IsText) {
      if (!hasTarget(Info->Targets, RelocationTargetKind::ReadOnlyData) ||
          Info->FieldLayout != RelocationFieldLayout::SplitLDDWOrData ||
          Info->Purpose != RelocationPurpose::Relative ||
          FieldWidth != sizeof(uint64_t))
        return relocationError(Relocation,
                               "relocation is invalid outside program text");
      uint64_t ImmediateAddress = 0;
      if (!checkedAdd(Relocation.Address, kImmediateOffset, ImmediateAddress))
        return relocationError(Relocation, "relocation field overflows");
      auto EncodedAddress = MutableSlice(ImmediateAddress, sizeof(uint32_t));
      if (!EncodedAddress)
        return EncodedAddress.takeError();
      uint64_t Value = llvm::support::endian::read32le(EncodedAddress->data());
      if (!checkedAdd(kBytecodeStart, Value, Value))
        return relocationError(Relocation, "relative address overflows");
      auto Field = MutableSlice(Relocation.Address, FieldWidth);
      if (!Field)
        return Field.takeError();
      llvm::support::endian::write64le(Field->data(), Value);
      continue;
    }

    const uint64_t TextOffset = Relocation.Address - TextAddress;
    if (TextOffset % kInstructionSize != 0)
      return relocationError(Relocation,
                             "text relocation is not instruction-aligned");
    auto Instruction = MutableSlice(Relocation.Address, kInstructionSize);
    if (!Instruction)
      return Instruction.takeError();

    if (Info->FieldLayout == RelocationFieldLayout::CallImmediate) {
      if (!hasTarget(Info->Targets, RelocationTargetKind::TextCall))
        return relocationError(Relocation,
                               "relocation cannot target a call instruction");
      if (Info->Purpose != RelocationPurpose::Call ||
          FieldWidth != sizeof(uint32_t))
        return relocationError(Relocation,
                               "call relocation metadata is inconsistent");
      const OpcodeInfo *OpcodeInfo =
          getOpcodeInfo((*Instruction)[kOpcodeOffset], Metadata.Version);
      if (!OpcodeInfo || OpcodeInfo->ID != Opcode::CALL_IMM)
        return relocationError(Relocation,
                               "call relocation does not reference a CALL");
      uint32_t Key = hashSymbolName(Relocation.SymbolName);
      std::string Identity =
          (llvm::Twine("symbol:") + Relocation.SymbolName).str();
      if (const Symbol *Symbol = Image.findSymbol(Relocation.SymbolName)) {
        if (Symbol->IsFunc && Symbol->Addr >= TextAddress &&
            Symbol->Addr - TextAddress < TextSize &&
            (Symbol->Addr - TextAddress) % kInstructionSize == 0) {
          const size_t TargetSlot = static_cast<size_t>(
              (Symbol->Addr - TextAddress) / kInstructionSize);
          Key = legacyFunctionKey(TargetSlot, Symbol->Name);
          Identity = (llvm::Twine("slot:") + llvm::Twine(TargetSlot)).str();
        }
      }
      const auto [Existing, Inserted] = CallKeys.emplace(Key, Identity);
      if (!Inserted && Existing->second != Identity)
        return relocationError(Relocation, "function/syscall hash collision");
      llvm::support::endian::write32le(Instruction->data() + kImmediateOffset,
                                       Key);
      continue;
    }

    if ((Info->FieldLayout != RelocationFieldLayout::SplitLDDWImmediate &&
         Info->FieldLayout != RelocationFieldLayout::SplitLDDWOrData) ||
        !hasTarget(Info->Targets, RelocationTargetKind::TextLDDW))
      return relocationError(Relocation,
                             "relocation cannot target an LDDW instruction");
    if (FieldWidth != sizeof(uint64_t))
      return relocationError(Relocation,
                             "LDDW relocation metadata is inconsistent");
    auto Pair =
        MutableSlice(Relocation.Address, kLDDWSlotCount * kInstructionSize);
    if (!Pair)
      return relocationError(
          Relocation, llvm::Twine(kDoubleWordBitWidth) +
                          "-bit text relocation is missing an LDDW slot");
    const OpcodeInfo *OpcodeInfo =
        getOpcodeInfo((*Pair)[kOpcodeOffset], Metadata.Version);
    if (!OpcodeInfo || OpcodeInfo->ID != Opcode::LDDW ||
        (*Pair)[kInstructionSize + kOpcodeOffset] != 0)
      return relocationError(
          Relocation, llvm::Twine(kDoubleWordBitWidth) +
                          "-bit text relocation does not reference LDDW");

    const uint32_t Low =
        llvm::support::endian::read32le(Pair->data() + kImmediateOffset);
    const uint32_t High = llvm::support::endian::read32le(
        Pair->data() + kInstructionSize + kImmediateOffset);
    uint64_t Value = static_cast<uint64_t>(Low) |
                     (static_cast<uint64_t>(High) << kWordBitWidth);
    switch (Info->Purpose) {
    case RelocationPurpose::Absolute: {
      const Symbol *Symbol = Image.findSymbol(Relocation.SymbolName);
      if (!Symbol)
        return relocationError(Relocation,
                               "absolute relocation has no resolvable symbol");
      const uint64_t RawSymbol = Symbol->Addr >= kBytecodeStart
                                     ? Symbol->Addr - kBytecodeStart
                                     : Symbol->Addr;
      if (!checkedAdd(RawSymbol, Low, Value))
        return relocationError(Relocation, "relocated address overflows");
      if (Value < kMemoryRegionSize &&
          !checkedAdd(kBytecodeStart, Value, Value))
        return relocationError(Relocation, "relocated address overflows");
      break;
    }
    case RelocationPurpose::Relative:
      if (Value == 0)
        return relocationError(Relocation,
                               "relative relocation has a zero value");
      if (Value < kMemoryRegionSize &&
          !checkedAdd(kBytecodeStart, Value, Value))
        return relocationError(Relocation, "relative address overflows");
      break;
    case RelocationPurpose::Call:
      return relocationError(Relocation,
                             "relocation cannot be applied to program text");
    }
    llvm::support::endian::write32le(Pair->data() + kImmediateOffset,
                                     static_cast<uint32_t>(Value));
    llvm::support::endian::write32le(
        Pair->data() + kInstructionSize + kImmediateOffset,
        static_cast<uint32_t>(Value >> kWordBitWidth));
  }
  return Result;
}

} // namespace neverd::sbf
