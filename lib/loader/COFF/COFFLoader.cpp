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
#include "neverd/Support/BinaryEncoding.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/FunctionDiscovery.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>

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
    Img.Entry = ImageBase + PE32Plus->AddressOfEntryPoint;
    Img.Bits = Bitness::Bits64;
  } else if (const auto *PE32 = Obj.getPE32Header()) {
    if (PE32->AddressOfEntryPoint > InvalidVA - ImageBase)
      return llvm::make_error<llvm::StringError>(
          "coff: entry point address overflows",
          llvm::inconvertibleErrorCode());
    Img.Entry = ImageBase + PE32->AddressOfEntryPoint;
    Img.Bits = Bitness::Bits32;
  } else {
    IsRelocatable = true;
    Img.Entry = 0;
    Img.Bits = (Img.Arch == Arch::X64 || Img.Arch == Arch::AArch64)
                   ? Bitness::Bits64
                   : Bitness::Bits32;
  }
  if (!IsRelocatable)
    Img.Entry = normalizeCodeAddress(Img.Entry, Img.Arch, Img.Mode);
  Img.Base = ImageBase;

  // --- Sections ---
  for (const SectionRef &SecRef : Obj.sections()) {
    const coff_section *CoffSec = Obj.getCOFFSection(SecRef);
    if (CoffSec->VirtualAddress > InvalidVA - ImageBase)
      return llvm::make_error<llvm::StringError>(
          "coff: section address overflows",
          llvm::inconvertibleErrorCode());
    va_t SectionVA = ImageBase + CoffSec->VirtualAddress;
    if (CoffSec->VirtualSize > InvalidVA - SectionVA)
      return llvm::make_error<llvm::StringError>(
          "coff: section range overflows",
          llvm::inconvertibleErrorCode());

    Segment Seg;
    if (auto NameOrErr = Obj.getSectionName(CoffSec))
      Seg.Name = NameOrErr->str();
    else
      llvm::consumeError(NameOrErr.takeError());

    Seg.VA = SectionVA;
    Seg.Size = CoffSec->VirtualSize;
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
    Sec.Size = CoffSec->VirtualSize;
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
    for (const auto &Reloc : SecRef.relocations()) {
      RelocationEntry RE;
      RE.Address = Reloc.getOffset();
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
      va_t ApplyVA = ImageBase + ApplySec->VirtualAddress;
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
        auto SymAddrOrErr = SymIt->getAddress();
        if (SymAddrOrErr)
          SymVal = *SymAddrOrErr;
        else {
          llvm::consumeError(SymAddrOrErr.takeError());
          continue;
        }

        va_t S = SymVal;
        va_t P = ApplyVA + RAddr;

        if (RAddr >= ApplySeg->Data.size())
          continue;

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
            int32_t Val = static_cast<int32_t>(S - (P + 4 + Extra));
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
          } else if (RType == IMAGE_REL_AMD64_ADDR64) {
            if (RAddr + 8 > ApplySeg->Data.size())
              continue;
            uint64_t Val = S;
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 8);
          } else if (RType == IMAGE_REL_AMD64_ADDR32NB) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            uint32_t Val = static_cast<uint32_t>(S - ImageBase);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
          } else if (RType == IMAGE_REL_AMD64_ADDR32) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            uint32_t Val = static_cast<uint32_t>(S);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
          }
        } else if (Img.Arch == Arch::X86) {
          if (RType == IMAGE_REL_I386_REL32) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int32_t Val = static_cast<int32_t>(S - (P + 4));
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
          } else if (RType == IMAGE_REL_I386_DIR32) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            uint32_t Val = static_cast<uint32_t>(S);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
          }
        } else if (Img.Arch == Arch::AArch64) {
          if (RType == IMAGE_REL_ARM64_BRANCH26) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int64_t Disp = static_cast<int64_t>(S - P);
            uint32_t Insn;
            std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
            Insn = (Insn & 0xFC000000u) |
                   (static_cast<uint32_t>((Disp >> 2) & 0x03FFFFFFu));
            std::memcpy(ApplySeg->Data.data() + RAddr, &Insn, 4);
          } else if (RType == IMAGE_REL_ARM64_PAGEBASE_REL21) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int64_t PageDelta =
                static_cast<int64_t>((S & ~0xFFFULL) - (P & ~0xFFFULL));
            uint32_t ImmLo = static_cast<uint32_t>((PageDelta >> 12) & 0x3);
            uint32_t ImmHi = static_cast<uint32_t>((PageDelta >> 14) & 0x7FFFF);
            uint32_t Insn;
            std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
            Insn = (Insn & 0x9F00001Fu) | (ImmLo << 29) | (ImmHi << 5);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Insn, 4);
          } else if (RType == IMAGE_REL_ARM64_PAGEOFFSET_12A ||
                     RType == IMAGE_REL_ARM64_PAGEOFFSET_12L) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            uint32_t Imm12 = static_cast<uint32_t>(S & 0xFFF);
            uint32_t Insn;
            std::memcpy(&Insn, ApplySeg->Data.data() + RAddr, 4);
            Insn = (Insn & 0xFFC003FFu) | ((Imm12 & 0xFFF) << 10);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Insn, 4);
          }
        } else if (Img.Arch == Arch::ARM) {
          if (RType == IMAGE_REL_ARM_ADDR32) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            uint32_t Val = static_cast<uint32_t>(S);
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
          } else if (RType == IMAGE_REL_ARM_REL32) {
            if (RAddr + 4 > ApplySeg->Data.size())
              continue;
            int32_t Val = static_cast<int32_t>(S - (P + 4));
            std::memcpy(ApplySeg->Data.data() + RAddr, &Val, 4);
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
  for (auto I = Obj.delay_import_directory_begin(),
            E = Obj.delay_import_directory_end();
       I != E; ++I) {
    llvm::StringRef DLLName;
    if (auto Err = I->getName(DLLName)) {
      llvm::consumeError(std::move(Err));
      continue;
    }
    std::string DelayModule = (DLLName + kDelayImportSuffix).str();
    for (auto SI = I->imported_symbol_begin(), SE = I->imported_symbol_end();
         SI != SE; ++SI)
      coff_loader::addImportedSymbol(SI, DelayModule, 0, Img);
  }

  // --- Base Relocation Table (.reloc) ---
  coff_loader::parseBaseRelocations(Obj, Img, ImageBase);

  // --- Debug Directory (PDB path) ---
  coff_loader::parseDebugDirectory(Obj, Img);

  // --- Exceptions (.pdata) ---
  coff_loader::parseExceptions(Obj, Img, ImageBase);

  // --- COFF Symbol table ---
  coff_loader::parseSymbolTable(Obj, Img, ImageBase);

  // --- TLS callbacks ---
  coff_loader::parseTLSDirectory(Obj, Img, ImageBase);

  // --- Load Configuration (security cookie, CF Guard) ---
  coff_loader::parseLoadConfiguration(Obj, Img, ImageBase);

  runPostLoadDiscovery(Img, "coff: loaded " + Path.filename().string());
  return Img;
}

} // namespace neverd
