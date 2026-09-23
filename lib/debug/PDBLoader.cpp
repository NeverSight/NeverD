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
#include "neverd/support/Parallel.h"

#define DEBUG_TYPE "neverd-pdb-loader"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/CodeView/GUID.h"
#include "llvm/DebugInfo/CodeView/RecordSerialization.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptor.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/ModuleDebugStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/MSF/MappedBlockStream.h"
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
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/BinaryStreamRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
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

namespace {
bool isPostLinkRewrittenSection(llvm::StringRef Name) {
  return Name == ".rsrc" || Name == ".reloc";
}
} // namespace

bool pdb_loader_detail::recordedSectionsMatch(
    const BinaryImage &Image, llvm::ArrayRef<RecordedSection> Recorded) {
  if (Recorded.size() != Image.Sections.size())
    return false;
  // Sections from FirstMovable on may be re-laid out; see the declaration.
  size_t FirstMovable = Image.Sections.size();
  while (FirstMovable > 0 &&
         isPostLinkRewrittenSection(Image.Sections[FirstMovable - 1].Name))
    --FirstMovable;
  constexpr uint64_t MaxCOFFField = std::numeric_limits<uint32_t>::max();
  for (size_t I = 0; I < Image.Sections.size(); ++I) {
    const Section &Loaded = Image.Sections[I];
    const RecordedSection &Header = Recorded[I];
    if (Loaded.VA < Image.Base || Loaded.Size > MaxCOFFField ||
        Loaded.FileOff > MaxCOFFField || Loaded.FileSz > MaxCOFFField ||
        Header.Name != Loaded.Name || Header.Characteristics != Loaded.Type)
      return false;
    if (I >= FirstMovable)
      continue;
    if (static_cast<uint64_t>(Header.VirtualAddress) !=
            Loaded.VA - Image.Base ||
        static_cast<uint64_t>(Header.VirtualSize) != Loaded.Size ||
        static_cast<uint64_t>(Header.PointerToRawData) != Loaded.FileOff ||
        static_cast<uint64_t>(Header.SizeOfRawData) != Loaded.FileSz)
      return false;
  }
  return true;
}

llvm::Error pdb_loader_detail::parsePublicSym32At(llvm::ArrayRef<uint8_t> Stream,
                                                  uint32_t Offset,
                                                  ParsedPublicSym32 &Out) {
  constexpr uint32_t PrefixSize =
      static_cast<uint32_t>(sizeof(llvm::codeview::RecordPrefix));
  constexpr uint32_t HeaderSize =
      static_cast<uint32_t>(sizeof(llvm::codeview::PublicSym32Header));
  if (Offset > Stream.size() || Stream.size() - Offset < PrefixSize)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "pdb: Publics GSI offset is not an exact "
                                   "symbol record boundary");

  const uint8_t *const Bytes = Stream.data() + Offset;
  const uint16_t RecordLen = llvm::support::endian::read16le(Bytes);
  const uint16_t Kind = llvm::support::endian::read16le(Bytes + 2);
  if (RecordLen < 2)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "pdb: Publics GSI offset is not an exact "
                                   "symbol record boundary");
  const uint32_t Total = static_cast<uint32_t>(RecordLen) + 2;
  if (Stream.size() - Offset < Total)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "pdb: Publics GSI offset is not an exact "
                                   "symbol record boundary");
  if (Kind != static_cast<uint16_t>(llvm::codeview::SymbolKind::S_PUB32))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "pdb: Publics GSI references a non-public "
                                   "symbol");
  if (RecordLen < 2 + HeaderSize + 1)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "pdb: invalid public symbol record");

  const uint8_t *const Body = Bytes + PrefixSize;
  const uint32_t Flags = llvm::support::endian::read32le(Body);
  Out.IsFunction =
      (Flags & static_cast<uint32_t>(
                   llvm::codeview::PublicSymFlags::Function)) != 0;
  Out.Offset = llvm::support::endian::read32le(Body + 4);
  Out.Segment = llvm::support::endian::read16le(Body + 8);
  const char *const Name = reinterpret_cast<const char *>(Body + HeaderSize);
  const size_t MaxName = Total - PrefixSize - HeaderSize;
  const size_t NameLen = ::strnlen(Name, MaxName);
  if (NameLen == MaxName)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "pdb: invalid public symbol record");
  Out.Name = llvm::StringRef(Name, NameLen);
  return llvm::Error::success();
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

llvm::Error forEachProcedureSymbol(
    const llvm::codeview::CVSymbolArray &Records,
    llvm::function_ref<llvm::Error(const llvm::codeview::ProcSym &)> OnProc) {
  bool HadError = false;
  auto It = llvm::codeview::CVSymbolArray::Iterator(
      Records, Records.getExtractor(), Records.skew(), &HadError);
  const auto End = Records.end();
  for (; It != End; ++It) {
    const llvm::codeview::SymbolKind Kind = It->kind();
    if (Kind != llvm::codeview::SymbolKind::S_LPROC32 &&
        Kind != llvm::codeview::SymbolKind::S_GPROC32)
      continue;
    auto ProcOr = llvm::codeview::SymbolDeserializer::deserializeAs<
        llvm::codeview::ProcSym>(*It);
    if (!ProcOr)
      return ProcOr.takeError();
    if (llvm::Error E = OnProc(*ProcOr))
      return E;
  }
  if (HadError)
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "malformed CodeView symbol record stream");
  return llvm::Error::success();
}

namespace {

llvm::Error pdbLoadError(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "pdb: " + Message);
}

// The PE's RSDS age names the link that wrote the DBI stream.  The Info
// stream age is a PDB write counter: pdbcopy, pdbstr, and symbol-server
// publishing bump it without relinking, so a public Windows PDB may carry an
// Info age above the DBI and RSDS ages.  Match on the DBI age when the DBI
// header records one, as the MSVC debugger and Ghidra do.
PDBBuildIdentity pdbIdentity(const llvm::pdb::InfoStream &Info,
                             const llvm::pdb::DbiStream &DBI) {
  PDBBuildIdentity Identity;
  Identity.Kind = PDBIdentityKind::RSDS;
  const llvm::codeview::GUID Guid = Info.getGuid();
  std::copy(std::begin(Guid.Guid), std::end(Guid.Guid), Identity.Guid.begin());
  Identity.Age = DBI.getAge() != 0 ? DBI.getAge() : Info.getAge();
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
  std::vector<pdb_loader_detail::RecordedSection> Recorded;
  Recorded.reserve(PDBSections.size());
  for (const llvm::object::coff_section &Header : PDBSections)
    Recorded.push_back({shortSectionName(Header), Header.VirtualAddress,
                        Header.VirtualSize, Header.PointerToRawData,
                        Header.SizeOfRawData, Header.Characteristics});
  if (!pdb_loader_detail::recordedSectionsMatch(Image, Recorded))
    return pdbLoadError("section table does not match loaded PE image");
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
  std::map<va_t, DataObjectSym> DataObjects;
  std::map<va_t, std::map<int64_t, VariableSym>> Locals;
  std::map<va_t, std::set<int64_t>> AmbiguousLocalOffsets;
  bool ImageIdentityAuthenticated = false;
  bool FunctionSignaturesAuthenticated = false;
  bool ObjectExtentsAuthenticated = false;
  bool Loaded = false;
};

PDBDebugContext::PDBDebugContext() = default;
PDBDebugContext::~PDBDebugContext() = default;

void PDBDebugContext::commitFunctions(std::vector<FunctionSym> Functions,
                                      bool Authenticated) {
  commitDebugFacts(std::move(Functions), {}, Authenticated, false, false);
}

void PDBDebugContext::commitDebugFacts(
    std::vector<FunctionSym> Functions,
    std::map<va_t, std::map<int64_t, VariableSym>> Locals, bool Authenticated,
    bool Signatures, bool Extents) {
  PImpl = std::make_unique<Impl>();
  for (FunctionSym &FS : Functions)
    PImpl->Functions[FS.Addr] = std::move(FS);
  PImpl->Locals = std::move(Locals);
  PImpl->ImageIdentityAuthenticated = Authenticated;
  PImpl->FunctionSignaturesAuthenticated = Signatures;
  PImpl->ObjectExtentsAuthenticated = Extents;
  PImpl->Loaded = !PImpl->Functions.empty();
}

llvm::Expected<std::unique_ptr<PDBDebugContext>>
PDBDebugContext::load(const std::filesystem::path &PdbPath,
                      const BinaryImage &Image, const LoadProgress &Progress) {
  if (Image.Format != BinaryFormat::COFF || Image.IsRelocatable)
    return pdbLoadError("strict PDB loading requires a linked PE image");
  if (Image.DynInfo.CodeViewPDBIdentityState != PDBIdentityState::Unique ||
      !Image.DynInfo.CodeViewPDBIdentity)
    return pdbLoadError(
        Image.DynInfo.CodeViewPDBIdentityState == PDBIdentityState::Ambiguous
            ? "PE CodeView identity is malformed or ambiguous"
            : "PE image has no unique CodeView identity");
  if (Image.DynInfo.CodeViewPDBIdentity->Kind == PDBIdentityKind::NB10)
    return loadPdb20DebugContext(PdbPath, Image);

  auto Ctx = std::unique_ptr<PDBDebugContext>(new PDBDebugContext());
  Ctx->PImpl = std::make_unique<Impl>();

  Progress.report("debug", 0, 1, "opening pdb");
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

  auto DbiOr = PDB.getPDBDbiStream();
  if (!DbiOr) {
    const std::string Detail = llvm::toString(DbiOr.takeError());
    return pdbLoadError("cannot read DBI stream: " + Detail);
  }
  auto &DBI = *DbiOr;
  const PDBBuildIdentity ActualIdentity = pdbIdentity(Info, DBI);
  if (!ActualIdentity.isValid())
    return pdbLoadError("PDB Info stream has an invalid GUID/age");
  if (ActualIdentity != *Image.DynInfo.CodeViewPDBIdentity)
    return pdbLoadError("PDB Info GUID/age does not match PE CodeView RSDS");
  if (DBI.getAge() > Info.getAge())
    return pdbLoadError("DBI age is newer than PDB Info age");
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
  pdb_loader_detail::FunctionNameRegistry DataNames;
  std::set<va_t> FunctionAddresses;
  std::set<va_t> DataAddresses;
  if (PDB.hasPDBPublicsStream()) {
    auto PubOr = PDB.getPDBPublicsStream();
    if (!PubOr) {
      const std::string Detail = llvm::toString(PubOr.takeError());
      return pdbLoadError("cannot read Publics stream: " + Detail);
    }
    auto &Publics = *PubOr;
    const uint32_t SymIndex = DBI.getSymRecordStreamIndex();
    if (SymIndex == llvm::pdb::kInvalidStreamIndex ||
        SymIndex >= PDB.getNumStreams())
      return pdbLoadError("Publics stream has no backing symbol stream");

    // Address map is a dense list of S_PUB32 offsets.  Copy it once so
    // FixedStreamArray does not issue a MappedBlockStream read per public.
    llvm::ArrayRef<uint8_t> AddrBytes;
    {
      llvm::BinaryStreamRef AddrStream =
          Publics.getAddressMap().getUnderlyingStream();
      const uint64_t AddrLen = AddrStream.getLength();
      if (AddrLen > 0) {
        if (auto ReadErr = AddrStream.readBytes(0, AddrLen, AddrBytes)) {
          llvm::consumeError(std::move(ReadErr));
          return pdbLoadError("cannot read Publics address map");
        }
      }
    }
    if (AddrBytes.size() % sizeof(uint32_t) != 0)
      return pdbLoadError("Publics address map is truncated");
    const uint32_t PubCount =
        static_cast<uint32_t>(AddrBytes.size() / sizeof(uint32_t));
    if (PubCount != 0) {
      // LLVM's SymbolStream::reload() indexes every global symbol.  Phase A
      // only needs the publics the address map points at, so linearize the
      // backing stream once and parse those records from the contiguous view.
      Progress.report("debug", 0, 1, "pdb symbol stream");
      std::unique_ptr<llvm::msf::MappedBlockStream> SymStream =
          PDB.createIndexedStream(static_cast<uint16_t>(SymIndex));
      if (!SymStream)
        return pdbLoadError("cannot read backing symbol stream");
      llvm::BinaryStreamReader SymReader(*SymStream);
      const uint64_t SymLen = SymReader.getLength();
      if (SymLen > std::numeric_limits<uint32_t>::max())
        return pdbLoadError("backing symbol stream is too large");
      llvm::ArrayRef<uint8_t> SymBytes;
      if (SymLen > 0) {
        if (auto ReadErr =
                SymReader.readBytes(SymBytes, static_cast<uint32_t>(SymLen)))
          return pdbLoadError("cannot read backing symbol stream: " +
                              llvm::toString(std::move(ReadErr)));
      }

      Progress.report("debug", 0, PubCount, "pdb publics");
      for (uint32_t I = 0; I < PubCount; ++I) {
        const uint32_t Off = llvm::support::endian::read32le(
            AddrBytes.data() + I * sizeof(uint32_t));
        pdb_loader_detail::ParsedPublicSym32 Pub;
        if (auto ParseErr =
                pdb_loader_detail::parsePublicSym32At(SymBytes, Off, Pub))
          return ParseErr;
        const va_t VA = ResolveVA(Pub.Segment, Pub.Offset);
        if (VA == 0)
          continue;
        if (Pub.IsFunction) {
          FunctionAddresses.insert(VA);
          FunctionNames.observe(VA, Pub.Name);
        } else {
          DataAddresses.insert(VA);
          DataNames.observe(VA, Pub.Name);
        }
        const uint32_t Done = I + 1;
        if (Done == PubCount || (Done % 4096u) == 0)
          Progress.report("debug", Done, PubCount, "pdb publics");
      }
    }
  }

  // Publics already named the linked functions.  Walking every module
  // symbol stream is what made a large PDB stall load for tens of seconds
  // with no UI; do that only when Publics did not contribute any function.
  const bool NeedModuleWalk = FunctionAddresses.empty();
  const uint32_t ModuleCount =
      NeedModuleWalk ? DBI.modules().getModuleCount() : 0;
  if (NeedModuleWalk && ModuleCount > 0)
    Progress.report("debug", 0, ModuleCount, "pdb modules");
  std::mutex NamesMutex;
  std::mutex ErrorMutex;
  std::string ModuleError;
  std::atomic<bool> Failed{false};
  std::atomic<uint32_t> Finished{0};
  std::mutex ProgressMutex;
  auto ScanModule = [&](uint32_t ModuleIndex) {
    if (Failed.load(std::memory_order_relaxed))
      return;
    const auto Descriptor = DBI.modules().getModuleDescriptor(ModuleIndex);
    if (Descriptor.getModuleStreamIndex() == llvm::pdb::kInvalidStreamIndex)
      return;
    llvm::Expected<llvm::pdb::ModuleDebugStreamRef> ModuleOr =
        Native->getModuleDebugStream(ModuleIndex);
    if (!ModuleOr) {
      const std::string Detail = llvm::toString(ModuleOr.takeError());
      std::lock_guard<std::mutex> Guard(ErrorMutex);
      if (!Failed.exchange(true))
        ModuleError = ("cannot read module debug stream " +
                       llvm::Twine(ModuleIndex) + ": " + Detail)
                          .str();
      return;
    }
    llvm::Error ScanErr = forEachProcedureSymbol(
        ModuleOr->getSymbolArray(), [&](const llvm::codeview::ProcSym &Proc) {
          const va_t VA = ResolveVA(Proc.Segment, Proc.CodeOffset);
          if (VA == 0)
            return llvm::Error::success();
          std::lock_guard<std::mutex> Guard(NamesMutex);
          FunctionAddresses.insert(VA);
          FunctionNames.observe(VA, Proc.Name);
          if (HasTPI) {
            FunctionSym &FS = Ctx->PImpl->Functions[VA];
            FS.Addr = VA;
            FS.ReturnType = functionReturnType(*Native, Proc.FunctionType);
          }
          return llvm::Error::success();
        });
    if (ScanErr) {
      const std::string Detail = llvm::toString(std::move(ScanErr));
      std::lock_guard<std::mutex> Guard(ErrorMutex);
      if (!Failed.exchange(true))
        ModuleError = ("invalid module symbol stream " +
                       llvm::Twine(ModuleIndex) + ": " + Detail)
                          .str();
    }
  };
  if (ModuleCount == 0) {
    if (NeedModuleWalk)
      Progress.report("debug", 1, 1, "pdb modules");
  } else {
    parallelForEach(ModuleCount, [&](auto Claim, size_t Total) {
      for (size_t Index = Claim(); Index < Total; Index = Claim()) {
        ScanModule(static_cast<uint32_t>(Index));
        const uint32_t Done =
            Finished.fetch_add(1, std::memory_order_relaxed) + 1;
        if (Done == ModuleCount || (Done % 16) == 0) {
          std::lock_guard<std::mutex> Guard(ProgressMutex);
          Progress.report("debug", Done, ModuleCount, "pdb modules");
        }
      }
    });
  }
  if (Failed.load(std::memory_order_relaxed))
    return pdbLoadError(ModuleError);

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
  for (const va_t VA : DataAddresses) {
    const std::optional<std::string> Name = DataNames.name(VA);
    if (!Name)
      continue;
    DataObjectSym &Obj = Ctx->PImpl->DataObjects[VA];
    Obj.Name = *Name;
    Obj.Addr = VA;
    Obj.Size = 0;
    Obj.IsBuffer = false;
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

std::optional<VariableSym> PDBDebugContext::resolveVariable(va_t FuncAddr,
                                                            int64_t Offset) const {
  if (!PImpl)
    return std::nullopt;
  const auto AmbIt = PImpl->AmbiguousLocalOffsets.find(FuncAddr);
  if (AmbIt != PImpl->AmbiguousLocalOffsets.end() &&
      AmbIt->second.count(Offset))
    return std::nullopt;
  const auto FuncIt = PImpl->Locals.find(FuncAddr);
  if (FuncIt == PImpl->Locals.end())
    return std::nullopt;
  const auto OffIt = FuncIt->second.find(Offset);
  if (OffIt == FuncIt->second.end())
    return std::nullopt;
  return OffIt->second;
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

std::vector<DataObjectSym> PDBDebugContext::allDataObjects() const {
  std::vector<DataObjectSym> Result;
  if (!PImpl)
    return Result;
  Result.reserve(PImpl->DataObjects.size());
  for (auto &[_, Obj] : PImpl->DataObjects)
    Result.push_back(Obj);
  return Result;
}

bool PDBDebugContext::hasInfo() const { return PImpl && PImpl->Loaded; }

bool PDBDebugContext::hasAuthenticatedImageIdentity() const {
  return PImpl && PImpl->ImageIdentityAuthenticated;
}

bool PDBDebugContext::hasAuthenticatedFunctionSignatures() const {
  return PImpl && PImpl->FunctionSignaturesAuthenticated;
}

bool PDBDebugContext::hasAuthenticatedObjectExtents() const {
  return PImpl && PImpl->ObjectExtentsAuthenticated;
}

bool PDBDebugContext::hasExactObjectMetadataPrerequisites() const {
  // RSDS Phase A remains names-only.  JG TPI + S_BPREL32_ST recovery is the
  // only PDB path that currently authorizes exact locals/params.
  return PImpl && PImpl->ObjectExtentsAuthenticated;
}

} // namespace neverd
