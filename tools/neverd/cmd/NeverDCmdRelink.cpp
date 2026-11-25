//===- NeverDCmdRelink.cpp - Rebuild a MIPS ELF from link inputs ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "../NeverDCLI.h"

#include "neverd/support/AtomicOutput.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/FilePath.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/JSON.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/WithColor.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::object;

namespace neverd::cli {
namespace {

namespace fs = std::filesystem;

struct LinkManifest {
  std::vector<std::string> Objects;
  std::string Original, Replacement, Function, Layout, SectionLayout, Section;
};

Expected<std::unique_ptr<MemoryBuffer>> readFile(StringRef Path) {
  auto Buffer = MemoryBuffer::getFileAsStream(Path);
  if (!Buffer)
    return createFileError(Path, Buffer.getError());
  return std::move(*Buffer);
}

Expected<LinkManifest> readManifest(StringRef Path, bool Inplace) {
  auto Buffer = readFile(Path);
  if (!Buffer)
    return Buffer.takeError();
  auto Value = json::parse((*Buffer)->getBuffer());
  if (!Value)
    return createFileError(Path, Value.takeError());
  LinkManifest M;
  json::Path::Root Root;
  json::ObjectMapper Fields(*Value, Root);
  if (!Fields || !Fields.map("objects", M.Objects) ||
      !Fields.map("original", M.Original) ||
      !Fields.map("replacement", M.Replacement) ||
      !Fields.map("function", M.Function) || !Fields.map("layout", M.Layout) ||
      (!Inplace && (!Fields.map("sectionLayout", M.SectionLayout) ||
                    !Fields.map("section", M.Section))))
    return createFileError(Path, Root.getError());
  const auto ValidName = [](StringRef S) {
    return !S.empty() && !S.contains('\0');
  };
  if (!ValidName(M.Function) || !ValidName(M.Layout) ||
      !ValidName(M.Replacement) || !ValidName(M.Original) ||
      !llvm::all_of(M.Objects, ValidName) ||
      (!Inplace && (!ValidName(M.SectionLayout) || !ValidName(M.Section))))
    return createFileError(
        Path, createStringError("empty name or embedded NUL in manifest"));
  if (llvm::count(M.Objects, M.Original) != 1)
    return createFileError(
        Path,
        createStringError("original must name exactly one entry in objects"));
  return M;
}

Expected<std::unique_ptr<ObjectFile>> mipsObject(MemoryBufferRef Bytes,
                                                 uint16_t Type) {
  auto Obj = ObjectFile::createObjectFile(Bytes);
  if (!Obj)
    return Obj.takeError();
  const auto *ELFObj = dyn_cast<ELFObjectFileBase>(Obj->get());
  if (!ELFObj || ELFObj->getEMachine() != ELF::EM_MIPS ||
      ELFObj->getEType() != Type)
    return createStringError(Type == ELF::ET_REL
                                 ? "expected a MIPS ELF relocatable object"
                                 : "expected a static MIPS ELF executable");
  return Obj;
}

struct FunctionRange {
  std::string Section;
  uint64_t Address, Offset, Size;
  uint8_t Other;
};

Expected<FunctionRange> findFunction(const ELFObjectFileBase &Obj,
                                     StringRef Name) {
  std::optional<FunctionRange> Result;
  for (const ELFSymbolRef &Sym : Obj.symbols()) {
    if (Sym.getBinding() != ELF::STB_GLOBAL ||
        Sym.getELFType() != ELF::STT_FUNC)
      continue;
    auto SymName = Sym.getName();
    if (!SymName)
      return SymName.takeError();
    if (*SymName != Name)
      continue;
    auto Sec = Sym.getSection();
    if (!Sec)
      return Sec.takeError();
    if (*Sec == Obj.section_end())
      continue;
    ELFSectionRef S(**Sec);
    auto SecName = S.getName();
    if (!SecName)
      return SecName.takeError();
    auto Addr = Sym.getAddress();
    if (!Addr)
      return Addr.takeError();
    const unsigned Flags = ELF::SHF_EXECINSTR | ELF::SHF_ALLOC;
    if (Result || !Sym.getSize() || S.getType() != ELF::SHT_PROGBITS ||
        (S.getFlags() & Flags) != Flags || *Addr < S.getAddress() ||
        !rangeInBounds(*Addr - S.getAddress(), Sym.getSize(), S.getSize()) ||
        !rangeInBounds(S.getOffset(), S.getSize(), Obj.getData().size()))
      return createStringError("ambiguous or invalid executable range for " +
                               Name);
    Result = FunctionRange{SecName->str(), *Addr,
                           S.getOffset() + (*Addr - S.getAddress()),
                           Sym.getSize(), Sym.getOther()};
  }
  if (!Result)
    return createStringError("missing defined global function " + Name);
  return std::move(*Result);
}

struct LoadSegment {
  uint64_t Offset, Address, Physical, FileSize, MemorySize, Alignment, Flags;
  bool operator==(const LoadSegment &) const = default;

  bool containsExecutableBytes(uint64_t VA, uint64_t Size) const {
    return (Flags & ELF::PF_X) && VA >= Address &&
           rangeInBounds(VA - Address, Size, FileSize);
  }
};

template <typename ELFT>
Expected<std::vector<LoadSegment>>
loadSegments(const ELFObjectFile<ELFT> &Obj) {
  auto Headers = Obj.getELFFile().program_headers();
  if (!Headers)
    return Headers.takeError();
  std::vector<LoadSegment> Segments;
  for (const auto &P : *Headers) {
    if (P.p_type == ELF::PT_INTERP || P.p_type == ELF::PT_DYNAMIC)
      return createStringError("relink requires a static ELF");
    if (P.p_type != ELF::PT_LOAD)
      continue;
    if (P.p_filesz > P.p_memsz ||
        !rangeInBounds(P.p_offset, P.p_filesz, Obj.getData().size()))
      return createStringError("invalid ELF load segment");
    Segments.push_back({P.p_offset, P.p_vaddr, P.p_paddr, P.p_filesz, P.p_memsz,
                        P.p_align, P.p_flags});
  }
  return Segments;
}

Expected<std::vector<LoadSegment>> loadSegments(const ELFObjectFileBase &Obj) {
  if (const auto *O = dyn_cast<ELF32LEObjectFile>(&Obj))
    return loadSegments(*O);
  if (const auto *O = dyn_cast<ELF32BEObjectFile>(&Obj))
    return loadSegments(*O);
  if (const auto *O = dyn_cast<ELF64LEObjectFile>(&Obj))
    return loadSegments(*O);
  return loadSegments(cast<ELF64BEObjectFile>(Obj));
}

struct LinkedImage {
  FunctionRange Function;
  std::vector<LoadSegment> Loads;
  std::optional<StringRef> ABIFlags;
  uint64_t Entry;
  SmallVector<StringRef> EntrySymbols;
  uint32_t Flags;
  unsigned AddressBytes, OSABI, ABIVersion;
  bool LittleEndian;
};

Expected<LinkedImage> inspect(MemoryBufferRef Buffer, StringRef Function) {
  auto Obj = mipsObject(Buffer, ELF::ET_EXEC);
  if (!Obj)
    return Obj.takeError();
  const auto &O = cast<ELFObjectFileBase>(**Obj);
  auto F = findFunction(O, Function);
  if (!F)
    return F.takeError();
  auto Loads = loadSegments(O);
  if (!Loads)
    return Loads.takeError();
  if (llvm::none_of(*Loads, [&](const LoadSegment &Seg) {
        return Seg.containsExecutableBytes(F->Address, F->Size) &&
               F->Address - Seg.Address + Seg.Offset == F->Offset;
      }))
    return createStringError("function is outside executable load segments");
  std::optional<StringRef> ABIFlags;
  for (const ELFSectionRef S : O.sections()) {
    if (S.getType() != ELF::SHT_MIPS_ABIFLAGS)
      continue;
    auto Contents = S.getContents();
    if (!Contents)
      return Contents.takeError();
    ABIFlags = *Contents;
  }
  auto Entry = O.getStartAddress();
  if (!Entry)
    return Entry.takeError();
  // Bit zero selects the MIPS instruction mode and is not part of the VA.
  const uint64_t EntryAddress = *Entry & ~uint64_t{1};
  if (llvm::none_of(*Loads, [&](const LoadSegment &Seg) {
        return Seg.containsExecutableBytes(EntryAddress, 1);
      }))
    return createStringError(
        "program entry is outside file-backed executable load segments");
  SmallVector<StringRef> EntrySymbols;
  for (const ELFSymbolRef &Sym : O.symbols()) {
    if (Sym.getBinding() != ELF::STB_GLOBAL ||
        (Sym.getELFType() != ELF::STT_FUNC &&
         Sym.getELFType() != ELF::STT_NOTYPE))
      continue;
    auto Address = Sym.getAddress();
    if (!Address)
      return Address.takeError();
    if (*Address != *Entry && *Address != EntryAddress)
      continue;
    auto Sec = Sym.getSection();
    if (!Sec)
      return Sec.takeError();
    if (*Sec == O.section_end() ||
        !(ELFSectionRef(**Sec).getFlags() & ELF::SHF_EXECINSTR))
      continue;
    auto Name = Sym.getName();
    if (!Name)
      return Name.takeError();
    if (!Name->empty())
      EntrySymbols.push_back(*Name);
  }
  return LinkedImage{std::move(*F),
                     std::move(*Loads),
                     ABIFlags,
                     *Entry,
                     std::move(EntrySymbols),
                     O.getPlatformFlags(),
                     O.getBytesInAddress(),
                     O.getEIdentOSABI(),
                     O.getEIdentABIVersion(),
                     O.isLittleEndian()};
}

Error verifyProduct(const LinkedImage &Old, const LinkedImage &New,
                    StringRef Input, StringRef Product, const LinkManifest &M,
                    bool Inplace) {
  if (Old.LittleEndian != New.LittleEndian ||
      Old.AddressBytes != New.AddressBytes || Old.Flags != New.Flags ||
      Old.ABIFlags != New.ABIFlags || Old.OSABI != New.OSABI ||
      Old.ABIVersion != New.ABIVersion ||
      Old.Function.Other != New.Function.Other)
    return createStringError("relink changed the MIPS target or function ABI");
  const auto &A = Old.Function;
  const auto &B = New.Function;
  if (!Inplace) {
    // LLD may move startup code too. Match a named entry by symbol and ISA
    // mode. An anonymous entry can only be verified at its original address.
    const bool SameEntry =
        Old.EntrySymbols.empty()
            ? Old.Entry == New.Entry
            : llvm::any_of(Old.EntrySymbols, [&](StringRef Name) {
                return llvm::is_contained(New.EntrySymbols, Name);
              });
    if (!SameEntry || ((Old.Entry ^ New.Entry) & 1))
      return createStringError("section relink changed the program entry");
    const bool Overlap = A.Address <= B.Address
                             ? B.Address - A.Address < A.Size
                             : A.Address - B.Address < B.Size;
    if (B.Section != M.Section || B.Section == A.Section || Overlap)
      return createStringError(
          "function did not move into the requested separate section");
    return Error::success();
  }
  if (B.Section != A.Section || B.Address != A.Address ||
      B.Offset != A.Offset || B.Size > A.Size)
    return createStringError(
        "inplace function does not fit its original range");
  if (Old.Entry != New.Entry || Old.Loads != New.Loads)
    return createStringError("inplace relink changed the entry or load layout");
  for (const auto &Seg : Old.Loads) {
    // Compare both sides of the original function extent in every mapping.
    const uint64_t End = Seg.Offset + Seg.FileSize;
    const uint64_t Before = std::clamp(A.Offset, Seg.Offset, End);
    const uint64_t After = std::clamp(A.Offset + A.Size, Seg.Offset, End);
    if (Input.slice(Seg.Offset, Before) != Product.slice(Seg.Offset, Before) ||
        Input.slice(After, End) != Product.slice(After, End))
      return createStringError("inplace relink changed other loaded bytes");
  }
  return Error::success();
}

Error copyInput(const fs::path &Source, const fs::path &Dest) {
  if (auto EC = sys::fs::copy_file(pathToUTF8(Source), pathToUTF8(Dest)))
    return createFileError(pathToUTF8(Source), EC);
  return Error::success();
}

Error checkObject(StringRef Path, StringRef Function) {
  auto Buffer = readFile(Path);
  if (!Buffer)
    return Buffer.takeError();
  auto Obj = mipsObject(**Buffer, ELF::ET_REL);
  if (!Obj)
    return createFileError(Path, Obj.takeError());
  if (!Function.empty()) {
    auto F = findFunction(cast<ELFObjectFileBase>(**Obj), Function);
    if (!F)
      return createFileError(Path, F.takeError());
  }
  return Error::success();
}

Error linkImage(StringRef Driver, StringRef Layout, StringRef Output,
                const std::vector<std::string> &Objects) {
  SmallVector<StringRef> Args{Driver, "-static", "-T", Layout, "-o", Output};
  llvm::append_range(Args, Objects);
  std::string Detail;
  int Status =
      sys::ExecuteAndWait(Driver, Args, std::nullopt, {}, 0, 0, &Detail);
  if (Status)
    return createStringError("ELF linker exited with status %d: %s", Status,
                             Detail.c_str());
  return Error::success();
}

Error publish(StringRef Bytes, StringRef Output, fs::perms Permissions) {
  using namespace neverd::support::atomic_output;
  auto Temp = sys::fs::TempFile::create(Output + ".tmp-%%%%%%");
  if (!Temp)
    return Temp.takeError();
  raw_fd_ostream Stream(Temp->FD, false);
  Stream << Bytes;
  Stream.flush();
  if (Stream.has_error()) {
    auto EC = Stream.error();
    Stream.clear_error();
    return joinErrors(errorCodeToError(EC), discardTemporaryOutput(*Temp));
  }
  return closeAndCommitTemporaryOutput(
      *Temp, Output, OutputMode::ReplaceExisting, Permissions);
}

Error relink(StringRef Output) {
  const bool Inplace = PatchStrat == InplaceMode;
  auto M = readManifest(PatchRelink, Inplace);
  if (!M)
    return M.takeError();
  if (PatchLinkerPath.empty())
    return createStringError("--linker-path requires an ELF linker executable");
  auto Driver = sys::findProgramByName(PatchLinkerPath);
  if (!Driver)
    return createFileError(PatchLinkerPath, Driver.getError());
  auto Input = readFile(InputFile);
  if (!Input)
    return Input.takeError();
  auto Old = inspect(**Input, M->Function);
  if (!Old)
    return createFileError(InputFile, Old.takeError());

  SmallString<128> Temp;
  if (auto EC = sys::fs::createUniqueDirectory("neverd-relink", Temp))
    return errorCodeToError(EC);
  scope_exit Cleanup([&] { sys::fs::remove_directories(Temp); });
  const fs::path Work = fs::u8path(Temp.str().str());
  const fs::path Base = fs::u8path(PatchRelink.getValue()).parent_path();
  const auto Source = [&](StringRef P) { return Base / fs::u8path(P.str()); };
  const std::string Layout = pathToUTF8(Work / "layout.ld");
  const std::string Candidate = pathToUTF8(Work / "image.elf");
  if (Error E = copyInput(Source(M->Layout), Work / "layout.ld"))
    return E;
  std::vector<std::string> Objects;
  fs::path Selected;
  for (const std::string &Name : M->Objects) {
    // Preserve input basenames when a layout uses file-specific selectors.
    const fs::path Dir = Work / std::to_string(Objects.size());
    if (auto EC = sys::fs::create_directory(pathToUTF8(Dir)))
      return errorCodeToError(EC);
    const fs::path Staged = Dir / Source(Name).filename();
    if (Error E = copyInput(Source(Name), Staged))
      return E;
    Objects.push_back(pathToUTF8(Staged));
    const bool IsSelected = Name == M->Original;
    if (Error E = checkObject(Objects.back(), IsSelected ? M->Function : ""))
      return E;
    if (IsSelected)
      Selected = Staged;
  }
  if (Error E = linkImage(*Driver, Layout, Candidate, Objects))
    return E;
  auto Baseline = readFile(Candidate);
  if (!Baseline)
    return Baseline.takeError();
  if ((*Baseline)->getBuffer() != (*Input)->getBuffer())
    return createStringError(
        "original objects and layout do not reproduce the input ELF");
  Baseline->reset();

  if (Error E = copyInput(Source(M->Replacement), Selected))
    return E;
  if (Error E = checkObject(pathToUTF8(Selected), M->Function))
    return E;
  if (!Inplace)
    if (Error E = copyInput(Source(M->SectionLayout), Work / "layout.ld"))
      return E;
  if (Error E = linkImage(*Driver, Layout, Candidate, Objects))
    return E;
  auto Product = readFile(Candidate);
  if (!Product)
    return Product.takeError();
  auto New = inspect(**Product, M->Function);
  if (!New)
    return New.takeError();
  if (Error E = verifyProduct(*Old, *New, (*Input)->getBuffer(),
                              (*Product)->getBuffer(), *M, Inplace))
    return E;
  std::error_code EC;
  const auto Perms =
      fs::status(fs::u8path(InputFile.getValue()), EC).permissions();
  if (EC)
    return createFileError(InputFile, EC);
  return publish((*Product)->getBuffer(), Output, Perms);
}

} // namespace

int runRelinkPatch(StringRef Output) {
  for (const auto &[Name, Opt] : cl::getRegisteredOptions(PatchCmd)) {
    if (Opt->getNumOccurrences() && Opt != &InputFile && Opt != &OutputFile &&
        Opt != &PatchRelink && Opt != &PatchLinkerPath && Opt != &PatchStrat &&
        Opt != &Verbose) {
      WithColor::error() << "--relink cannot be combined with --" << Name
                         << "\n";
      return 1;
    }
  }
  if (Error E = relink(Output)) {
    WithColor::error() << toString(std::move(E)) << "\n";
    return 1;
  }
  outs() << "Relinked MIPS ELF: " << Output << "\n";
  return 0;
}

} // namespace neverd::cli
