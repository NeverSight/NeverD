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

#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/Error.h"
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

  Img.Arch = tripleToArch(Obj.getArch());
  if (Img.Arch == Arch::Unknown)
    return llvm::make_error<llvm::StringError>("elf: unsupported architecture",
                                               llvm::inconvertibleErrorCode());

  Img.Bits = ELFT::Is64Bits ? Bitness::Bits64 : Bitness::Bits32;
  Img.Entry = EH.e_entry;
  if (Img.Arch == Arch::ARM)
    Img.Entry = clearThumbBit(Img.Entry);

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

  if (llvm::Error E = elf_loader::detail::buildSegments<ELFT>(
          ELF, *SectionsOr, ShStrTab, Data, Size, SecBase, IsRelocatable, Img))
    return E;

  if (llvm::Error E = elf_loader::detail::buildSections<ELFT>(
          *SectionsOr, ShStrTab, Data, Size, SecBase, IsRelocatable, Img))
    return E;

  elf_loader::detail::collectRelocations<ELFT>(ELF, *SectionsOr, ShStrTab, Data,
                                               Size, IsRelocatable, Img);

  // --- Apply relocations for relocatable objects (.o files) ---
  // PC-relative references in .text to .rodata need fixup so the lifter
  // sees correct displacements for constant pool loads.
  if (IsRelocatable)
    elf_loader::detail::applyRelocations<ELFT>(ELF, *SectionsOr, Data, Size,
                                               SecBase, IsRelocatable, Img);

  elf_loader::detail::collectSymbols<ELFT>(ELF, *SectionsOr, Size, SecBase,
                                           IsRelocatable, Img);

  // --- .dynamic ---
  for (const Elf_Shdr &SH : *SectionsOr) {
    if (SH.sh_type == SHT_DYNAMIC) {
      elf_loader::parseDynamic(ELF, SH, Data, Size, Img);
      break;
    }
  }

  // --- .rela.plt / .rel.plt imports ---
  elf_loader::parsePLTImports(ELF, *SectionsOr, Data, Size, Img);

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
  for (const Elf_Shdr &SH : *SectionsOr) {
    if (elf_loader::detail::getSectionName<ELFT>(ShStrTab, SH) !=
        section_names::elf::EhFrameHdr)
      continue;
    elf_loader::addFunctionsFromEhFrameHdr(Data, Size, SH, Img);
    break;
  }

  if (!IsRelocatable && EH.e_entry != 0)
    Img.recordRuntimeFunction(Img.Entry);

  return llvm::Error::success();
}

} // anonymous namespace

llvm::Expected<BinaryImage> ELFLoader::load(const std::filesystem::path &Path) {
  BinaryImage Img;
  auto BufOrErr = readFileInto(Path, Img, BinaryFormat::ELF);
  if (!BufOrErr)
    return BufOrErr.takeError();
  auto &Buf = *BufOrErr;

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

  auto ObjOrErr =
      llvm::object::ObjectFile::createObjectFile(Buf->getMemBufferRef());
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

  runPostLoadDiscovery(Img, "elf: loaded " + Path.filename().string());
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

} // namespace neverd
