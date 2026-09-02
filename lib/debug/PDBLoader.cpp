//===- PDBLoader.cpp - PDB debug info loader -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// PDB debug information loading implementation.
///
//===----------------------------------------------------------------------===//

#include "neverd/debug/PDBLoader.h"

#include "neverd/loader/BinaryImage.h"

#define DEBUG_TYPE "neverd-pdb-loader"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/CodeView/GUID.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/ModuleDebugStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/PublicsStream.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/Native/SymbolCache.h"
#include "llvm/DebugInfo/PDB/Native/SymbolStream.h"
#include "llvm/DebugInfo/PDB/PDB.h"
#include "llvm/DebugInfo/PDB/PDBSymbolTypeBuiltin.h"
#include "llvm/DebugInfo/PDB/PDBSymbolTypeFunctionSig.h"
#include "llvm/DebugInfo/PDB/PDBSymbolTypePointer.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <set>

namespace neverd {

llvm::Expected<std::vector<pdb_loader_detail::IndexedSymbolRecord>>
pdb_loader_detail::indexSymbolRecords(
    const llvm::codeview::CVSymbolArray &Records) {
  std::vector<IndexedSymbolRecord> Indexed;
  bool HadError = false;

  // VarStreamArray::at() is undefined for a non-record boundary.  Also, this
  // vendored LLVM revision's begin(HadError) does not propagate HadError into
  // its iterator, so construct the public iterator explicitly with the error
  // sink instead of relying on either convenience API.
  auto It = llvm::codeview::CVSymbolArray::Iterator(
      Records, Records.getExtractor(), Records.skew(), &HadError);
  const auto End = Records.end();
  for (; It != End; ++It)
    Indexed.push_back({It.offset(), *It});

  if (HadError)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "malformed CodeView symbol record stream");
  return Indexed;
}

const llvm::codeview::CVSymbol *pdb_loader_detail::findSymbolAtExactOffset(
    llvm::ArrayRef<IndexedSymbolRecord> Records, uint32_t Offset) {
  const auto It = std::lower_bound(
      Records.begin(), Records.end(), Offset,
      [](const IndexedSymbolRecord &Record, uint32_t WantedOffset) {
        return Record.Offset < WantedOffset;
      });
  return It != Records.end() && It->Offset == Offset ? &It->Symbol : nullptr;
}

void pdb_loader_detail::FunctionNameRegistry::observe(va_t Address,
                                                      llvm::StringRef Name) {
  Record &Entry = Records[Address];
  if (Entry.State == FunctionNameState::Ambiguous)
    return;
  if (Name.empty()) {
    Entry.State = FunctionNameState::Ambiguous;
    Entry.Name.clear();
    return;
  }
  if (Entry.State == FunctionNameState::Absent) {
    Entry.State = FunctionNameState::Unique;
    Entry.Name = Name.str();
    return;
  }
  if (Entry.Name != Name) {
    Entry.State = FunctionNameState::Ambiguous;
    Entry.Name.clear();
  }
}

pdb_loader_detail::FunctionNameState
pdb_loader_detail::FunctionNameRegistry::state(va_t Address) const {
  const auto It = Records.find(Address);
  return It == Records.end() ? FunctionNameState::Absent : It->second.State;
}

std::optional<std::string>
pdb_loader_detail::FunctionNameRegistry::name(va_t Address) const {
  const auto It = Records.find(Address);
  if (It == Records.end() || It->second.State != FunctionNameState::Unique)
    return std::nullopt;
  return It->second.Name;
}

namespace {

llvm::Error pdbLoadError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "pdb: " + Message);
}

PDBBuildIdentity pdbIdentity(const llvm::pdb::InfoStream &Info) {
  PDBBuildIdentity Identity;
  const llvm::codeview::GUID Guid = Info.getGuid();
  std::copy(std::begin(Guid.Guid), std::end(Guid.Guid), Identity.Guid.begin());
  Identity.Age = Info.getAge();
  return Identity;
}

constexpr bool machineMatches(llvm::pdb::PDB_Machine Machine, Arch ImageArch) {
  using llvm::pdb::PDB_Machine;
  switch (ImageArch) {
  case Arch::X64:
    return Machine == PDB_Machine::Amd64;
  case Arch::X86:
    return Machine == PDB_Machine::x86;
  case Arch::AArch64:
    return Machine == PDB_Machine::Arm64;
  case Arch::ARM:
    // The PE loader accepts IMAGE_FILE_MACHINE_ARMNT only.  Do not collapse
    // the unsupported legacy ARM machine value into the same Arch enum.
    return Machine == PDB_Machine::ArmNT;
  default:
    return false;
  }
}

static_assert(machineMatches(llvm::pdb::PDB_Machine::ArmNT, Arch::ARM));
static_assert(!machineMatches(llvm::pdb::PDB_Machine::Arm, Arch::ARM));

llvm::StringRef shortSectionName(const llvm::object::coff_section &Section) {
  size_t Length = 0;
  while (Length < llvm::COFF::NameSize && Section.Name[Length] != '\0')
    ++Length;
  return llvm::StringRef(Section.Name, Length);
}

llvm::Error validateSectionTable(
    const BinaryImage &Image,
    llvm::FixedStreamArray<llvm::object::coff_section> PDBSections) {
  if (PDBSections.size() != Image.Sections.size())
    return pdbLoadError("section table does not match loaded PE image");

  for (size_t I = 0; I < Image.Sections.size(); ++I) {
    const Section &Loaded = Image.Sections[I];
    const llvm::object::coff_section &Recorded = PDBSections[I];
    constexpr uint64_t MaxCOFFField = std::numeric_limits<uint32_t>::max();
    if (Loaded.VA < Image.Base || Loaded.Size > MaxCOFFField ||
        Loaded.FileOff > MaxCOFFField || Loaded.FileSz > MaxCOFFField ||
        shortSectionName(Recorded) != Loaded.Name ||
        static_cast<uint64_t>(Recorded.VirtualAddress) !=
            Loaded.VA - Image.Base ||
        static_cast<uint64_t>(Recorded.VirtualSize) != Loaded.Size ||
        static_cast<uint64_t>(Recorded.PointerToRawData) != Loaded.FileOff ||
        static_cast<uint64_t>(Recorded.SizeOfRawData) != Loaded.FileSz ||
        static_cast<uint32_t>(Recorded.Characteristics) != Loaded.Type)
      return pdbLoadError("section table does not match loaded PE image");
  }
  return llvm::Error::success();
}

TypeRef convertPDBType(const llvm::pdb::PDBSymbol &Symbol) {
  using namespace llvm::pdb;
  if (const auto *Builtin = llvm::dyn_cast<PDBSymbolTypeBuiltin>(&Symbol)) {
    const uint64_t Length = Builtin->getLength();
    switch (Builtin->getBuiltinType()) {
    case PDB_BuiltinType::Void:
      return NdType::makeVoid();
    case PDB_BuiltinType::Float:
      return Length ? NdType::makeFloat(static_cast<uint16_t>(Length))
                    : TypeRef{};
    case PDB_BuiltinType::UInt:
    case PDB_BuiltinType::ULong:
    case PDB_BuiltinType::Bool:
      return Length ? NdType::makeInt(static_cast<uint16_t>(Length), false)
                    : TypeRef{};
    case PDB_BuiltinType::Char:
    case PDB_BuiltinType::WCharT:
    case PDB_BuiltinType::Int:
    case PDB_BuiltinType::Long:
    case PDB_BuiltinType::HResult:
    case PDB_BuiltinType::Char8:
    case PDB_BuiltinType::Char16:
    case PDB_BuiltinType::Char32:
      return Length ? NdType::makeInt(static_cast<uint16_t>(Length), true)
                    : TypeRef{};
    default:
      return {};
    }
  }
  if (const auto *Pointer = llvm::dyn_cast<PDBSymbolTypePointer>(&Symbol)) {
    TypeRef Pointee;
    if (auto Inner = Pointer->getPointeeType())
      Pointee = convertPDBType(*Inner);
    if (!Pointee)
      Pointee = NdType::makeVoid();
    return NdType::makePtr(Pointee);
  }
  return {};
}

TypeRef functionReturnType(llvm::pdb::NativeSession &Session,
                           llvm::codeview::TypeIndex FunctionType) {
  using namespace llvm::pdb;
  const SymIndexId Id =
      Session.getSymbolCache().findSymbolByTypeIndex(FunctionType);
  if (Id == 0)
    return {};
  auto Symbol = Session.getSymbolById(Id);
  if (!Symbol)
    return {};
  auto Signature =
      llvm::unique_dyn_cast<PDBSymbolTypeFunctionSig>(std::move(Symbol));
  if (!Signature)
    return {};
  if (auto Return = Signature->getReturnType())
    return convertPDBType(*Return);
  return {};
}

} // namespace

struct PDBDebugContext::Impl {
  std::map<va_t, FunctionSym> Functions;
  bool ImageIdentityAuthenticated = false;
  bool Loaded = false;
};

PDBDebugContext::~PDBDebugContext() = default;

llvm::Expected<std::unique_ptr<PDBDebugContext>>
PDBDebugContext::load(const std::filesystem::path &PdbPath,
                      const BinaryImage &Image) {
  if (Image.Format != BinaryFormat::COFF || Image.IsRelocatable)
    return pdbLoadError("strict PDB loading requires a linked PE image");
  if (Image.DynInfo.CodeViewPDBIdentityState != PDBIdentityState::Unique ||
      !Image.DynInfo.CodeViewPDBIdentity)
    return pdbLoadError(
        Image.DynInfo.CodeViewPDBIdentityState == PDBIdentityState::Ambiguous
            ? "PE CodeView RSDS identity is malformed or ambiguous"
            : "PE image has no unique CodeView RSDS identity");

  auto Ctx = std::unique_ptr<PDBDebugContext>(new PDBDebugContext());
  Ctx->PImpl = std::make_unique<Impl>();

  std::unique_ptr<llvm::pdb::IPDBSession> Session;
  auto Err =
      llvm::pdb::loadDataForPDB(llvm::pdb::PDB_ReaderType::Native,
                                llvm::StringRef(PdbPath.string()), Session);
  if (Err)
    return llvm::createFileError(PdbPath.string(), std::move(Err));

  auto *Native = static_cast<llvm::pdb::NativeSession *>(Session.get());
  Session->setLoadAddress(Image.Base);
  auto &PDB = Native->getPDBFile();

  auto InfoOr = PDB.getPDBInfoStream();
  if (!InfoOr) {
    const std::string Detail = llvm::toString(InfoOr.takeError());
    return pdbLoadError("cannot read PDB Info stream: " + Detail);
  }
  auto &Info = *InfoOr;
  const PDBBuildIdentity ActualIdentity = pdbIdentity(Info);
  if (!ActualIdentity.isValid())
    return pdbLoadError("PDB Info stream has an invalid GUID/age");
  if (ActualIdentity != *Image.DynInfo.CodeViewPDBIdentity)
    return pdbLoadError("PDB Info GUID/age does not match PE CodeView RSDS");

  auto DbiOr = PDB.getPDBDbiStream();
  if (!DbiOr) {
    const std::string Detail = llvm::toString(DbiOr.takeError());
    return pdbLoadError("cannot read DBI stream: " + Detail);
  }
  auto &DBI = *DbiOr;
  if (DBI.getAge() != Info.getAge())
    return pdbLoadError("DBI age does not match PDB Info age");
  if (!machineMatches(DBI.getMachineType(), Image.Arch))
    return pdbLoadError("DBI machine does not match loaded PE image");

  auto SecHeaders = DBI.getSectionHeaders();
  if (auto SectionError = validateSectionTable(Image, SecHeaders))
    return std::move(SectionError);

  Ctx->PImpl->ImageIdentityAuthenticated = true;
  // Phase A authenticates the PE/PDB pairing and transactionally validates
  // the symbol streams used for names.  Merely having a TPI stream does not
  // prove that every referenced type record is well-formed, so do not enter
  // LLVM's lazy type graph until Phase B validates that graph explicitly.
  constexpr bool HasTPI = false;

  auto ResolveVA = [&](uint16_t Seg, uint32_t Off) -> va_t {
    if (Seg == 0 || Seg > Image.Sections.size())
      return 0;
    const Section &Owner = Image.Sections[Seg - 1];
    if (Off >= Owner.Size || Off > InvalidVA - Owner.VA)
      return 0;
    return Owner.VA + Off;
  };

  pdb_loader_detail::FunctionNameRegistry FunctionNames;
  std::set<va_t> FunctionAddresses;
  if (PDB.hasPDBPublicsStream()) {
    auto PubOr = PDB.getPDBPublicsStream();
    if (!PubOr) {
      const std::string Detail = llvm::toString(PubOr.takeError());
      return pdbLoadError("cannot read Publics stream: " + Detail);
    }
    auto &Publics = *PubOr;
    if (!PDB.hasPDBSymbolStream())
      return pdbLoadError("Publics stream has no backing symbol stream");
    auto SymOr = PDB.getPDBSymbolStream();
    if (!SymOr) {
      const std::string Detail = llvm::toString(SymOr.takeError());
      return pdbLoadError("cannot read backing symbol stream: " + Detail);
    }
    auto IndexedOr =
        pdb_loader_detail::indexSymbolRecords(SymOr->getSymbolArray());
    if (!IndexedOr) {
      const std::string Detail = llvm::toString(IndexedOr.takeError());
      return pdbLoadError("invalid backing symbol stream: " + Detail);
    }

    for (uint32_t Off : Publics.getPublicsTable()) {
      const llvm::codeview::CVSymbol *CVS =
          pdb_loader_detail::findSymbolAtExactOffset(*IndexedOr, Off);
      if (!CVS)
        return pdbLoadError(
            "Publics GSI offset is not an exact symbol record boundary");
      if (CVS->kind() != llvm::codeview::SymbolKind::S_PUB32)
        return pdbLoadError("Publics GSI references a non-public symbol");

      llvm::codeview::PublicSym32 PubRec(
          llvm::codeview::SymbolRecordKind::PublicSym32);
      if (auto Error = llvm::codeview::SymbolDeserializer::deserializeAs<
              llvm::codeview::PublicSym32>(*CVS, PubRec)) {
        const std::string Detail = llvm::toString(std::move(Error));
        return pdbLoadError("invalid public symbol record: " + Detail);
      }

      const bool IsFunc =
          (static_cast<uint32_t>(PubRec.Flags) &
           static_cast<uint32_t>(llvm::codeview::PublicSymFlags::Function)) !=
          0;
      if (!IsFunc)
        continue;

      const va_t VA = ResolveVA(PubRec.Segment, PubRec.Offset);
      if (VA != 0) {
        FunctionAddresses.insert(VA);
        FunctionNames.observe(VA, PubRec.Name);
      }
    }
  }

  for (uint32_t ModuleIndex = 0; ModuleIndex < DBI.modules().getModuleCount();
       ++ModuleIndex) {
    const auto Descriptor = DBI.modules().getModuleDescriptor(ModuleIndex);
    if (Descriptor.getModuleStreamIndex() == llvm::pdb::kInvalidStreamIndex)
      continue;
    auto ModuleOr = Native->getModuleDebugStream(ModuleIndex);
    if (!ModuleOr) {
      const std::string Detail = llvm::toString(ModuleOr.takeError());
      return pdbLoadError("cannot read module debug stream " +
                          llvm::Twine(ModuleIndex) + ": " + Detail);
    }
    auto IndexedOr =
        pdb_loader_detail::indexSymbolRecords(ModuleOr->getSymbolArray());
    if (!IndexedOr) {
      const std::string Detail = llvm::toString(IndexedOr.takeError());
      return pdbLoadError("invalid module symbol stream " +
                          llvm::Twine(ModuleIndex) + ": " + Detail);
    }
    for (const pdb_loader_detail::IndexedSymbolRecord &Indexed : *IndexedOr) {
      const llvm::codeview::CVSymbol &Record = Indexed.Symbol;
      if (Record.kind() != llvm::codeview::SymbolKind::S_LPROC32 &&
          Record.kind() != llvm::codeview::SymbolKind::S_GPROC32)
        continue;
      auto ProcOr = llvm::codeview::SymbolDeserializer::deserializeAs<
          llvm::codeview::ProcSym>(Record);
      if (!ProcOr) {
        const std::string Detail = llvm::toString(ProcOr.takeError());
        return pdbLoadError("invalid procedure symbol record in module " +
                            llvm::Twine(ModuleIndex) + ": " + Detail);
      }
      const auto &Proc = *ProcOr;
      const va_t VA = ResolveVA(Proc.Segment, Proc.CodeOffset);
      if (VA == 0)
        continue;
      FunctionAddresses.insert(VA);
      FunctionNames.observe(VA, Proc.Name);
      if (HasTPI) {
        FunctionSym &FS = Ctx->PImpl->Functions[VA];
        FS.Addr = VA;
        FS.ReturnType = functionReturnType(*Native, Proc.FunctionType);
      }
    }
  }

  for (const va_t VA : FunctionAddresses) {
    const std::optional<std::string> Name = FunctionNames.name(VA);
    if (!Name) {
      Ctx->PImpl->Functions.erase(VA);
      continue;
    }
    FunctionSym &FS = Ctx->PImpl->Functions[VA];
    FS.Addr = VA;
    FS.Name = *Name;
  }
  Ctx->PImpl->Loaded = !Ctx->PImpl->Functions.empty();
  LLVM_DEBUG(llvm::dbgs() << "pdb: loaded " << Ctx->PImpl->Functions.size()
                          << " function symbols from "
                          << PdbPath.filename().string() << "\n");
  return Ctx;
}

std::optional<FunctionSym> PDBDebugContext::resolveFunction(va_t Addr) const {
  if (!PImpl)
    return std::nullopt;
  auto It = PImpl->Functions.find(Addr);
  if (It != PImpl->Functions.end())
    return It->second;
  auto LB = PImpl->Functions.lower_bound(Addr);
  if (LB != PImpl->Functions.begin()) {
    --LB;
    if (LB->second.contains(Addr))
      return LB->second;
  }
  return std::nullopt;
}

std::optional<VariableSym> PDBDebugContext::resolveVariable(va_t,
                                                            int64_t) const {
  return std::nullopt;
}

std::optional<TypeSym> PDBDebugContext::resolveType(uint64_t) const {
  return std::nullopt;
}

std::optional<SourceLoc> PDBDebugContext::sourceLocation(va_t) const {
  return std::nullopt;
}

std::vector<FunctionSym> PDBDebugContext::allFunctions() const {
  std::vector<FunctionSym> Result;
  if (!PImpl)
    return Result;
  Result.reserve(PImpl->Functions.size());
  for (auto &[_, FS] : PImpl->Functions)
    Result.push_back(FS);
  return Result;
}

bool PDBDebugContext::hasInfo() const { return PImpl && PImpl->Loaded; }

bool PDBDebugContext::hasAuthenticatedImageIdentity() const {
  return PImpl && PImpl->ImageIdentityAuthenticated;
}

bool PDBDebugContext::hasExactObjectMetadataPrerequisites() const {
  // Phase A authenticates only the image/PDB identity chain and function
  // names.  It deliberately cannot authorize exact object recovery until
  // Phase B transactionally validates and consumes the owning type/data
  // streams.
  return false;
}

} // namespace neverd
