//===- SourcePatchDependencies.cpp - Source patch support ===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Plan and verify explicit native dependencies.
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/SourcePatchDependencies.h"

#include "ELF/ELFSourceDependencies.h"
#include "MachO/MachOSourceDependencies.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/codegen/COFF/COFFLayoutGrowth.h"
#include "neverd/loader/Loader.h"
#include "neverd/object/PELayout.h"

#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/MathExtras.h"

#include <map>
#include <set>

namespace neverd {
namespace {
#define SOURCE_DEPENDENCY_TEXT(Name, Text) constexpr char Name[] = Text;
#include "SourcePatchDependencies.def"
llvm::Error fail(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::errc::not_supported,
                                 "source dependency: " + Message);
}

llvm::Expected<std::vector<uint8_t>>
appendCOFFImports(const BinaryImage &Original,
                  std::vector<SourcePatchImport> &Imports) {
  using namespace llvm::COFF;
  auto Bytes = Original.Raw;
  auto Growth = ensureCOFFSectionHeaderCapacity(Bytes, 1);
  if (!Growth)
    return Growth.takeError();
  auto PE = locatePEHeaders(Bytes.data(), Bytes.size());
  const uint64_t SA = getPESectionAlignment(PE), FA = getPEFileAlignment(PE);
  if (!PE.valid() || !PE.SectionTable || PE.NumSections >= 96 ||
      !llvm::isPowerOf2_64(SA) || SA < 4096 || SA > 65536 ||
      !llvm::isPowerOf2_64(FA) || FA < 512 || FA > SA ||
      getPEImageBase(PE) != Original.Base)
    return fail("invalid PE import section layout");
  auto *ImportDirectory = getPEDataDirectory(PE, IMPORT_TABLE);
  if (!ImportDirectory)
    return fail("PE optional header has no import directory field");
  const uint64_t RVA = llvm::alignTo(getPESizeOfImage(PE), SA);
  const unsigned Width = PE.Is64 ? 8 : 4;
  std::vector<uint8_t> Storage;
  // The old descriptors continue to point to their original INT/IAT slots.
  // Bound imports cannot be silently converted to a different binding policy.
  if (auto *Bound = getPEDataDirectory(PE, BOUND_IMPORT))
    if (Bound->Size || Bound->RelativeVirtualAddress)
      return fail("bound PE imports require explicit rebinding support");
  if (ImportDirectory->Size || ImportDirectory->RelativeVirtualAddress) {
    const uint64_t Size = ImportDirectory->Size;
    const auto *Old = Original.readVA(
        Original.Base + ImportDirectory->RelativeVirtualAddress, Size);
    if (!Old || Size < 20 ||
        Size > ::neverd::limits::backend::
                   kSourcePatchDependenciesAppendCOFFImportsSizeLimit)
      return fail("original import descriptors are not bounded");
    bool Terminated = false;
    for (uint64_t I = 0; rangeInBounds(I, 20, Size); I += 20) {
      auto Row = llvm::ArrayRef<uint8_t>(Old + I, 20);
      if (llvm::all_of(Row, [](uint8_t B) { return B == 0; })) {
        Terminated = true;
        break;
      }
      if (readLE<uint32_t>(Old + I + 4) != 0 ||
          readLE<uint32_t>(Old + I + 8) != 0 ||
          !readLE<uint32_t>(Old + I + 12) || !readLE<uint32_t>(Old + I + 16))
        return fail("unsupported bound or malformed PE import descriptor");
      Storage.insert(Storage.end(), Row.begin(), Row.end());
    }
    if (!Terminated)
      return fail("original import descriptors lack a terminator");
  }
  std::map<std::string, std::vector<size_t>> Libraries;
  for (size_t I = 0; I < Imports.size(); ++I)
    Libraries[Imports[I].Request.Library].push_back(I);
  const uint64_t FirstNew = Storage.size();
  Storage.resize(Storage.size() + 20 * (Libraries.size() + 1), 0);
  const uint64_t DirectorySize = Storage.size();
  auto Align = [&](unsigned N) {
    Storage.resize(llvm::alignTo(Storage.size(), N), 0);
  };
  auto String = [&](llvm::StringRef S) {
    const uint64_t Offset = Storage.size();
    Storage.insert(Storage.end(), S.begin(), S.end());
    Storage.push_back(0);
    return Offset;
  };
  uint64_t Row = FirstNew;
  for (const auto &[Library, Indices] : Libraries) {
    const uint64_t Name = String(Library);
    Align(Width);
    const uint64_t INT = Storage.size();
    Storage.resize(Storage.size() + Width * (Indices.size() + 1), 0);
    const uint64_t IAT = Storage.size();
    Storage.resize(Storage.size() + Width * (Indices.size() + 1), 0);
    for (size_t N = 0; N < Indices.size(); ++N) {
      auto &Import = Imports[Indices[N]];
      Align(2);
      const uint64_t HintName = Storage.size();
      Storage.resize(Storage.size() + 2, 0);
      String(Import.Request.Symbol);
      if (RVA + HintName > UINT32_MAX)
        return fail("PE import names exceed RVA limits");
      if (Width == 8) {
        writeLE<uint64_t>(Storage.data() + INT + N * Width, RVA + HintName);
        writeLE<uint64_t>(Storage.data() + IAT + N * Width, RVA + HintName);
      } else {
        writeLE<uint32_t>(Storage.data() + INT + N * Width, RVA + HintName);
        writeLE<uint32_t>(Storage.data() + IAT + N * Width, RVA + HintName);
      }
      Import.SlotVA = Original.Base + RVA + IAT + N * Width;
    }
    writeLE<uint32_t>(Storage.data() + Row, RVA + INT);
    writeLE<uint32_t>(Storage.data() + Row + 12, RVA + Name);
    writeLE<uint32_t>(Storage.data() + Row + 16, RVA + IAT);
    Row += 20;
  }
  const uint64_t Offset = llvm::alignTo(Bytes.size(), FA);
  const uint64_t RawSize = llvm::alignTo(Storage.size(), FA);
  const uint64_t ImageEnd = llvm::alignTo(RVA + Storage.size(), SA);
  if (Offset + RawSize > UINT32_MAX || ImageEnd > UINT32_MAX ||
      Original.Base > UINT64_MAX - ImageEnd)
    return fail("PE import section exceeds address limits");
  Bytes.resize(Offset + RawSize, 0);
  std::copy(Storage.begin(), Storage.end(), Bytes.begin() + Offset);
  PE = locatePEHeaders(Bytes.data(), Bytes.size());
  auto *Section =
      reinterpret_cast<llvm::object::coff_section *>(PE.SectionTable) +
      PE.NumSections;
  std::memset(Section, 0, sizeof(*Section));
  std::memcpy(Section->Name, ".ndimp", 6);
  Section->VirtualAddress = RVA;
  Section->VirtualSize = Storage.size();
  Section->PointerToRawData = Offset;
  Section->SizeOfRawData = RawSize;
  Section->Characteristics =
      IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_WRITE;
  PE.FileHeader->NumberOfSections = PE.NumSections + 1;
  setPESizeOfImage(PE, ImageEnd);
  if (PE.Is64) {
    auto *Optional =
        reinterpret_cast<llvm::object::pe32plus_header *>(PE.OptionalHeader);
    if (RawSize > UINT32_MAX - Optional->SizeOfInitializedData)
      return fail("PE initialized-data size overflows");
    Optional->SizeOfInitializedData += RawSize;
    Optional->CheckSum = 0;
  } else {
    auto *Optional =
        reinterpret_cast<llvm::object::pe32_header *>(PE.OptionalHeader);
    if (RawSize > UINT32_MAX - Optional->SizeOfInitializedData)
      return fail("PE initialized-data size overflows");
    Optional->SizeOfInitializedData += RawSize;
    Optional->CheckSum = 0;
  }
  auto *NewImportDirectory = const_cast<llvm::object::data_directory *>(
      getPEDataDirectory(PE, IMPORT_TABLE));
  NewImportDirectory->RelativeVirtualAddress = RVA;
  NewImportDirectory->Size = DirectorySize;
  // Preserve the original IAT directory, including its protection contract.
  // The new FirstThunk arrays live in their own writable section and do not
  // require widening that directory across unrelated executable pages.
  return Bytes;
}
} // namespace

llvm::Error validateSourcePatchDependencyRequests(
    llvm::ArrayRef<SourcePatchDependency> Requests) {
  if (Requests.empty() ||
      Requests.size() >
          ::neverd::limits::backend::
              kSourcePatchDependenciesPrepareSourcePatchDependenciesRequestsSizeLimit)
    return fail(kRequestCount);
  std::set<std::string> Names;
  for (const auto &Request : Requests) {
    if (Request.Library.empty() ||
        Request.Library.size() >
            ::neverd::limits::backend::
                kSourcePatchDependenciesPrepareSourcePatchDependenciesRequestLibrarySizeLimit ||
        Request.Symbol.empty() ||
        Request.Symbol.size() >
            ::neverd::limits::backend::
                kSourcePatchDependenciesPrepareSourcePatchDependenciesRequestSymbolSizeLimit ||
        llvm::StringRef(Request.Library).contains('\0') ||
        llvm::StringRef(Request.Symbol).contains('\0') ||
        (Request.Kind != SourcePatchDependencyKind::Function &&
         Request.Kind != SourcePatchDependencyKind::Data) ||
        !Names.insert(Request.Symbol).second)
      return fail(kRequestShape);
  }
  return llvm::Error::success();
}

llvm::Expected<SourcePatchDependencies>
prepareSourcePatchDependencies(const BinaryImage &Original,
                               llvm::ArrayRef<SourcePatchDependency> Requests) {
  if (auto Error = validateSourcePatchDependencyRequests(Requests))
    return Error;
  SourcePatchDependencies Result;
  for (const auto &Request : Requests) {
    for (const auto &Import : Original.Imports)
      if (Import.Name == Request.Symbol ||
          (Original.Format == BinaryFormat::MachO &&
           Import.Name == "_" + Request.Symbol))
        return fail("requested symbol already has an import identity");
    for (const auto &Symbol : Original.Symbols)
      if (Symbol.Name == Request.Symbol ||
          (Original.Format == BinaryFormat::MachO &&
           Symbol.Name == "_" + Request.Symbol))
        return fail("requested symbol already exists in the image");
    Result.Imports.push_back({Request, 0});
  }
  if (Original.Format != BinaryFormat::COFF &&
      Original.Format != BinaryFormat::ELF &&
      Original.Format != BinaryFormat::MachO)
    return fail("this native format has no dependency installer");
  auto Bytes = Original.isELF()
                   ? appendELFSourceDependencies(Original, Result.Imports)
               : Original.Format == BinaryFormat::MachO
                   ? appendMachOSourceDependencies(Original, Result.Imports)
                   : appendCOFFImports(Original, Result.Imports);
  if (!Bytes)
    return Bytes.takeError();
  auto Reader = Loader::create(Original.Format);
  auto Reloaded = Reader->loadBuffer(llvm::MemoryBufferRef(
      llvm::StringRef(reinterpret_cast<const char *>(Bytes->data()),
                      Bytes->size()),
      "prepared-source-dependencies"));
  if (!Reloaded)
    return Reloaded.takeError();
  Result.Image = std::move(*Reloaded);
  if (auto E = verifySourcePatchDependencies(Result.Image, Result.Imports))
    return E;
  for (const auto &Old : Original.Imports) {
    if (Original.isELF() && !Old.IATAddr) {
      if (!llvm::is_contained(Result.Image.DynInfo.NeededLibs, Old.Module))
        return fail("an original dependency library disappeared");
      continue;
    }
    const auto *Now = Result.Image.findImportAt(Old.IATAddr);
    if (!Now || Now->Name != Old.Name || Now->Module != Old.Module ||
        Now->Ordinal != Old.Ordinal || Now->IsDelayImport != Old.IsDelayImport)
      return fail("an original import changed its identity or address");
  }
  return Result;
}

// These are linker-owned tail thunks, not fabricated object/MC functions.
// Each instruction sequence has an explicit ABI and its own unwind record.
llvm::Expected<std::vector<uint8_t>>
buildSourceImportThunk(Arch A, InstructionMode Mode, uint64_t VA,
                       uint64_t Slot) {
  std::vector<uint8_t> B;
  auto Word = [&](uint32_t V) {
    size_t O = B.size();
    B.resize(O + 4);
    writeLE<uint32_t>(B.data() + O, V);
  };
  if (A == Arch::X64) {
    const int64_t D = int64_t(Slot) - int64_t(VA + 6);
    if (!llvm::isInt<32>(D))
      return fail("import thunk exceeds RIP-relative reach");
    B = {0xff, 0x25};
    Word(uint32_t(D));
  } else if (A == Arch::X86) {
    // Preserve eax, every argument register and flags. The temporary return
    // address supplies a PIC base; xchg/ret restores the complete entry state.
    B = {0x50, 0xe8, 0, 0, 0, 0, 0x58, 0x8b, 0x80};
    Word(uint32_t(Slot) - uint32_t(VA + 6));
    B.insert(B.end(), {0x87, 0x04, 0x24, 0xc3});
  } else if (A == Arch::AArch64) {
    const int64_t Pages =
        (int64_t(Slot & ~uint64_t(4095)) - int64_t(VA & ~uint64_t(4095))) /
        4096;
    if (!llvm::isInt<21>(Pages) || Slot % 8)
      return fail("import thunk exceeds AArch64 GOT reach");
    Word(0x90000010 | ((uint32_t(Pages) & 3) << 29) |
         (((uint32_t(Pages) >> 2) & 0x7ffff) << 5));
    Word(0xf9400210 | (uint32_t((Slot & 4095) / 8) << 10));
    Word(0xd61f0200); // br x16: the ABI's inter-procedure scratch register.
  } else if (A == Arch::ARM && Mode == InstructionMode::ARM) {
    Word(0xe59fc004);
    Word(0xe08fc00c);
    Word(0xe59cf000);
    Word(uint32_t(Slot) - uint32_t(VA + 12));
  } else if (A == Arch::ARM && Mode == InstructionMode::Thumb) {
    B = {0xdf, 0xf8, 0x08, 0xc0, 0xfc, 0x44,
         0xdc, 0xf8, 0x00, 0xf0, 0x00, 0xbf};
    Word(uint32_t(Slot) - uint32_t(VA + 8));
  } else
    return fail("unsupported import thunk architecture");
  return B;
}

llvm::Expected<std::vector<uint8_t>>
buildSourceImportThunkFrame(Arch A, uint64_t VA, uint64_t Size,
                            uint64_t FrameVA) {
  std::vector<uint8_t> B;
  auto Word = [&](uint32_t V) {
    size_t O = B.size();
    B.resize(O + 4);
    writeLE<uint32_t>(B.data() + O, V);
  };
  Word(0);
  Word(0);
  const uint8_t Width = (A == Arch::X86 || A == Arch::ARM) ? 4 : 8;
  const uint8_t SP = A == Arch::X86   ? 4
                     : A == Arch::X64 ? 7
                     : A == Arch::ARM ? 13
                                      : 31;
  const uint8_t RA = A == Arch::X86   ? 8
                     : A == Arch::X64 ? 16
                     : A == Arch::ARM ? 14
                                      : 30;
  B.insert(B.end(),
           {1, 'z', 'R', 0, 1, uint8_t(0x80 - Width), RA, 1, 0x1b, 0x0c, SP,
            uint8_t((A == Arch::AArch64 || A == Arch::ARM) ? 0 : Width)});
  if (A == Arch::X86 || A == Arch::X64)
    B.insert(B.end(), {uint8_t(0x80 | RA), 1});
  B.resize(llvm::alignTo(B.size(), 8), 0);
  writeLE<uint32_t>(B.data(), B.size() - 4);
  const size_t F = B.size();
  Word(0);
  Word(F + 4);
  const int64_t D = int64_t(VA) - int64_t(FrameVA + B.size());
  if (!llvm::isInt<32>(D))
    return fail("import thunk frame exceeds DWARF reach");
  Word(uint32_t(D));
  Word(Size);
  B.push_back(0);
  if (A == Arch::X86)
    B.insert(B.end(), {0x41, 0x0e, 8, 0x80, 2, // push eax
                       0x45, 0x0e, 12,         // call next
                       0x41, 0x0e, 8,          // pop eax
                       0x49, 0xc0});           // xchg restores eax
  B.resize(llvm::alignTo(B.size(), 8), 0);
  writeLE<uint32_t>(B.data() + F, B.size() - F - 4);
  return B;
}

llvm::Error
verifySourcePatchDependencies(const BinaryImage &Image,
                              llvm::ArrayRef<SourcePatchImport> Imports) {
  if (Imports.empty())
    return llvm::Error::success();
  if (Image.isELF())
    return verifyELFSourceDependencies(Image, Imports);
  if (Image.Format == BinaryFormat::MachO)
    return verifyMachOSourceDependencies(Image, Imports);
  for (const auto &Import : Imports) {
    const auto *Found = Image.findImportAt(Import.SlotVA);
    const auto *Segment = Image.getSegmentFor(Import.SlotVA);
    if (!Found || Found->Name != Import.Request.Symbol ||
        !llvm::StringRef(Found->Module)
             .equals_insensitive(Import.Request.Library) ||
        Found->IsDelayImport || !Segment || !Segment->isReadable() ||
        Segment->isExecutable() ||
        !Image.readVA(Import.SlotVA, Image.getPointerSize()))
      return fail("added import failed native loader replay");
  }
  return llvm::Error::success();
}

llvm::Error
bindSourcePatchDependencies(llvm::Module &Module,
                            llvm::ArrayRef<SourcePatchImport> Imports,
                            bool RewriteDeclarations) {
  for (const auto &Import : Imports) {
    auto *Value = Module.getNamedValue(Import.Request.Symbol);
    if (!Value || !Value->isDeclaration() || Value->getAddressSpace() ||
        (llvm::isa<llvm::Function>(Value) !=
         (Import.Request.Kind == SourcePatchDependencyKind::Function)))
      return fail("request does not match an external declaration: " +
                  Import.Request.Symbol);
    if (const auto *Global = llvm::dyn_cast<llvm::GlobalVariable>(Value))
      if (Global->isThreadLocal())
        return fail("new TLS imports require a native TLS allocation contract");
    for (const auto &Use : Value->uses())
      if (!llvm::isa<llvm::Instruction>(Use.getUser()))
        return fail("new imports in constant initializers are not modeled");
    if (Module.getNamedValue(makeNdDataSymbol(Import.SlotVA)))
      return fail("import slot declaration collides with an existing symbol");
  }
  if (!RewriteDeclarations)
    return llvm::Error::success();
  for (const auto &Import : Imports) {
    auto *Value = Module.getNamedValue(Import.Request.Symbol);
    auto *Slot = new llvm::GlobalVariable(
        Module, Value->getType(), false, llvm::GlobalValue::ExternalLinkage,
        nullptr, makeNdDataSymbol(Import.SlotVA));
    Slot->setDSOLocal(true);
    Slot->setVisibility(llvm::GlobalValue::HiddenVisibility);
    Slot->setAlignment(llvm::Align(Module.getDataLayout().getPointerSize()));
    while (!Value->use_empty()) {
      auto &Use = *Value->use_begin();
      auto *User = llvm::cast<llvm::Instruction>(Use.getUser());
      llvm::Instruction *Before = User;
      if (auto *Phi = llvm::dyn_cast<llvm::PHINode>(User))
        Before = Phi->getIncomingBlock(Use.getOperandNo())->getTerminator();
      llvm::IRBuilder<> Builder(Before);
      auto *Pointer =
          Builder.CreateLoad(Value->getType(), Slot, "dependency.address");
      // Loader-owned, eagerly populated slot. Keep it observable as memory;
      // never treat its on-disk placeholder as the eventual pointer value.
      Pointer->setVolatile(true);
      Use.set(Pointer);
    }
  }
  return llvm::Error::success();
}
} // namespace neverd
