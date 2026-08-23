//===- COFFLoader.cpp - COFF/PE binary format loader --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements COFF/PE loading using LLVM's Object/COFF API: header
/// validation, section mapping, import/export table resolution, exception
/// directory processing, and heuristic function discovery (IAT thunks,
/// padding boundaries, data function pointers).
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/COFF/COFFLoader.h"

#include "neverd/Limits.h"
#include "neverd/loader/COFF/COFFDelphiEH.h"
#include "neverd/loader/COFF/COFFException.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/DWARF/ItaniumEH.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/Go/GoRuntimeEH.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/ObjC/ObjCEH.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/loader/Rust/RustEH.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <optional>
#include <vector>

#define DEBUG_TYPE "neverd-coff-loader"

namespace neverd {

namespace {

using namespace llvm::COFF;
using namespace llvm::object;

Arch machineToArch(uint16_t Machine) {
  switch (Machine) {
  case IMAGE_FILE_MACHINE_AMD64:
    return Arch::X64;
  case IMAGE_FILE_MACHINE_I386:
    return Arch::X86;
  case IMAGE_FILE_MACHINE_ARM64:
    return Arch::AArch64;
  case IMAGE_FILE_MACHINE_ARMNT:
    return Arch::ARM;
  default:
    return Arch::Unknown;
  }
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// COFFLoader public interface
// ---------------------------------------------------------------------------

llvm::Expected<BinaryImage>
COFFLoader::load(const std::filesystem::path &Path) {
  BinaryImage Img;
  auto BufOrErr = readFileInto(Path, Img, BinaryFormat::COFF);
  if (!BufOrErr)
    return BufOrErr.takeError();
  auto &Buf = *BufOrErr;

  auto ObjOrErr = COFFObjectFile::create(Buf->getMemBufferRef());
  if (!ObjOrErr)
    return ObjOrErr.takeError();
  const auto &Obj = **ObjOrErr;

  Img.Arch = machineToArch(Obj.getMachine());
  if (Img.Arch == Arch::Unknown)
    return llvm::make_error<llvm::StringError>(
        "coff: unsupported machine 0x" + llvm::utohexstr(Obj.getMachine()),
        llvm::inconvertibleErrorCode());
  if (Img.Arch == Arch::ARM)
    Img.Mode = InstructionMode::Thumb;

  uint64_t ImageBase = Obj.getImageBase();
  bool IsRelocatable = false;
  if (const auto *PE32Plus = Obj.getPE32PlusHeader()) {
    if (PE32Plus->AddressOfEntryPoint > InvalidVA - ImageBase)
      return llvm::make_error<llvm::StringError>(
          "coff: entry point address overflows",
          llvm::inconvertibleErrorCode());
    Img.Entry = PE32Plus->AddressOfEntryPoint == 0
                    ? 0
                    : ImageBase + PE32Plus->AddressOfEntryPoint;
    Img.Bits = Bitness::Bits64;
  } else if (const auto *PE32 = Obj.getPE32Header()) {
    if (PE32->AddressOfEntryPoint > InvalidVA - ImageBase)
      return llvm::make_error<llvm::StringError>(
          "coff: entry point address overflows",
          llvm::inconvertibleErrorCode());
    Img.Entry = PE32->AddressOfEntryPoint == 0
                    ? 0
                    : ImageBase + PE32->AddressOfEntryPoint;
    Img.Bits = Bitness::Bits32;
  } else {
    IsRelocatable = true;
    Img.Entry = 0;
    Img.Bits = (Img.Arch == Arch::X64 || Img.Arch == Arch::AArch64)
                   ? Bitness::Bits64
                   : Bitness::Bits32;
  }
  Img.IsRelocatable = IsRelocatable;
  if (!IsRelocatable)
    Img.Entry = normalizeCodeAddress(Img.Entry, Img.Arch, Img.Mode);
  Img.Base = ImageBase;

  // COFF object-file section headers are required to keep VirtualAddress at
  // zero. Build a private mapped layout instead of mutating that format-native
  // invariant or allowing every section to alias VA zero. This same table is
  // the authority for section mapping, symbol values, and relocation S.
  std::vector<va_t> SectionVAs(Obj.getNumberOfSections() + 1, InvalidVA);
  std::vector<uint64_t> SectionSizes(Obj.getNumberOfSections() + 1, 0);
  va_t NextRelocatableVA = 0x1000;
  for (const SectionRef &SecRef : Obj.sections()) {
    const unsigned SectionID = Obj.getSectionID(SecRef);
    if (SectionID < SectionSizes.size())
      SectionSizes[SectionID] = Obj.getSectionSize(Obj.getCOFFSection(SecRef));
  }
  if (IsRelocatable)
    for (const SymbolRef &SymRef : Obj.symbols()) {
      COFFSymbolRef Sym = Obj.getCOFFSymbol(SymRef);
      if (Sym.getSectionNumber() <= 0)
        continue;
      const size_t SectionID = static_cast<size_t>(Sym.getSectionNumber());
      const coff_aux_section_definition *Definition =
          Sym.getSectionDefinition();
      if (Definition && SectionID < SectionSizes.size())
        SectionSizes[SectionID] =
            std::max<uint64_t>(SectionSizes[SectionID], Definition->Length);
    }

  // --- Sections ---
  for (const SectionRef &SecRef : Obj.sections()) {
    const coff_section *CoffSec = Obj.getCOFFSection(SecRef);
    const unsigned SectionID = Obj.getSectionID(SecRef);
    if (SectionID >= SectionVAs.size())
      return llvm::make_error<llvm::StringError>(
          "coff: section index is out of range",
          llvm::inconvertibleErrorCode());
    const uint64_t SectionSize = IsRelocatable ? SectionSizes[SectionID]
                                               : uint64_t(CoffSec->VirtualSize);
    va_t SectionVA = 0;
    if (IsRelocatable) {
      const uint64_t Alignment = std::max<uint64_t>(CoffSec->getAlignment(), 1);
      const uint64_t Remainder = NextRelocatableVA % Alignment;
      const uint64_t Padding = Remainder == 0 ? 0 : Alignment - Remainder;
      if (Padding > InvalidVA - NextRelocatableVA)
        return llvm::make_error<llvm::StringError>(
            "coff: synthetic section address overflows",
            llvm::inconvertibleErrorCode());
      SectionVA = NextRelocatableVA + Padding;
      const uint64_t MappedSpan = std::max<uint64_t>(SectionSize, uint64_t(1));
      if (MappedSpan > InvalidVA - SectionVA)
        return llvm::make_error<llvm::StringError>(
            "coff: synthetic section range overflows",
            llvm::inconvertibleErrorCode());
      NextRelocatableVA = SectionVA + MappedSpan;
    } else {
      if (CoffSec->VirtualAddress > InvalidVA - ImageBase)
        return llvm::make_error<llvm::StringError>(
            "coff: section address overflows", llvm::inconvertibleErrorCode());
      SectionVA = ImageBase + CoffSec->VirtualAddress;
    }
    if (SectionSize > InvalidVA - SectionVA)
      return llvm::make_error<llvm::StringError>(
          "coff: section range overflows", llvm::inconvertibleErrorCode());
    SectionVAs[SectionID] = SectionVA;

    Segment Seg;
    if (auto NameOrErr = Obj.getSectionName(CoffSec))
      Seg.Name = NameOrErr->str();
    else
      llvm::consumeError(NameOrErr.takeError());

    Seg.VA = SectionVA;
    Seg.Size = SectionSize;
    Seg.FileOff = CoffSec->PointerToRawData;
    Seg.FileSz = CoffSec->SizeOfRawData;
    Seg.Flags = coffFlagsToNd(CoffSec->Characteristics);

    llvm::ArrayRef<uint8_t> Contents;
    if (auto Err = Obj.getSectionContents(CoffSec, Contents)) {
      llvm::consumeError(std::move(Err));
    } else {
      Seg.Data.assign(Contents.begin(), Contents.end());
      // VirtualSize is untrusted; only zero-fill up to the cap (see
      // kMaxSegmentZeroFill) so a crafted size cannot force a huge allocation.
      if (CoffSec->VirtualSize > CoffSec->SizeOfRawData &&
          CoffSec->VirtualSize <= limits::kMaxSegmentZeroFill)
        Seg.Data.resize(CoffSec->VirtualSize, 0);
    }

    Img.Segments.push_back(std::move(Seg));

    Section Sec;
    Sec.Name = Img.Segments.back().Name;
    Sec.VA = SectionVA;
    Sec.Size = SectionSize;
    Sec.FileOff = CoffSec->PointerToRawData;
    Sec.FileSz = CoffSec->SizeOfRawData;
    Sec.Type = CoffSec->Characteristics;
    uint32_t AlignField =
        (CoffSec->Characteristics & llvm::COFF::IMAGE_SCN_ALIGN_MASK) >>
        kCOFFAlignShift;
    Sec.Alignment = AlignField > 0 ? (1u << (AlignField - 1)) : 1;
    Sec.Flags = coffFlagsToNd(CoffSec->Characteristics);
    if (!Contents.empty())
      Sec.Data.assign(Contents.begin(), Contents.end());
    Img.Sections.push_back(std::move(Sec));
  }

  // --- COFF Relocations ---
  for (const SectionRef &SecRef : Obj.sections()) {
    const unsigned SectionID = Obj.getSectionID(SecRef);
    if (SectionID >= SectionVAs.size() || SectionVAs[SectionID] == InvalidVA)
      continue;
    for (const auto &Reloc : SecRef.relocations()) {
      RelocationEntry RE;
      if (Reloc.getOffset() > InvalidVA - SectionVAs[SectionID])
        continue;
      RE.Address = SectionVAs[SectionID] + Reloc.getOffset();
      RE.Type = Reloc.getType();
      auto SymOrErr = Reloc.getSymbol();
      if (SymOrErr != Obj.symbol_end()) {
        auto NameOrErr = SymOrErr->getName();
        if (NameOrErr)
          RE.SymbolName = NameOrErr->str();
        else
          llvm::consumeError(NameOrErr.takeError());
      }
      if (auto NameOrErr = SecRef.getName())
        RE.SectionName = NameOrErr->str();
      else
        llvm::consumeError(NameOrErr.takeError());
      Img.Relocations.push_back(std::move(RE));
    }
  }

  // --- Apply relocations for .obj files ---
  if (IsRelocatable) {
    for (const SectionRef &SecRef : Obj.sections()) {
      const coff_section *ApplySec = Obj.getCOFFSection(SecRef);
      if (!ApplySec)
        continue;
      const unsigned ApplySectionID = Obj.getSectionID(SecRef);
      if (ApplySectionID >= SectionVAs.size() ||
          SectionVAs[ApplySectionID] == InvalidVA)
        continue;
      va_t ApplyVA = SectionVAs[ApplySectionID];
      Segment *ApplySeg = nullptr;
      for (auto &Seg : Img.Segments) {
        if (Seg.VA == ApplyVA && !Seg.Data.empty()) {
          ApplySeg = &Seg;
          break;
        }
      }
      if (!ApplySeg)
        continue;

      for (const auto &Reloc : SecRef.relocations()) {
        uint64_t RAddr = Reloc.getOffset();
        uint32_t RType = Reloc.getType();
        auto SymIt = Reloc.getSymbol();
        if (SymIt == Obj.symbol_end())
          continue;

        uint64_t SymVal = 0;
        va_t SymOwnerVA = InvalidVA;
        COFFSymbolRef CoffSym = Obj.getCOFFSymbol(*SymIt);
        if (CoffSym.getSectionNumber() > 0) {
          const size_t SymbolSection =
              static_cast<size_t>(CoffSym.getSectionNumber());
          if (SymbolSection >= SectionVAs.size() ||
              SectionVAs[SymbolSection] == InvalidVA ||
              CoffSym.getValue() > InvalidVA - SectionVAs[SymbolSection])
            continue;
          SymOwnerVA = SectionVAs[SymbolSection];
          SymVal = SectionVAs[SymbolSection] + CoffSym.getValue();
        } else {
          auto SymAddrOrErr = SymIt->getAddress();
          if (SymAddrOrErr) {
            SymVal = *SymAddrOrErr;
          } else {
            llvm::consumeError(SymAddrOrErr.takeError());
            continue;
          }
        }

        va_t S = SymVal;
        va_t P = ApplyVA + RAddr;

        if (RAddr >= ApplySeg->Data.size())
          continue;

        // COFF absolute relocations apply the signed, field-width in-place
        // addend A to the symbol address S. Keep the linker's wrapping encoded
        // value separate from the full address used for provenance: a negative
        // addend is valid, but only publish provenance when S + A is a
        // representable VA.
        auto AddSignedAddend = [](uint64_t Base,
                                  int64_t Addend) -> std::optional<uint64_t> {
          if (Addend >= 0) {
            const uint64_t Magnitude = static_cast<uint64_t>(Addend);
            if (Magnitude > InvalidVA - Base)
              return std::nullopt;
            return Base + Magnitude;
          }

          const uint64_t Magnitude = static_cast<uint64_t>(-(Addend + 1)) + 1;
          if (Magnitude > Base)
            return std::nullopt;
          return Base - Magnitude;
        };

        auto EncodeSignedRel32 = [](uint64_t Target,
                                    uint64_t Base) -> std::optional<int32_t> {
          if (Target >= Base) {
            const uint64_t Delta = Target - Base;
            if (Delta > static_cast<uint64_t>(INT32_MAX))
              return std::nullopt;
            return static_cast<int32_t>(Delta);
          }

          const uint64_t Magnitude = Base - Target;
          if (Magnitude > static_cast<uint64_t>(INT32_MAX) + 1)
            return std::nullopt;
          if (Magnitude == static_cast<uint64_t>(INT32_MAX) + 1)
            return INT32_MIN;
          return -static_cast<int32_t>(Magnitude);
        };

        auto SignedDifference = [](uint64_t Left,
                                   uint64_t Right) -> std::optional<int64_t> {
          if (Left >= Right) {
            const uint64_t Delta = Left - Right;
            if (Delta > static_cast<uint64_t>(INT64_MAX))
              return std::nullopt;
            return static_cast<int64_t>(Delta);
          }
          const uint64_t Magnitude = Right - Left;
          if (Magnitude > static_cast<uint64_t>(INT64_MAX) + 1)
            return std::nullopt;
          if (Magnitude == static_cast<uint64_t>(INT64_MAX) + 1)
            return INT64_MIN;
          return -static_cast<int64_t>(Magnitude);
        };

        auto SignExtend = [](uint64_t Value, unsigned Bits) -> int64_t {
          const uint64_t Sign = uint64_t(1) << (Bits - 1);
          const uint64_t Mask = (uint64_t(1) << Bits) - 1;
          Value &= Mask;
          if ((Value & Sign) == 0)
            return static_cast<int64_t>(Value);
          return static_cast<int64_t>(Value) -
                 static_cast<int64_t>(uint64_t(1) << Bits);
        };

        // A complete absolute address stored in data is a pointer slot, while
        // the same field inside an instruction is occurrence provenance for
        // exactly that operand. On AMD64, ADDR32 is narrower than a pointer,
        // so it may only publish an exact executable-field occurrence; it is
        // never a pointer-table slot. Image-relative ADDR32NB fields do not
        // call this helper because their encoded RVA is not a complete VA.
        auto RecordAbsoluteField = [&](uint64_t EncodedValue, uint64_t TargetVA,
                                       uint64_t TargetOwnerVA,
                                       size_t FieldWidth) {
          if (TargetOwnerVA == InvalidVA)
            return;
          if (FieldWidth == Img.getPointerSize()) {
            recordAbsolutePointerRelocation(Img, P, TargetVA, TargetOwnerVA);
            return;
          }
          if (!Img.hasExecutableCodeOwnerAt(P))
            return;

          const Segment *TargetSeg = Img.getSegmentFor(TargetOwnerVA);
          if (!TargetSeg)
            return;
          const Section *TargetSec = Img.getSectionFor(TargetOwnerVA);
          const bool TargetReadable =
              TargetSec ? TargetSec->isReadable() : TargetSeg->isReadable();
          const bool TargetWritable =
              TargetSec ? TargetSec->isWritable() &&
                              !section_names::isReadOnlyAfterRelocSectionName(
                                  TargetSec->Name) &&
                              !section_names::isReadOnlyAfterRelocSectionName(
                                  TargetSec->SegmentName)
                        : TargetSeg->isWritable();
          const bool TargetExecutable =
              Img.hasExecutableCodeOwnerAt(TargetOwnerVA);
          const va_t OwnerBegin = TargetSec ? TargetSec->VA : TargetSeg->VA;
          const uint64_t OwnerSize =
              TargetSec ? TargetSec->Size : TargetSeg->Size;
          if (!TargetReadable || OwnerSize > InvalidVA - OwnerBegin)
            return;
          const va_t OwnerEnd = OwnerBegin + OwnerSize;
          if (TargetVA < OwnerBegin ||
              (TargetExecutable ? TargetVA >= OwnerEnd : TargetVA > OwnerEnd))
            return;

          const RelocatedAddressField Field{EncodedValue, TargetVA,
                                            static_cast<uint8_t>(FieldWidth),
                                            OwnerBegin};
          if (TargetExecutable) {
            Img.DataAddressRelocOperands.erase(P);
            Img.CodeAddressRelocOperands[P] = Field;
            Img.CodeRefTargets.insert(
                normalizeCodeAddress(TargetVA, Img.Arch, Img.Mode));
            return;
          }

          Img.CodeAddressRelocOperands.erase(P);
          Img.DataAddressRelocOperands[P] = Field;
          if (TargetWritable)
            Img.WritableRelocDataAddrs.insert(TargetVA);
          else
            Img.RelocDataAddrs.insert(TargetVA);
        };

        auto RecordMaterializedTarget = [&](uint64_t TargetVA,
                                            uint64_t TargetOwnerVA,
                                            bool IsAddressValue) {
          if (TargetOwnerVA == InvalidVA)
            return;
          const Segment *TargetSeg = Img.getSegmentFor(TargetOwnerVA);
          if (!TargetSeg)
            return;
          const Section *TargetSec = Img.getSectionFor(TargetOwnerVA);
          const uint64_t OwnerBegin = TargetSec ? TargetSec->VA : TargetSeg->VA;
          const uint64_t OwnerSize =
              TargetSec ? TargetSec->Size : TargetSeg->Size;
          if (OwnerSize > InvalidVA - OwnerBegin)
            return;
          const uint64_t OwnerEnd = OwnerBegin + OwnerSize;
          const bool TargetExecutable =
              Img.hasExecutableCodeOwnerAt(TargetOwnerVA);
          if (TargetVA < OwnerBegin ||
              (TargetExecutable ? TargetVA >= OwnerEnd : TargetVA > OwnerEnd))
            return;
          if (IsAddressValue && Img.hasExecutableCodeOwnerAt(P))
            Img.InstructionAddressMaterializations[P] = {TargetVA,
                                                         TargetOwnerVA};
          if (TargetExecutable) {
            if (IsAddressValue)
              Img.CodeRefTargets.insert(
                  normalizeCodeAddress(TargetVA, Img.Arch, Img.Mode));
            return;
          }
          const bool TargetReadable =
              TargetSec ? TargetSec->isReadable() : TargetSeg->isReadable();
          if (!TargetReadable)
            return;
          const bool TargetWritable =
              TargetSec ? TargetSec->isWritable() &&
                              !section_names::isReadOnlyAfterRelocSectionName(
                                  TargetSec->Name) &&
                              !section_names::isReadOnlyAfterRelocSectionName(
                                  TargetSec->SegmentName)
                        : TargetSeg->isWritable();
          if (TargetWritable)
            Img.WritableRelocDataAddrs.insert(TargetVA);
          else
            Img.RelocDataAddrs.insert(TargetVA);
        };

        if (Img.Arch == Arch::X64) {
          if (RType == IMAGE_REL_AMD64_REL32 ||
              RType == IMAGE_REL_AMD64_REL32_1 ||
              RType == IMAGE_REL_AMD64_REL32_2 ||
              RType == IMAGE_REL_AMD64_REL32_3 ||
              RType == IMAGE_REL_AMD64_REL32_4 ||
              RType == IMAGE_REL_AMD64_REL32_5) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int32_t Extra = 0;
            if (RType >= IMAGE_REL_AMD64_REL32_1)
              Extra = static_cast<int32_t>(RType - IMAGE_REL_AMD64_REL32);
            int32_t InPlace = 0;
            std::memcpy(&InPlace, ApplySeg->Data.data() + RAddr, 4);
            const uint64_t Bias = 4 + static_cast<uint64_t>(Extra);
            if (Bias > InvalidVA - P)
              continue;
            auto Target = AddSignedAddend(S, InPlace);
            if (!Target)
              continue;
            auto Val = EncodeSignedRel32(*Target, P + Bias);
            if (!Val)
              continue;
            std::memcpy(ApplySeg->Data.data() + RAddr, &*Val, 4);
          } else if (RType == IMAGE_REL_AMD64_ADDR64) {
            if (RAddr + 8 > ApplySeg->Data.size())
              continue;
            uint64_t InPlace = 0;
            std::memcpy(&InPlace, ApplySeg->Data.data() + RAddr, 8);
            uint64_t Val = S + InPlace;
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 8);
            int64_t SignedAddend = 0;
            std::memcpy(&SignedAddend, &InPlace, sizeof(SignedAddend));
            if (auto FullTarget = AddSignedAddend(S, SignedAddend))
              RecordAbsoluteField(Val, *FullTarget, SymOwnerVA, 8);
          } else if (RType == IMAGE_REL_AMD64_ADDR32NB) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            uint32_t InPlace = 0;
            std::memcpy(&InPlace, ApplySeg->Data.data() + RAddr, 4);
            uint32_t Val = static_cast<uint32_t>(S - ImageBase + InPlace);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
          } else if (RType == IMAGE_REL_AMD64_ADDR32) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int32_t InPlace = 0;
            std::memcpy(&InPlace, ApplySeg->Data.data() + RAddr, 4);
            const uint32_t Val =
                static_cast<uint32_t>(S) + static_cast<uint32_t>(InPlace);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
            if (auto FullTarget = AddSignedAddend(S, InPlace);
                FullTarget && *FullTarget <= UINT32_MAX)
              RecordAbsoluteField(Val, *FullTarget, SymOwnerVA, 4);
          }
        } else if (Img.Arch == Arch::X86) {
          if (RType == IMAGE_REL_I386_REL32) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int32_t InPlace = 0;
            std::memcpy(&InPlace, ApplySeg->Data.data() + RAddr, 4);
            if (P > InvalidVA - 4)
              continue;
            auto Target = AddSignedAddend(S, InPlace);
            if (!Target)
              continue;
            auto Val = EncodeSignedRel32(*Target, P + 4);
            if (!Val)
              continue;
            std::memcpy(ApplySeg->Data.data() + RAddr, &*Val, 4);
          } else if (RType == IMAGE_REL_I386_DIR32) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int32_t InPlace = 0;
            std::memcpy(&InPlace, ApplySeg->Data.data() + RAddr, 4);
            const uint32_t Val =
                static_cast<uint32_t>(S) + static_cast<uint32_t>(InPlace);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
            if (auto FullTarget = AddSignedAddend(S, InPlace);
                FullTarget && *FullTarget <= UINT32_MAX)
              RecordAbsoluteField(Val, *FullTarget, SymOwnerVA, 4);
          }
        } else if (Img.Arch == Arch::AArch64) {
          if (RType == IMAGE_REL_ARM64_BRANCH26) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            uint32_t Insn;
            std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
            if ((Insn & 0xfc000000u) != 0x14000000u &&
                (Insn & 0xfc000000u) != 0x94000000u)
              continue;
            const int64_t Addend = SignExtend(Insn & 0x03ffffffu, 26) * 4;
            auto Target = AddSignedAddend(S, Addend);
            if (!Target)
              continue;
            auto Disp = SignedDifference(*Target, P);
            if (!Disp || (*Disp & 3) != 0 || *Disp < -(int64_t(1) << 27) ||
                *Disp > (int64_t(1) << 27) - 4)
              continue;
            Insn = (Insn & 0xfc000000u) |
                   (static_cast<uint32_t>(*Disp >> 2) & 0x03ffffffu);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Insn, 4);
          } else if (RType == IMAGE_REL_ARM64_PAGEBASE_REL21) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            uint32_t Insn;
            std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
            if ((Insn & 0x9f000000u) != 0x90000000u)
              continue;
            const uint32_t EncodedPages =
                (((Insn >> 5) & 0x7ffffu) << 2) | ((Insn >> 29) & 0x3u);
            const int64_t Addend = SignExtend(EncodedPages, 21) * 0x1000;
            auto Target = AddSignedAddend(S, Addend);
            if (!Target)
              continue;
            auto PageDelta =
                SignedDifference(*Target & ~0xfffULL, P & ~0xfffULL);
            if (!PageDelta || *PageDelta < -(int64_t(1) << 32) ||
                *PageDelta > (int64_t(1) << 32) - 0x1000)
              continue;
            const uint32_t ImmLo =
                static_cast<uint32_t>(*PageDelta >> 12) & 0x3u;
            const uint32_t ImmHi =
                static_cast<uint32_t>(*PageDelta >> 14) & 0x7ffffu;
            Insn = (Insn & 0x9f00001fu) | (ImmLo << 29) | (ImmHi << 5);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Insn, 4);
            if (Img.hasExecutableCodeOwnerAt(P) &&
                Img.relocatedTargetBelongsToOwner(*Target, SymOwnerVA))
              Img.InstructionPageAddressFragments[P] = {*Target, SymOwnerVA};
          } else if (RType == IMAGE_REL_ARM64_PAGEOFFSET_12A ||
                     RType == IMAGE_REL_ARM64_PAGEOFFSET_12L) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            uint32_t Insn;
            std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
            unsigned Shift = 0;
            if (RType == IMAGE_REL_ARM64_PAGEOFFSET_12A) {
              if ((Insn & 0x7f800000u) != 0x11000000u ||
                  (Insn & (1u << 22)) != 0)
                continue;
            } else {
              if ((Insn & 0x3b000000u) != 0x39000000u)
                continue;
              Shift = Insn >> 30;
              if (Shift == 0 && (Insn & 0x04800000u) == 0x04800000u)
                Shift = 4;
            }
            const uint64_t Addend = static_cast<uint64_t>((Insn >> 10) & 0xfffu)
                                    << Shift;
            if (Addend > static_cast<uint64_t>(INT64_MAX))
              continue;
            auto Target = AddSignedAddend(S, static_cast<int64_t>(Addend));
            if (!Target)
              continue;
            const uint64_t PageOffset = *Target & 0xfffULL;
            if ((PageOffset & ((uint64_t(1) << Shift) - 1)) != 0)
              continue;
            const uint32_t Imm12 = static_cast<uint32_t>(PageOffset >> Shift);
            Insn = (Insn & 0xffc003ffu) | ((Imm12 & 0xfffu) << 10);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Insn, 4);
            RecordMaterializedTarget(*Target, SymOwnerVA,
                                     RType == IMAGE_REL_ARM64_PAGEOFFSET_12A);
          } else if (RType == IMAGE_REL_ARM64_ADDR64) {
            if (RAddr + 8 > ApplySeg->Data.size())
              continue;
            uint64_t InPlace = 0;
            std::memcpy(&InPlace, ApplySeg->Data.data() + RAddr, 8);
            const uint64_t Val = S + InPlace;
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 8);
            int64_t SignedAddend = 0;
            std::memcpy(&SignedAddend, &InPlace, sizeof(SignedAddend));
            if (auto FullTarget = AddSignedAddend(S, SignedAddend))
              RecordAbsoluteField(Val, *FullTarget, SymOwnerVA, 8);
          } else if (RType == IMAGE_REL_ARM64_ADDR32) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int32_t InPlace = 0;
            std::memcpy(&InPlace, ApplySeg->Data.data() + RAddr, 4);
            const uint32_t Val =
                static_cast<uint32_t>(S) + static_cast<uint32_t>(InPlace);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
            if (auto FullTarget = AddSignedAddend(S, InPlace);
                FullTarget && *FullTarget <= UINT32_MAX)
              RecordAbsoluteField(Val, *FullTarget, SymOwnerVA, 4);
          } else if (RType == IMAGE_REL_ARM64_ADDR32NB) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int32_t InPlace = 0;
            std::memcpy(&InPlace, ApplySeg->Data.data() + RAddr, 4);
            auto FullTarget = AddSignedAddend(S, InPlace);
            if (!FullTarget || *FullTarget < ImageBase ||
                *FullTarget - ImageBase > UINT32_MAX)
              continue;
            const uint32_t Val = static_cast<uint32_t>(*FullTarget - ImageBase);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
          }
        } else if (Img.Arch == Arch::ARM) {
          if (RType == IMAGE_REL_ARM_ADDR32) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int32_t InPlace = 0;
            std::memcpy(&InPlace, ApplySeg->Data.data() + RAddr, 4);
            const uint32_t Val =
                static_cast<uint32_t>(S) + static_cast<uint32_t>(InPlace);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
            if (auto FullTarget = AddSignedAddend(S, InPlace);
                FullTarget && *FullTarget <= UINT32_MAX)
              RecordAbsoluteField(Val, *FullTarget, SymOwnerVA, 4);
          } else if (RType == IMAGE_REL_ARM_REL32) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int32_t InPlace = 0;
            std::memcpy(&InPlace, ApplySeg->Data.data() + RAddr, 4);
            auto Target = AddSignedAddend(S, InPlace);
            if (!Target || P > InvalidVA - 4)
              continue;
            auto Val = EncodeSignedRel32(*Target, P + 4);
            if (!Val)
              continue;
            std::memcpy(ApplySeg->Data.data() + RAddr, &*Val, 4);
          } else if (RType == IMAGE_REL_ARM_MOV32T) {
            if (RAddr + 8 > ApplySeg->Data.size())
              continue;
            uint16_t MovWHi = 0, MovWLo = 0, MovTHi = 0, MovTLo = 0;
            std::memcpy(&MovWHi, ApplySeg->Data.data() + RAddr, 2);
            std::memcpy(&MovWLo, ApplySeg->Data.data() + RAddr + 2, 2);
            std::memcpy(&MovTHi, ApplySeg->Data.data() + RAddr + 4, 2);
            std::memcpy(&MovTLo, ApplySeg->Data.data() + RAddr + 6, 2);
            if ((MovWHi & 0xfbf0u) != 0xf240u || (MovTHi & 0xfbf0u) != 0xf2c0u)
              continue;
            auto DecodeThumbImm16 = [](uint16_t Hi, uint16_t Lo) {
              return static_cast<uint32_t>(
                  ((Hi & 0x000fu) << 12) | ((Hi & 0x0400u) << 1) |
                  ((Lo & 0x7000u) >> 4) | (Lo & 0x00ffu));
            };
            const uint32_t Encoded = DecodeThumbImm16(MovWHi, MovWLo) |
                                     (DecodeThumbImm16(MovTHi, MovTLo) << 16);
            auto Target = AddSignedAddend(S, static_cast<int32_t>(Encoded));
            if (!Target || *Target > UINT32_MAX)
              continue;
            auto EncodeThumbImm16 = [](uint16_t &Hi, uint16_t &Lo,
                                       uint16_t Value) {
              Hi = static_cast<uint16_t>((Hi & ~0x040fu) |
                                         ((Value >> 12) & 0x000fu) |
                                         ((Value >> 1) & 0x0400u));
              Lo = static_cast<uint16_t>((Lo & ~0x70ffu) |
                                         ((Value << 4) & 0x7000u) |
                                         (Value & 0x00ffu));
            };
            EncodeThumbImm16(MovWHi, MovWLo, static_cast<uint16_t>(*Target));
            EncodeThumbImm16(MovTHi, MovTLo,
                             static_cast<uint16_t>(*Target >> 16));
            std::memcpy(ApplySeg->Data.data() + RAddr, &MovWHi, 2);
            std::memcpy(ApplySeg->Data.data() + RAddr + 2, &MovWLo, 2);
            std::memcpy(ApplySeg->Data.data() + RAddr + 4, &MovTHi, 2);
            std::memcpy(ApplySeg->Data.data() + RAddr + 6, &MovTLo, 2);
            RecordMaterializedTarget(*Target, SymOwnerVA, true);
          } else if (RType == IMAGE_REL_ARM_MOV32A) {
            if (RAddr + 8 > ApplySeg->Data.size())
              continue;
            uint32_t MovW = 0, MovT = 0;
            std::memcpy(&MovW, ApplySeg->Data.data() + RAddr, 4);
            std::memcpy(&MovT, ApplySeg->Data.data() + RAddr + 4, 4);
            if ((MovW & 0x0ff00000u) != 0x03000000u ||
                (MovT & 0x0ff00000u) != 0x03400000u)
              continue;
            auto DecodeARMImm16 = [](uint32_t Insn) {
              return ((Insn >> 4) & 0xf000u) | (Insn & 0x0fffu);
            };
            const uint32_t Encoded =
                DecodeARMImm16(MovW) | (DecodeARMImm16(MovT) << 16);
            auto Target = AddSignedAddend(S, static_cast<int32_t>(Encoded));
            if (!Target || *Target > UINT32_MAX)
              continue;
            auto EncodeARMImm16 = [](uint32_t Insn, uint16_t Value) {
              return (Insn & ~0x000f0fffu) |
                     ((static_cast<uint32_t>(Value) & 0xf000u) << 4) |
                     (static_cast<uint32_t>(Value) & 0x0fffu);
            };
            MovW = EncodeARMImm16(MovW, static_cast<uint16_t>(*Target));
            MovT = EncodeARMImm16(MovT, static_cast<uint16_t>(*Target >> 16));
            std::memcpy(ApplySeg->Data.data() + RAddr, &MovW, 4);
            std::memcpy(ApplySeg->Data.data() + RAddr + 4, &MovT, 4);
            RecordMaterializedTarget(*Target, SymOwnerVA, true);
          }
        }
      }
    }
  }

  // --- Imports ---
  uint32_t PtrSize = Img.getPointerSize();
  for (auto I = Obj.import_directory_begin(), E = Obj.import_directory_end();
       I != E; ++I) {
    llvm::StringRef DLLName;
    if (auto Err = I->getName(DLLName)) {
      llvm::consumeError(std::move(Err));
      continue;
    }
    uint32_t IATRVA = 0;
    if (auto Err = I->getImportAddressTableRVA(IATRVA)) {
      llvm::consumeError(std::move(Err));
      continue;
    }
    uint32_t Idx = 0;
    for (auto SI = I->imported_symbol_begin(), SE = I->imported_symbol_end();
         SI != SE; ++SI) {
      uint64_t IATOffset =
          static_cast<uint64_t>(IATRVA) + static_cast<uint64_t>(Idx) * PtrSize;
      if (IATOffset > InvalidVA - ImageBase)
        break;
      coff_loader::addImportedSymbol(SI, DLLName, ImageBase + IATOffset, Img);
      ++Idx;
    }
  }

  // --- Exports ---
  for (auto I = Obj.export_directory_begin(), E = Obj.export_directory_end();
       I != E; ++I) {
    llvm::StringRef Name;
    if (auto Err = I->getSymbolName(Name)) {
      llvm::consumeError(std::move(Err));
      continue;
    }
    uint32_t RVA = 0;
    if (auto Err = I->getExportRVA(RVA)) {
      llvm::consumeError(std::move(Err));
      continue;
    }
    uint32_t Ord = 0;
    if (auto Err = I->getOrdinal(Ord)) {
      llvm::consumeError(std::move(Err));
      continue;
    }

    if (RVA > InvalidVA - ImageBase)
      continue;
    va_t RawAddr = ImageBase + RVA;
    const Segment *TargetSeg = Img.getSegmentFor(RawAddr);
    va_t Addr = TargetSeg && TargetSeg->isExecutable()
                    ? normalizeCodeAddress(RawAddr, Img.Arch, Img.Mode)
                    : RawAddr;

    Export Exp;
    Exp.Name = Name.str();
    Exp.Ordinal = Ord;
    Exp.Addr = Addr;
    Img.Exports.push_back(std::move(Exp));
  }

  // --- Delay Imports ---
  coff_loader::parseDelayImports(Obj, Img);

  // --- Base Relocation Table (.reloc) ---
  coff_loader::parseBaseRelocations(Obj, Img, ImageBase);

  // --- Debug Directory (PDB path) ---
  coff_loader::parseDebugDirectory(Obj, Img);

  // --- Exceptions (.pdata) ---
  coff_loader::parseExceptions(Obj, Img, ImageBase);

  // --- COFF Symbol table ---
  coff_loader::parseSymbolTable(Obj, Img, ImageBase, SectionVAs);

  // --- TLS callbacks ---
  coff_loader::parseTLSDirectory(Obj, Img, ImageBase);

  // --- Load Configuration (security cookie, CF Guard) ---
  coff_loader::parseLoadConfiguration(Obj, Img, ImageBase);

  if (!IsRelocatable && Img.Entry != 0)
    Img.recordRuntimeFunction(Img.Entry);

  runPostLoadDiscovery(Img, "coff: loaded " + Path.filename().string());
  // Classified before any table is read.  A PE is the format where schema and
  // language diverge most: Delphi and MSVC share the registration chain, Rust
  // and C++ share the `FuncInfo`, and a MinGW image carries Itanium tables
  // inside a PE.  None of those are separable from the table alone.
  Img.ExceptionMetadata.Runtime = detectLanguageRuntime(Img);
  // A Go PE has an exception directory covering only the cgo and runtime
  // assembly that Windows itself unwinds; everything Go compiled is described
  // by the pclntab instead.  Reading it first is what names the one routine Go
  // installs as a personality, which lives in the pclntab and in no PE symbol
  // table, and would otherwise leave that record permanently unclassified.
  go_loader::parseGoExceptions(Img);
  // Handler names may sit behind executable import veneers found during
  // post-load discovery.  Decode language tables only after those mappings and
  // the COFF symbol table are both available.
  coff_loader::resolveExceptionHandlers(Img);
  // x86-32 has no exception directory; its tables are reachable only from the
  // prologue that installs the FS:[0] registration record, so recovery needs
  // the discovered function bodies that scan runs over.  Delphi shares that
  // mechanism but nothing else, and its descriptors would read as unclassified
  // SEH handlers, so its frames are claimed first.
  coff_loader::parseDelphiExceptions(Img);
  coff_loader::parseX86RegistrationExceptions(Img);
  // A MinGW or `*-pc-windows-gnu` image carries both table families: the PE
  // exception directory Windows itself unwinds through, and a `.eh_frame` with
  // the Itanium LSDAs GCC's personality reads.  Decoding only the first would
  // report every C++ frame in such an image as having no handler at all.
  dwarf_eh::parseItaniumExceptions(Img);
  // Rust and Objective-C share whichever of those two table families their
  // target uses, so their readings of them run last, over records that are
  // already normalized.
  rust_eh::parseRustExceptions(Img);
  objc_eh::parseObjCExceptions(Img);
  return Img;
}

} // namespace neverd
