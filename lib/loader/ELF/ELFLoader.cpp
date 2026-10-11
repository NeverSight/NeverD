//===- ELFLoader.cpp - ELF binary format loader -------------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implements ELF loading via LLVM's Object/ELF API (ELFFile and
/// ELFObjectFile).  Supports ELF32/ELF64 relocatable and executable
/// objects on little-endian hosts.
///
/// This file drives the load in order; the individual phases -- address-space
/// layout, section and symbol tables, and relocations -- live beside it in
/// ELFLoaderSegments.cpp, ELFLoaderSections.cpp, and
/// ELFLoaderRelocations.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/ELF/ELFLoader.h"

#include "ELFLoaderDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/DWARF/ItaniumEH.h"
#include "neverd/loader/ELF/ARMEHABI.h"
#include "neverd/loader/ELF/ELFLoaderUtils.h"
#include "neverd/loader/ELF/EhFrameHdr.h"
#include "neverd/loader/ELF/SBFELFLoader.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/Go/GoRuntimeEH.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/loader/ObjC/ObjCEH.h"
#include "neverd/loader/Rust/RustEH.h"
#include "neverd/support/FilePath.h"

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"

#include <optional>
#include <set>
#include <vector>

#define DEBUG_TYPE "neverd-elf-loader"

namespace neverd {

namespace {

Arch tripleToArch(llvm::Triple::ArchType TA) {
  switch (TA) {
  case llvm::Triple::x86_64:
    return Arch::X64;
  case llvm::Triple::aarch64:
    return Arch::AArch64;
  case llvm::Triple::x86:
    return Arch::X86;
  case llvm::Triple::arm:
  case llvm::Triple::thumb:
    return Arch::ARM;
  default:
    return Arch::Unknown;
  }
}

template <typename ELFT>
llvm::Error loadELF(llvm::object::ELFObjectFile<ELFT> &Obj, BinaryImage &Img) {
  using namespace llvm::ELF;
  using Elf_Ehdr = typename ELFT::Ehdr;
  using Elf_Shdr = typename ELFT::Shdr;

  const auto &ELF = Obj.getELFFile();
  const uint8_t *Data = reinterpret_cast<const uint8_t *>(Obj.getData().data());
  size_t Size = Obj.getData().size();

  const Elf_Ehdr &EH = ELF.getHeader();
  bool IsRelocatable = (EH.e_type == ET_REL);
  Img.IsRelocatable = IsRelocatable;
  Img.LoadsAtLinkAddress = EH.e_type == ET_EXEC;

  Img.Arch = tripleToArch(Obj.getArch());
  if (Img.Arch == Arch::Unknown)
    return llvm::make_error<llvm::StringError>("elf: unsupported architecture",
                                               llvm::inconvertibleErrorCode());

  Img.Bits = ELFT::Is64Bits ? Bitness::Bits64 : Bitness::Bits32;
  Img.Entry = EH.e_entry;
  if (Img.Arch == Arch::ARM) {
    Img.Entry = clearThumbBit(Img.Entry);
    Img.ARMModeRequiresLocalEvidence = true;
  }

  auto SectionsOr = ELF.sections();
  if (!SectionsOr)
    return SectionsOr.takeError();

  auto ShStrTabOr = ELF.getSectionStringTable(*SectionsOr);
  if (!ShStrTabOr)
    return ShStrTabOr.takeError();
  llvm::StringRef ShStrTab = *ShStrTabOr;

  // Relocatable .o files often use sh_addr==0 for every SHF_ALLOC section.
  // sh_offset is a file position, not a VMA — map each section to a unique
  // base.
  std::vector<va_t> SecBase(SectionsOr->size(), 0);
  if (IsRelocatable) {
    va_t Next = 0;
    for (uint32_t I = 0; I < SectionsOr->size(); ++I) {
      const Elf_Shdr &SH = (*SectionsOr)[I];
      if (!(SH.sh_flags & SHF_ALLOC) || SH.sh_size == 0)
        continue;
      if (SH.sh_addr != 0) {
        SecBase[I] = static_cast<va_t>(SH.sh_addr);
        if (SH.sh_size > InvalidVA - SecBase[I])
          return llvm::make_error<llvm::StringError>(
              "elf: section address range overflows",
              llvm::inconvertibleErrorCode());
        va_t End = SecBase[I] + static_cast<va_t>(SH.sh_size);
        if (End > Next)
          Next = End;
      } else {
        uint64_t A = SH.sh_addralign ? SH.sh_addralign : 1;
        if ((A & (A - 1)) != 0 || Next > InvalidVA - (A - 1))
          return llvm::make_error<llvm::StringError>(
              "elf: invalid section alignment", llvm::inconvertibleErrorCode());
        Next = (Next + A - 1) & ~(A - 1);
        if (SH.sh_size > InvalidVA - Next)
          return llvm::make_error<llvm::StringError>(
              "elf: synthesized section range overflows",
              llvm::inconvertibleErrorCode());
        SecBase[I] = Next;
        Next += static_cast<va_t>(SH.sh_size);
      }
    }
  }

  // The undefined and common symbols of a relocatable object take addresses
  // past every section, beside the GOT entries its GOT references reach.
  elf_loader::detail::ObjectExterns Externs;
  if (IsRelocatable) {
    va_t ImageEnd = 0;
    for (uint32_t I = 0; I < SectionsOr->size(); ++I) {
      const Elf_Shdr &SH = (*SectionsOr)[I];
      if ((SH.sh_flags & SHF_ALLOC) && SH.sh_size != 0)
        ImageEnd = std::max<va_t>(ImageEnd,
                                  SecBase[I] + static_cast<va_t>(SH.sh_size));
    }
    auto PlanOr = elf_loader::detail::planObjectExterns<ELFT>(
        ELF, *SectionsOr, Data, Size, Img.Arch, ImageEnd);
    if (!PlanOr)
      return PlanOr.takeError();
    Externs = std::move(*PlanOr);
  }

  if (llvm::Error E = elf_loader::detail::buildSegments<ELFT>(
          ELF, *SectionsOr, ShStrTab, Data, Size, SecBase, IsRelocatable, Img))
    return E;

  if (llvm::Error E = elf_loader::detail::buildSections<ELFT>(
          *SectionsOr, ShStrTab, Data, Size, SecBase, IsRelocatable, Img))
    return E;
  if (IsRelocatable)
    elf_loader::detail::addObjectExterns<ELFT>(ELF, *SectionsOr, SecBase,
                                               Externs, Img);

  // An image whose section header table was stripped still has the tables
  // its dynamic linker and unwinder read through the program headers; the
  // parsers below read those.  The image publishes none of them as a
  // section, since a segment holding sections takes its code from them.
  elf_loader::detail::DynamicSections<ELFT> Reconstructed;
  llvm::ArrayRef<Elf_Shdr> Tables = *SectionsOr;
  llvm::StringRef TableNames = ShStrTab;
  if (!IsRelocatable && SectionsOr->empty()) {
    Reconstructed =
        elf_loader::detail::reconstructDynamicSections<ELFT>(ELF, Img);
    Tables = Reconstructed.Headers;
    TableNames = Reconstructed.Names;
  }

  elf_loader::detail::collectRelocations<ELFT>(ELF, Tables, TableNames, Data,
                                               Size, IsRelocatable, Img);

  // --- Apply relocations for relocatable objects (.o files) ---
  // PC-relative references in .text to .rodata need fixup so the lifter
  // sees correct displacements for constant pool loads.
  if (IsRelocatable) {
    if (llvm::Error E = elf_loader::detail::applyRelocations<ELFT>(
            ELF, *SectionsOr, Data, Size, SecBase, IsRelocatable, Externs, Img))
      return E;
  } else
    elf_loader::detail::applyDynamicRelativeRelocations<ELFT>(ELF, Tables, Data,
                                                              Size, Img);

  if (llvm::Error E = elf_loader::detail::collectSymbols<ELFT>(
          ELF, Tables, *SectionsOr, Size, SecBase, IsRelocatable,
          Externs.CommonSlots, Img))
    return E;

  // --- .dynamic ---
  for (const Elf_Shdr &SH : Tables) {
    if (SH.sh_type == SHT_DYNAMIC) {
      elf_loader::parseDynamic(ELF, Tables, SH, Data, Size, Img);
      break;
    }
  }

  // --- .rela.plt / .rel.plt imports ---
  elf_loader::parsePLTImports(ELF, Tables, TableNames, Data, Size, Img);

  // --- Loader-invoked lifecycle arrays and legacy constructor sections ---
  elf_loader::parseRuntimeSections(Img);

  // Every PLT family is dynamic-linker machinery even when a malformed or
  // stripped indirect-symbol table prevents exact stub-to-import recovery.
  for (const Section &Sec : Img.Sections) {
    llvm::StringRef Name = Sec.Name;
    if (Name == section_names::elf::Plt || Name == section_names::elf::Iplt ||
        Name.starts_with(section_names::elf::PltPrefix))
      Img.recordImportStubRange(Sec.VA, Sec.Size);
  }
  // A range says a veneer is one; it does not say which import it forwards to.
  // On ARM that question has an answer in the veneer's own instructions, and
  // it has to be asked: a personality routine is named by veneer address in
  // every `.ARM.extab` entry that has one.
  elf_loader::recordARMPLTVeneers(Img);

  // --- .got / .got.plt ---
  elf_loader::parseGOTEntries(ELF, *SectionsOr, Data, Size, Img);

  // --- PT_NOTE (build-id, ABI tag) ---
  elf_loader::parseNotes(ELF, Data, Size, Img);

  // --- .eh_frame_hdr ---
  for (const Elf_Shdr &SH : Tables) {
    if (elf_loader::detail::getSectionName<ELFT>(TableNames, SH) !=
        section_names::elf::EhFrameHdr)
      continue;
    elf_loader::addFunctionsFromEhFrameHdr(Data, Size, SH, Img);
    break;
  }

  if (!IsRelocatable && EH.e_entry != 0)
    Img.recordRuntimeFunction(Img.Entry);

  return llvm::Error::success();
}

/// How the load dialog names an e_machine value.
std::string elfMachineName(uint16_t Machine) {
  switch (Machine) {
#define NEVERD_ELF_MACHINE(Value, Name)                                        \
  case Value:                                                                  \
    return Name;
#include "neverd/loader/ELF/ELFNames.def"
  }
  if (const llvm::StringRef Name =
          llvm::ELF::convertEMachineToArchName(Machine);
      !Name.empty() && Name != "None")
    return Name.str();
  return llvm::formatv("machine {0}", Machine).str();
}

/// How the load dialog names an e_type value.
std::string elfTypeName(uint16_t Type) {
  switch (Type) {
#define NEVERD_ELF_TYPE(Value, Name)                                           \
  case Value:                                                                  \
    return Name;
#include "neverd/loader/ELF/ELFNames.def"
  }
  return llvm::formatv("type {0}", Type).str();
}

} // anonymous namespace

llvm::Expected<BinaryImage> ELFLoader::load(const std::filesystem::path &Path) {
  auto BufOrErr = readFileBuffer(Path, BinaryFormat::ELF);
  if (!BufOrErr)
    return BufOrErr.takeError();
  return loadBuffer((*BufOrErr)->getMemBufferRef());
}

llvm::Expected<BinaryImage>
ELFLoader::loadBuffer(llvm::MemoryBufferRef Buffer) {
  BinaryImage Img;
  initializeImage(Buffer, Img, BinaryFormat::ELF);

  if (Img.Raw.size() < llvm::ELF::EI_NIDENT)
    return llvm::make_error<llvm::StringError>("elf: file too small",
                                               llvm::inconvertibleErrorCode());

  const uint8_t *Data = Img.Raw.data();
  if (!std::equal(Data, Data + 4,
                  reinterpret_cast<const uint8_t *>(llvm::ELF::ElfMagic)))
    return llvm::make_error<llvm::StringError>("elf: bad magic",
                                               llvm::inconvertibleErrorCode());

  if (Data[llvm::ELF::EI_DATA] != llvm::ELF::ELFDATA2LSB)
    return llvm::make_error<llvm::StringError>("elf: big-endian not supported",
                                               llvm::inconvertibleErrorCode());

  auto IsSBF = loadSBFELF(Img);
  if (!IsSBF)
    return IsSBF.takeError();
  if (*IsSBF)
    return Img;

  auto ObjOrErr = llvm::object::ObjectFile::createObjectFile(Buffer);
  if (!ObjOrErr)
    return ObjOrErr.takeError();

  auto *Obj = ObjOrErr->get();
  // Produced rather than assigned: `Error::operator=` refuses to overwrite a
  // value that has not been checked, so seeding a variable with
  // `Error::success()` and assigning the real result over it aborts before the
  // result is ever looked at.
  auto Loaded = [&]() -> llvm::Error {
    if (auto *ELF64 = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(Obj))
      return loadELF(*ELF64, Img);
    if (auto *ELF32 = llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(Obj))
      return loadELF(*ELF32, Img);
    return llvm::make_error<llvm::StringError>("elf: unsupported ELF class",
                                               llvm::inconvertibleErrorCode());
  }();
  if (Loaded)
    return std::move(Loaded);

  // An ET_EXEC synthesized from an unlinked object can still contain
  // unresolved branch relocations. Only follow the encoded targets after a
  // program-header-backed link has fixed their executable addresses.
  const bool HasProgramHeaders = [&] {
    if (auto *ELF32 = llvm::dyn_cast<llvm::object::ELF32LEObjectFile>(Obj))
      return ELF32->getELFFile().getHeader().e_phnum != 0;
    if (auto *ELF64 = llvm::dyn_cast<llvm::object::ELF64LEObjectFile>(Obj))
      return ELF64->getELFFile().getHeader().e_phnum != 0;
    return false;
  }();
  if (llvm::Error E = applyARMFunctionModeHints(Img, ARMFunctionModes))
    return std::move(E);
  if (Img.Arch == Arch::ARM && !Img.IsRelocatable && HasProgramHeaders)
    if (llvm::Error E = discoverARMReachableModes(Img))
      return std::move(E);
  if (llvm::Error E = verifyARMFunctionModeHints(Img, ARMFunctionModes))
    return std::move(E);

  // Frame extents are function boundaries the discovery heuristics must not
  // guess inside, as PE .pdata ranges already are when they run.
  dwarf_eh::recordFrameExtents(Img);
  runPostLoadDiscovery(
      Img, "elf: loaded " + pathToUTF8(std::filesystem::u8path(
                                           Buffer.getBufferIdentifier().str())
                                           .filename()));
  // Classified before any table is read: a decoder that finds an Itanium LSDA
  // cannot tell from the table alone whether its cleanup pads are C++
  // destructors or Rust drop glue, and the evidence that settles it is the
  // image's symbols and sections rather than anything in the table.
  Img.ExceptionMetadata.Runtime = detectLanguageRuntime(Img);
  // Personality routines are usually reached through a dynamically bound slot,
  // so language-table decoding waits until imports and veneers are known.
  dwarf_eh::parseItaniumExceptions(Img);
  // ARM32 keeps its unwinding in an index of its own rather than in DWARF, and
  // a C++ frame's language data inside that index's table rather than in a
  // section.  Runs after the DWARF reader because an image built with
  // `-fasynchronous-unwind-tables` has both, and the two agree about the
  // frames they share.
  arm_ehabi::parseARMEHABIExceptions(Img);
  // Go emits no DWARF frame information for its own functions, so a Go image
  // reaches this point with nothing recovered.  Its metadata lives in the
  // runtime's own table, which is present in every container format.
  go_loader::parseGoExceptions(Img);
  // Rust and Objective-C share the Itanium tables above rather than emitting
  // their own, so their readings of them run last, over records that are
  // already normalized.
  rust_eh::parseRustExceptions(Img);
  objc_eh::parseObjCExceptions(Img);
  return Img;
}

void ELFLoader::identify(llvm::MemoryBufferRef Buffer,
                         std::vector<LoadCandidate> &Rows) {
  using namespace llvm::ELF;
  const auto Bytes = llvm::arrayRefFromStringRef(Buffer.getBuffer());
  if (Bytes.size() < EI_NIDENT + 4 ||
      !std::equal(Bytes.begin(), Bytes.begin() + 4,
                  reinterpret_cast<const uint8_t *>(ElfMagic)))
    return;
  LoadCandidate Row;
  Row.Row = LoadRow::ELF;
  Row.Format = BinaryFormat::ELF;
  const bool Is64 = Bytes[EI_CLASS] == ELFCLASS64;
  Row.Bits = Is64 ? 64 : 32;
  Row.BigEndian = Bytes[EI_DATA] == ELFDATA2MSB;
  // e_type and e_machine follow e_ident in either class.
  const auto read16 = [&](size_t Offset) {
    return Row.BigEndian
               ? llvm::support::endian::read16be(Bytes.data() + Offset)
               : llvm::support::endian::read16le(Bytes.data() + Offset);
  };
  const uint16_t Type = read16(EI_NIDENT), Machine = read16(EI_NIDENT + 2);
  const std::string MachineName = elfMachineName(Machine);
  Row.Description =
      llvm::formatv(getLoadRowText(LoadRow::ELF).data(), Is64 ? "64" : "",
                    MachineName, elfTypeName(Type))
          .str();
  // The checks load() makes, in its order.
  bool SupportedSBF = false;
  if (Bytes[EI_DATA] != ELFDATA2LSB) {
    Row.Reason = getLoadReasonText(LoadReason::BigEndian).str();
  } else if (isSBFELF(Bytes, SupportedSBF)) {
    Row.TheArch = Arch::SBF;
    Row.Loadable = SupportedSBF;
    if (!SupportedSBF)
      Row.Reason = getLoadReasonText(LoadReason::SBFVersion).str();
  } else if (Bytes[EI_CLASS] != ELFCLASS32 && !Is64) {
    Row.Reason = getLoadReasonText(LoadReason::Class).str();
  } else if (auto Obj = llvm::object::ObjectFile::createObjectFile(Buffer);
             !Obj) {
    Row.Reason = llvm::formatv(getLoadReasonText(LoadReason::Malformed).data(),
                               llvm::toString(Obj.takeError()))
                     .str();
  } else {
    Row.TheArch = tripleToArch((*Obj)->getArch());
    Row.Loadable = Row.TheArch != Arch::Unknown;
    if (!Row.Loadable)
      Row.Reason =
          llvm::formatv(getLoadReasonText(LoadReason::Processor).data(),
                        MachineName)
              .str();
  }
  Rows.push_back(std::move(Row));
}

} // namespace neverd
