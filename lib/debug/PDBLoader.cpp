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

#include "neverd/Common.h"
#include "neverd/Limits.h"
#include "neverd/ir/NdTypes.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/support/Parallel.h"

#define DEBUG_TYPE "neverd-pdb-loader"
#include "llvm/ADT/STLFunctionalExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/CodeView/GUID.h"
#include "llvm/DebugInfo/CodeView/RecordSerialization.h"
#include "llvm/DebugInfo/CodeView/SymbolDeserializer.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/CVTypeVisitor.h"
#include "llvm/DebugInfo/CodeView/TypeDeserializer.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/CodeView/TypeVisitorCallbacks.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptor.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/ISectionContribVisitor.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/ModuleDebugStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/RawTypes.h"
#include "llvm/DebugInfo/MSF/MappedBlockStream.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/PublicsStream.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/Native/SymbolStream.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/DebugInfo/PDB/PDB.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/BinaryStreamRef.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <thread>
#include <set>
#include <tuple>
#include <utility>

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

llvm::Error parsePublicSym32FromStream(llvm::BinaryStreamRef Stream,
                                       uint32_t Offset,
                                       pdb_loader_detail::ParsedPublicSym32 &Out) {
  llvm::BinaryStreamReader Reader(Stream);
  Reader.setOffset(Offset);
  llvm::ArrayRef<uint8_t> Prefix;
  if (auto Err = Reader.readBytes(Prefix, 4))
    return Err;
  const uint16_t RecordLen = llvm::support::endian::read16le(Prefix.data());
  if (RecordLen < 2)
    return pdbLoadError("invalid public symbol record");
  const uint32_t Total = static_cast<uint32_t>(RecordLen) + 2;
  Reader.setOffset(Offset);
  llvm::ArrayRef<uint8_t> Record;
  if (auto Err = Reader.readBytes(Record, Total))
    return Err;
  return pdb_loader_detail::parsePublicSym32At(Record, 0, Out);
}

PDBBuildIdentity pdbIdentity(const llvm::pdb::InfoStream &Info) {
  PDBBuildIdentity Identity;
  Identity.Kind = PDBIdentityKind::RSDS;
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

TypeRef namedPrimitive(uint16_t Size, bool Signed, llvm::StringRef Spelling) {
  auto Ty = NdType::makeInt(Size, Signed);
  Ty->SourceName = Spelling.str();
  return Ty;
}

TypeRef primitiveType(llvm::codeview::TypeIndex Index, uint16_t PtrSize) {
  using llvm::codeview::SimpleTypeKind;
  using llvm::codeview::SimpleTypeMode;
  if (!Index.isSimple() || Index.isNoneType())
    return {};
  TypeRef Base;
  switch (Index.getSimpleKind()) {
  case SimpleTypeKind::Void:
    Base = NdType::makeVoid();
    break;
  case SimpleTypeKind::HResult:
    Base = namedPrimitive(4, true, "HRESULT");
    break;
  case SimpleTypeKind::Int32Long:
  case SimpleTypeKind::Int32:
    Base = NdType::makeInt(4, true);
    break;
  case SimpleTypeKind::UInt32Long:
  case SimpleTypeKind::UInt32:
    Base = NdType::makeInt(4, false);
    break;
  case SimpleTypeKind::SignedCharacter:
    Base = namedPrimitive(1, true, "signed char");
    break;
  case SimpleTypeKind::SByte:
    Base = NdType::makeInt(1, true);
    break;
  case SimpleTypeKind::NarrowCharacter:
    Base = namedPrimitive(1, true, "char");
    break;
  case SimpleTypeKind::UnsignedCharacter:
    Base = namedPrimitive(1, false, "unsigned char");
    break;
  case SimpleTypeKind::Byte:
    Base = NdType::makeInt(1, false);
    break;
  case SimpleTypeKind::Boolean8:
    Base = namedPrimitive(1, false, "bool");
    break;
  case SimpleTypeKind::Int16Short:
  case SimpleTypeKind::Int16:
    Base = NdType::makeInt(2, true);
    break;
  case SimpleTypeKind::WideCharacter:
    Base = namedPrimitive(2, true, "wchar_t");
    break;
  case SimpleTypeKind::Character16:
    Base = namedPrimitive(2, false, "char16_t");
    break;
  case SimpleTypeKind::UInt16Short:
  case SimpleTypeKind::UInt16:
  case SimpleTypeKind::Boolean16:
    Base = NdType::makeInt(2, false);
    break;
  case SimpleTypeKind::Character32:
    Base = namedPrimitive(4, false, "char32_t");
    break;
  case SimpleTypeKind::Int64Quad:
  case SimpleTypeKind::Int64:
    Base = NdType::makeInt(8, true);
    break;
  case SimpleTypeKind::UInt64Quad:
  case SimpleTypeKind::UInt64:
  case SimpleTypeKind::Boolean64:
    Base = NdType::makeInt(8, false);
    break;
  case SimpleTypeKind::Float32:
    Base = NdType::makeFloat(4);
    break;
  case SimpleTypeKind::Float64:
    Base = NdType::makeFloat(8);
    break;
  case SimpleTypeKind::Float80:
    Base = NdType::makeFloat(10);
    break;
  default:
    return {};
  }
  uint16_t Size = PtrSize;
  switch (Index.getSimpleMode()) {
  case SimpleTypeMode::Direct:
    return Base;
  case SimpleTypeMode::NearPointer64:
    Size = 8;
    break;
  case SimpleTypeMode::NearPointer128:
    Size = 16;
    break;
  case SimpleTypeMode::NearPointer:
  case SimpleTypeMode::FarPointer:
  case SimpleTypeMode::HugePointer:
    Size = 2;
    break;
  case SimpleTypeMode::NearPointer32:
  case SimpleTypeMode::FarPointer32:
    Size = 4;
    break;
  }
  auto Ptr = NdType::makePtr(Base ? Base : NdType::makeVoid());
  Ptr->Size = Size;
  return Ptr;
}

std::string udtSpelling(llvm::StringRef Name, llvm::StringRef UniqueName) {
  if (const std::string FromUnique = msvcRttiTypeSpelling(UniqueName);
      !FromUnique.empty())
    return FromUnique;
  if (const std::string FromName = msvcUdtDisplayName(Name); !FromName.empty())
    return FromName;
  return msvcUdtDisplayName(UniqueName);
}

uint16_t boundedTypeSize(uint64_t Size) {
  return Size > 0 && Size <= std::numeric_limits<uint16_t>::max()
             ? static_cast<uint16_t>(Size)
             : 0;
}

llvm::codeview::CPUType cpuTypeFromMachine(llvm::pdb::PDB_Machine Machine) {
  using llvm::pdb::PDB_Machine;
  using llvm::codeview::CPUType;
  switch (Machine) {
  case PDB_Machine::Amd64:
    return CPUType::X64;
  case PDB_Machine::x86:
    return CPUType::Pentium3;
  case PDB_Machine::Arm64:
    return CPUType::ARM64;
  case PDB_Machine::ArmNT:
    return CPUType::ARMNT;
  default:
    return CPUType::Unknown;
  }
}

bool isIntegerArgumentRegister(llvm::codeview::RegisterId Reg) {
  using llvm::codeview::RegisterId;
  return Reg == RegisterId::RCX || Reg == RegisterId::RDX ||
         Reg == RegisterId::R8 || Reg == RegisterId::R9 ||
         Reg == RegisterId::ECX || Reg == RegisterId::EDX;
}

bool isStackPointerRegister(llvm::codeview::RegisterId Reg) {
  using llvm::codeview::RegisterId;
  return Reg == RegisterId::RSP || Reg == RegisterId::ESP ||
         Reg == RegisterId::SP || Reg == RegisterId::ARM64_SP;
}

bool isFramePointerRegister(llvm::codeview::RegisterId Reg) {
  using llvm::codeview::RegisterId;
  return Reg == RegisterId::RBP || Reg == RegisterId::EBP ||
         Reg == RegisterId::ARM64_FP;
}

bool isProcedureSymbol(llvm::codeview::SymbolKind Kind) {
  using llvm::codeview::SymbolKind;
  return Kind == SymbolKind::S_LPROC32 || Kind == SymbolKind::S_GPROC32 ||
         Kind == SymbolKind::S_LPROC32_ID || Kind == SymbolKind::S_GPROC32_ID ||
         Kind == SymbolKind::S_LPROC32_DPC ||
         Kind == SymbolKind::S_LPROC32_DPC_ID;
}

bool isProcedureIdSymbol(llvm::codeview::SymbolKind Kind) {
  using llvm::codeview::SymbolKind;
  return Kind == SymbolKind::S_LPROC32_ID || Kind == SymbolKind::S_GPROC32_ID ||
         Kind == SymbolKind::S_LPROC32_DPC_ID;
}

} // namespace

void pdb_loader_detail::publishNamedOffset(std::map<int64_t, VariableSym> &Slots,
                                           std::set<int64_t> &Ambiguous,
                                           int64_t Offset, VariableSym VS) {
  if (Ambiguous.count(Offset))
    return;
  auto IsHomeParam = [](const VariableSym &V) {
    return V.IsParam || V.Name == "this" || V.Name == "result";
  };
  if (IsHomeParam(VS))
    VS.IsParam = true;
  auto [It, Inserted] = Slots.emplace(Offset, VS);
  if (Inserted)
    return;
  if (It->second.Name == VS.Name) {
    if (!VS.IsParam)
      It->second.IsParam = false;
    return;
  }
  if (IsHomeParam(It->second) != IsHomeParam(VS)) {
    if (IsHomeParam(It->second))
      It->second = std::move(VS);
    return;
  }
  Slots.erase(It);
  Ambiguous.insert(Offset);
}

struct PDBDebugContext::Impl {
  std::unique_ptr<llvm::pdb::IPDBSession> Session;
  llvm::pdb::NativeSession *Native = nullptr;
  llvm::codeview::CPUType CPU = llvm::codeview::CPUType::Unknown;
  std::vector<Section> Sections;

  struct ModuleRange {
    va_t Start = 0;
    va_t End = 0;
    uint16_t Module = 0;
  };
  std::vector<ModuleRange> Contribs;

  /// Address-map + symbol stream kept only when `--func` asked for selected
  /// entries.  Full-image loads still ingest every public and drop the index.
  std::vector<uint8_t> PublicAddrBytes;
  std::unique_ptr<llvm::msf::MappedBlockStream> PublicSymStream;

  mutable std::map<va_t, FunctionSym> Functions;
  mutable std::map<va_t, DataObjectSym> DataObjects;
  mutable std::set<va_t> AbsentPublics;
  mutable std::map<va_t, std::map<int64_t, VariableSym>> Locals;
  mutable std::map<va_t, std::map<int64_t, VariableSym>> StackPointerLocals;
  mutable std::map<va_t, std::set<int64_t>> AmbiguousLocalOffsets;
  mutable std::map<va_t, std::set<int64_t>> AmbiguousStackPointerOffsets;
  mutable std::set<uint32_t> LoadedModules;
  mutable std::set<va_t> LoadedLocalVAs;
  /// After a complete module walk that did not find the requested VA, every
  /// procedure start in that module is known. Later `--func` unwind ActionVAs
  /// in the same CU must not rescan the symbol stream.
  mutable std::set<uint32_t> FullyIndexedLocalModules;
  /// Next unread symbol offset after a requested `ProcSym` closed. A later
  /// miss in the same CU resumes here instead of walking the prefix again.
  mutable std::map<uint32_t, uint32_t> LocalIndexResume;
  mutable std::map<uint32_t, std::set<va_t>> ModuleProcedureVAs;
  /// Stream offset of each seen `ProcSym`. A later request for a prefix
  /// callee seeks here instead of replaying the CU head.
  mutable std::map<uint32_t, std::map<va_t, uint32_t>> ModuleProcedureOffs;
  mutable std::mutex LocalMutex;
  mutable std::mutex PublicMutex;
  mutable std::mutex TypeMutex;
  /// First `getPDBTpiStream` on a large companion is tens of milliseconds.
  /// Run it after publics so `--func` CFG / Med / High overlap the open.
  mutable std::mutex TypePrefetchMutex;
  mutable std::thread TypePrefetch;

  void startTypePrefetch() {
    TypePrefetch = std::thread([this] { prefetchTypeStreams(); });
  }
  void joinTypePrefetch() const {
    std::lock_guard<std::mutex> Guard(TypePrefetchMutex);
    if (TypePrefetch.joinable())
      TypePrefetch.join();
  }
  void prefetchTypeStreams() const {
    llvm::pdb::TpiStream *TpiS = nullptr;
    llvm::pdb::TpiStream *IpiS = nullptr;
    if (Native) {
      llvm::pdb::PDBFile &PDB = Native->getPDBFile();
      if (PDB.hasPDBTpiStream()) {
        if (auto StreamOr = PDB.getPDBTpiStream())
          TpiS = &*StreamOr;
        else
          llvm::consumeError(StreamOr.takeError());
      }
      if (PDB.hasPDBIpiStream()) {
        if (auto StreamOr = PDB.getPDBIpiStream())
          IpiS = &*StreamOr;
        else
          llvm::consumeError(StreamOr.takeError());
      }
    }
    std::lock_guard<std::mutex> Guard(TypeMutex);
    if (!Tpi.Indexed) {
      Tpi.Stream = TpiS;
      Tpi.Valid = TpiS != nullptr;
      Tpi.Indexed = true;
    }
    if (!Ipi.Indexed) {
      Ipi.Stream = IpiS;
      Ipi.Valid = IpiS != nullptr;
      Ipi.Indexed = true;
    }
  }
  ~Impl() { joinTypePrefetch(); }

  struct TypeGraph {
    bool Indexed = false;
    bool Valid = false;
    llvm::pdb::TpiStream *Stream = nullptr;
    std::map<uint32_t, TypeRef> Cache;
  };
  mutable TypeGraph Tpi;
  mutable TypeGraph Ipi;
  /// Named TPI records remember their field list so pointer pointees can
  /// stay name-only until \ref completeType / a Fields walk.
  struct RecordMeta {
    llvm::codeview::TypeIndex FieldList;
    bool UseIpi = false;
    bool Attached = false;
  };
  mutable std::map<const NdType *, RecordMeta> RecordFields;
  uint16_t PtrSize = 8;

  enum class TpiWalk { Named, Fields };

  bool ImageIdentityAuthenticated = false;
  bool FunctionSignaturesAuthenticated = false;
  bool ObjectExtentsAuthenticated = false;
  bool Loaded = false;

  va_t resolveVA(uint16_t Seg, uint32_t Off) const {
    if (Seg == 0 || Seg > Sections.size())
      return 0;
    const Section &Owner = Sections[Seg - 1];
    if (Off >= Owner.Size || Off > InvalidVA - Owner.VA)
      return 0;
    return Owner.VA + Off;
  }

  std::optional<uint16_t> moduleForVA(va_t Addr) const {
    if (Addr == 0 || Contribs.empty())
      return std::nullopt;
    const auto It = std::upper_bound(
        Contribs.begin(), Contribs.end(), Addr,
        [](va_t Value, const ModuleRange &Range) { return Value < Range.Start; });
    auto Cur = It;
    while (Cur != Contribs.begin()) {
      --Cur;
      if (Addr >= Cur->Start && Addr < Cur->End)
        return Cur->Module;
      if (Cur->End <= Addr)
        break;
    }
    return std::nullopt;
  }

  bool hasLazyPublics() const {
    return PublicSymStream && !PublicAddrBytes.empty() &&
           PublicAddrBytes.size() % sizeof(uint32_t) == 0;
  }

  uint32_t publicCount() const {
    return static_cast<uint32_t>(PublicAddrBytes.size() / sizeof(uint32_t));
  }

  llvm::Error readPublic(uint32_t Index,
                         pdb_loader_detail::ParsedPublicSym32 &Out) const {
    if (!PublicSymStream || Index >= publicCount())
      return pdbLoadError("publics index is out of range");
    const uint32_t RecordOff = llvm::support::endian::read32le(
        PublicAddrBytes.data() + Index * sizeof(uint32_t));
    return parsePublicSym32FromStream(llvm::BinaryStreamRef(*PublicSymStream),
                                      RecordOff, Out);
  }

  std::optional<std::pair<uint16_t, uint32_t>> sectOffForVA(va_t Addr) const {
    for (size_t I = 0; I < Sections.size(); ++I) {
      const Section &Owner = Sections[I];
      if (Addr < Owner.VA || Addr - Owner.VA >= Owner.Size)
        continue;
      return std::make_pair(static_cast<uint16_t>(I + 1),
                            static_cast<uint32_t>(Addr - Owner.VA));
    }
    return std::nullopt;
  }

  /// Publish the unique public names at \p Addr.  A conflicting pair of
  /// names is not a semantic identity source, matching the full-scan
  /// FunctionNameRegistry policy.
  llvm::Error ingestPublicAt(va_t Addr, bool Strict) const {
    std::lock_guard<std::mutex> Guard(PublicMutex);
    if (Addr == 0 || !hasLazyPublics())
      return llvm::Error::success();
    if (Functions.count(Addr) || DataObjects.count(Addr) ||
        AbsentPublics.count(Addr))
      return llvm::Error::success();

    const auto Key = sectOffForVA(Addr);
    if (!Key) {
      AbsentPublics.insert(Addr);
      return llvm::Error::success();
    }

    const uint32_t Count = publicCount();
    uint32_t Lo = 0;
    uint32_t Hi = Count;
    while (Lo < Hi) {
      const uint32_t Mid = Lo + (Hi - Lo) / 2;
      pdb_loader_detail::ParsedPublicSym32 Pub;
      if (auto Err = readPublic(Mid, Pub)) {
        if (Strict)
          return Err;
        llvm::consumeError(std::move(Err));
        Hi = Mid;
        continue;
      }
      if (std::tie(Pub.Segment, Pub.Offset) < std::tie(Key->first, Key->second))
        Lo = Mid + 1;
      else
        Hi = Mid;
    }

    pdb_loader_detail::FunctionNameRegistry FunctionNames;
    pdb_loader_detail::FunctionNameRegistry DataNames;
    bool Found = false;
    for (uint32_t I = Lo; I < Count; ++I) {
      pdb_loader_detail::ParsedPublicSym32 Pub;
      if (auto Err = readPublic(I, Pub)) {
        if (Strict)
          return Err;
        llvm::consumeError(std::move(Err));
        break;
      }
      if (std::tie(Pub.Segment, Pub.Offset) !=
          std::tie(Key->first, Key->second))
        break;
      Found = true;
      if (Pub.IsFunction)
        FunctionNames.observe(Addr, Pub.Name);
      else
        DataNames.observe(Addr, Pub.Name);
    }

    if (const std::optional<std::string> Name = FunctionNames.name(Addr)) {
      FunctionSym &FS = Functions[Addr];
      FS.Addr = Addr;
      FS.Name = *Name;
    }
    if (const std::optional<std::string> Name = DataNames.name(Addr)) {
      DataObjectSym &Obj = DataObjects[Addr];
      Obj.Name = *Name;
      Obj.Addr = Addr;
      Obj.Size = 0;
      Obj.IsBuffer = false;
    }
    if (!Functions.count(Addr) && !DataObjects.count(Addr))
      AbsentPublics.insert(Addr);
    (void)Found;
    return llvm::Error::success();
  }

  void ensureGraph(TypeGraph &Graph, bool UseIpi) const;
  void noteRecordFields(const TypeRef &Named,
                        llvm::codeview::TypeIndex FieldList, bool UseIpi) const;
  void ensureRecordFields(const TypeRef &Ty) const;
  void materializeVariableType(VariableSym &VS) const;
  TypeRef resolveIndex(TypeGraph &Graph, llvm::codeview::TypeIndex TI,
                       bool UseIpi, int Depth,
                       TpiWalk Walk = TpiWalk::Fields) const;
  TypeRef resolveRecord(llvm::codeview::CVType CV, bool UseIpi, int Depth,
                        llvm::codeview::TypeIndex Self, TpiWalk Walk) const;
  TypeRef resolveTpi(llvm::codeview::TypeIndex TI, int Depth = 0,
                     TpiWalk Walk = TpiWalk::Fields) const;
  TypeRef resolveIpi(llvm::codeview::TypeIndex TI, int Depth = 0,
                     TpiWalk Walk = TpiWalk::Fields) const;
  void attachDisplayFields(const TypeRef &Record,
                           llvm::codeview::TypeIndex FieldList, bool UseIpi,
                           int Depth) const;
  TypeRef functionReturnFromType(llvm::codeview::TypeIndex TI,
                                 bool FromIpi) const;
  std::vector<TypeRef> functionParamTypesFromType(llvm::codeview::TypeIndex TI,
                                                  bool FromIpi) const;
  DebugCallConv functionCallConvFromType(llvm::codeview::TypeIndex TI,
                                         bool FromIpi) const;
  std::optional<llvm::codeview::CVType>
  loadType(TypeGraph &Graph, bool UseIpi, llvm::codeview::TypeIndex TI) const;
  bool visitProcedureType(
      llvm::codeview::TypeIndex TI, bool FromIpi,
      const llvm::function_ref<void(llvm::codeview::TypeIndex,
                                    llvm::codeview::CallingConvention)> &Fn)
      const;
};

void PDBDebugContext::Impl::ensureGraph(TypeGraph &Graph, bool UseIpi) const {
  if (Graph.Indexed)
    return;
  Graph.Indexed = true;
  if (!Native)
    return;
  llvm::pdb::PDBFile &PDB = Native->getPDBFile();
  if (UseIpi ? !PDB.hasPDBIpiStream() : !PDB.hasPDBTpiStream())
    return;
  auto StreamOr = UseIpi ? PDB.getPDBIpiStream() : PDB.getPDBTpiStream();
  if (!StreamOr) {
    llvm::consumeError(StreamOr.takeError());
    return;
  }
  Graph.Stream = &*StreamOr;
  Graph.Valid = Graph.Stream != nullptr;
}

void PDBDebugContext::Impl::noteRecordFields(
    const TypeRef &Named, llvm::codeview::TypeIndex FieldList,
    bool UseIpi) const {
  if (!Named || FieldList.isNoneType() || FieldList.isSimple())
    return;
  std::lock_guard<std::mutex> Guard(TypeMutex);
  RecordFields.emplace(Named.get(), RecordMeta{FieldList, UseIpi, false});
}

void PDBDebugContext::Impl::ensureRecordFields(const TypeRef &Ty) const {
  TypeRef Cur = Ty;
  while (Cur && Cur->Kind == NdTypeKind::Ptr)
    Cur = Cur->Pointee;
  if (!Cur || Cur->Kind != NdTypeKind::Struct)
    return;
  RecordMeta Meta;
  {
    std::lock_guard<std::mutex> Guard(TypeMutex);
    const auto It = RecordFields.find(Cur.get());
    if (It == RecordFields.end() || It->second.Attached)
      return;
    if (!Cur->FieldDisplayNames.empty()) {
      It->second.Attached = true;
      return;
    }
    It->second.Attached = true;
    Meta = It->second;
  }
  attachDisplayFields(Cur, Meta.FieldList, Meta.UseIpi, 0);
}

void PDBDebugContext::Impl::materializeVariableType(VariableSym &VS) const {
  if (VS.Type || VS.TypeId == 0)
    return;
  VS.Type = resolveTpi(llvm::codeview::TypeIndex(VS.TypeId));
}

TypeRef PDBDebugContext::Impl::resolveIndex(TypeGraph &Graph,
                                            llvm::codeview::TypeIndex TI,
                                            bool UseIpi, int Depth,
                                            TpiWalk Walk) const {
  joinTypePrefetch();
  if (Depth > 32 || TI.isNoneType())
    return {};
  if (TI.isSimple())
    return primitiveType(TI, PtrSize);

  llvm::codeview::CVType CV;
  TypeRef Cached;
  bool HaveCached = false;
  {
    std::lock_guard<std::mutex> Guard(TypeMutex);
    ensureGraph(Graph, UseIpi);
    if (!Graph.Valid || !Graph.Stream)
      return {};
    const uint32_t Index = TI.getIndex();
    if (const auto It = Graph.Cache.find(Index); It != Graph.Cache.end()) {
      Cached = It->second;
      HaveCached = true;
    } else {
      auto TypeOr = Graph.Stream->tryGetType(TI);
      if (!TypeOr)
        return {};
      Graph.Cache.emplace(Index, TypeRef{});
      CV = *TypeOr;
    }
  }
  if (HaveCached) {
    if (Walk == TpiWalk::Fields && Cached)
      ensureRecordFields(Cached);
    return Cached;
  }

  TypeRef Result = resolveRecord(CV, UseIpi, Depth, TI, Walk);
  {
    std::lock_guard<std::mutex> Guard(TypeMutex);
    Graph.Cache[TI.getIndex()] = Result;
  }
  return Result;
}

TypeRef PDBDebugContext::Impl::resolveRecord(llvm::codeview::CVType CV,
                                             bool UseIpi, int Depth,
                                             llvm::codeview::TypeIndex Self,
                                             TpiWalk Walk) const {
  using LK = llvm::codeview::TypeLeafKind;
  auto Recurse = [&](llvm::codeview::TypeIndex Next) -> TypeRef {
    return resolveTpi(Next, Depth + 1, Walk);
  };
  auto RecurseNamed = [&](llvm::codeview::TypeIndex Next) -> TypeRef {
    return resolveTpi(Next, Depth + 1, TpiWalk::Named);
  };

  switch (CV.kind()) {
  case LK::LF_MODIFIER: {
    llvm::codeview::ModifierRecord Rec;
    if (auto Err =
            llvm::codeview::TypeDeserializer::deserializeAs(CV, Rec)) {
      llvm::consumeError(std::move(Err));
      return {};
    }
    return Recurse(Rec.ModifiedType);
  }
  case LK::LF_POINTER: {
    llvm::codeview::PointerRecord Rec;
    if (auto Err =
            llvm::codeview::TypeDeserializer::deserializeAs(CV, Rec)) {
      llvm::consumeError(std::move(Err));
      return {};
    }
    TypeRef Pointee = RecurseNamed(Rec.ReferentType);
    if (!Pointee)
      Pointee = NdType::makeVoid();
    auto Ptr = NdType::makePtr(Pointee);
    if (const uint8_t Size = Rec.getSize())
      Ptr->Size = Size;
    else
      Ptr->Size = PtrSize;
    return Ptr;
  }
  case LK::LF_ARRAY: {
    llvm::codeview::ArrayRecord Rec;
    if (auto Err =
            llvm::codeview::TypeDeserializer::deserializeAs(CV, Rec)) {
      llvm::consumeError(std::move(Err));
      return {};
    }
    TypeRef Elem = Recurse(Rec.ElementType);
    if (!Elem)
      return {};
    uint32_t Count = 0;
    if (Elem->Size > 0 && Rec.Size > 0 && Rec.Size % Elem->Size == 0 &&
        Rec.Size / Elem->Size <= std::numeric_limits<uint32_t>::max())
      Count = static_cast<uint32_t>(Rec.Size / Elem->Size);
    return NdType::makeArray(Elem, Count, boundedTypeSize(Rec.Size));
  }
  case LK::LF_CLASS:
  case LK::LF_STRUCTURE:
  case LK::LF_INTERFACE:
  case LK::LF_UNION: {
    llvm::codeview::ClassRecord Rec;
    if (CV.kind() == LK::LF_UNION) {
      llvm::codeview::UnionRecord Union;
      if (auto Err =
              llvm::codeview::TypeDeserializer::deserializeAs(CV, Union)) {
        llvm::consumeError(std::move(Err));
        return {};
      }
      const std::string Name = udtSpelling(Union.getName(), Union.getUniqueName());
      if (Name.empty())
        return {};
      if (Union.isForwardRef() && !Self.isNoneType()) {
        std::optional<llvm::codeview::TypeIndex> Full;
        {
          std::lock_guard<std::mutex> Guard(TypeMutex);
          TypeGraph &Graph = UseIpi ? Ipi : Tpi;
          ensureGraph(Graph, UseIpi);
          if (Graph.Valid && Graph.Stream) {
            auto FullOr = Graph.Stream->findFullDeclForForwardRef(Self);
            if (FullOr)
              Full = *FullOr;
            else
              llvm::consumeError(FullOr.takeError());
          }
        }
        if (Full && *Full != Self)
          return Recurse(*Full);
      }
      TypeRef Named =
          NdType::makeNamedRecord(Name, boundedTypeSize(Union.getSize()));
      noteRecordFields(Named, Union.getFieldList(), UseIpi);
      if (!Union.isForwardRef() && Walk == TpiWalk::Fields) {
        attachDisplayFields(Named, Union.getFieldList(), UseIpi, Depth);
        ensureRecordFields(Named);
      }
      return Named;
    }
    if (auto Err =
            llvm::codeview::TypeDeserializer::deserializeAs(CV, Rec)) {
      llvm::consumeError(std::move(Err));
      return {};
    }
    const std::string Name = udtSpelling(Rec.getName(), Rec.getUniqueName());
    if (Name.empty())
      return {};
    if (Rec.isForwardRef() && !Self.isNoneType()) {
      std::optional<llvm::codeview::TypeIndex> Full;
      {
        std::lock_guard<std::mutex> Guard(TypeMutex);
        TypeGraph &Graph = UseIpi ? Ipi : Tpi;
        ensureGraph(Graph, UseIpi);
        if (Graph.Valid && Graph.Stream) {
          auto FullOr = Graph.Stream->findFullDeclForForwardRef(Self);
          if (FullOr)
            Full = *FullOr;
          else
            llvm::consumeError(FullOr.takeError());
        }
      }
      if (Full && *Full != Self)
        return Recurse(*Full);
    }
    TypeRef Named =
        NdType::makeNamedRecord(Name, boundedTypeSize(Rec.getSize()));
    noteRecordFields(Named, Rec.getFieldList(), UseIpi);
    if (!Rec.isForwardRef() && Walk == TpiWalk::Fields) {
      attachDisplayFields(Named, Rec.getFieldList(), UseIpi, Depth);
      ensureRecordFields(Named);
    }
    return Named;
  }
  case LK::LF_ENUM: {
    llvm::codeview::EnumRecord Rec;
    if (auto Err =
            llvm::codeview::TypeDeserializer::deserializeAs(CV, Rec)) {
      llvm::consumeError(std::move(Err));
      return {};
    }
    const std::string Name = udtSpelling(Rec.getName(), Rec.getUniqueName());
    if (Name.empty())
      return {};
    if (Rec.isForwardRef() && !Self.isNoneType()) {
      std::optional<llvm::codeview::TypeIndex> Full;
      {
        std::lock_guard<std::mutex> Guard(TypeMutex);
        TypeGraph &Graph = UseIpi ? Ipi : Tpi;
        ensureGraph(Graph, UseIpi);
        if (Graph.Valid && Graph.Stream) {
          auto FullOr = Graph.Stream->findFullDeclForForwardRef(Self);
          if (FullOr)
            Full = *FullOr;
          else
            llvm::consumeError(FullOr.takeError());
        }
      }
      if (Full && *Full != Self)
        return Recurse(*Full);
    }
    TypeRef Named = NdType::makeNamedRecord(Name, 0, true);
    noteRecordFields(Named, Rec.getFieldList(), UseIpi);
    if (!Rec.isForwardRef() && Walk == TpiWalk::Fields) {
      attachDisplayFields(Named, Rec.getFieldList(), UseIpi, Depth);
      ensureRecordFields(Named);
    }
    return Named;
  }
  case LK::LF_PROCEDURE: {
    llvm::codeview::ProcedureRecord Rec;
    if (auto Err =
            llvm::codeview::TypeDeserializer::deserializeAs(CV, Rec)) {
      llvm::consumeError(std::move(Err));
      return {};
    }
    TypeRef Return = Recurse(Rec.ReturnType);
    if (!Return)
      Return = NdType::makeVoid();
    std::vector<TypeRef> Params;
    llvm::codeview::ArgListRecord Args;
    llvm::codeview::CVType ArgCV;
    bool HaveArgs = false;
    {
      std::lock_guard<std::mutex> Guard(TypeMutex);
      TypeGraph &Graph = UseIpi ? Ipi : Tpi;
      ensureGraph(Graph, UseIpi);
      if (Graph.Valid && Graph.Stream && !Rec.ArgumentList.isSimple()) {
        if (auto TypeOr = Graph.Stream->tryGetType(Rec.ArgumentList)) {
          ArgCV = *TypeOr;
          HaveArgs = true;
        }
      }
    }
    if (HaveArgs) {
      if (auto Err =
              llvm::codeview::TypeDeserializer::deserializeAs(ArgCV, Args)) {
        llvm::consumeError(std::move(Err));
      } else {
        const uint32_t Limit = std::min<uint32_t>(
            Args.ArgIndices.size(), Rec.ParameterCount ? Rec.ParameterCount
                                                       : Args.ArgIndices.size());
        for (uint32_t I = 0; I < Limit; ++I) {
          if (Args.ArgIndices[I].isNoneType())
            continue;
          TypeRef Arg = Recurse(Args.ArgIndices[I]);
          Params.push_back(Arg ? Arg : NdType::makeVoid());
        }
      }
    }
    return NdType::makeFunc(Return, std::move(Params));
  }
  case LK::LF_MFUNCTION: {
    llvm::codeview::MemberFunctionRecord Rec;
    if (auto Err =
            llvm::codeview::TypeDeserializer::deserializeAs(CV, Rec)) {
      llvm::consumeError(std::move(Err));
      return {};
    }
    TypeRef Return = Recurse(Rec.ReturnType);
    if (!Return)
      Return = NdType::makeVoid();
    std::vector<TypeRef> Params;
    llvm::codeview::ArgListRecord Args;
    llvm::codeview::CVType ArgCV;
    bool HaveArgs = false;
    {
      std::lock_guard<std::mutex> Guard(TypeMutex);
      TypeGraph &Graph = UseIpi ? Ipi : Tpi;
      ensureGraph(Graph, UseIpi);
      if (Graph.Valid && Graph.Stream && !Rec.ArgumentList.isSimple()) {
        if (auto TypeOr = Graph.Stream->tryGetType(Rec.ArgumentList)) {
          ArgCV = *TypeOr;
          HaveArgs = true;
        }
      }
    }
    if (HaveArgs) {
      if (auto Err =
              llvm::codeview::TypeDeserializer::deserializeAs(ArgCV, Args)) {
        llvm::consumeError(std::move(Err));
      } else {
        const uint32_t Limit = std::min<uint32_t>(
            Args.ArgIndices.size(), Rec.ParameterCount ? Rec.ParameterCount
                                                       : Args.ArgIndices.size());
        for (uint32_t I = 0; I < Limit; ++I) {
          if (Args.ArgIndices[I].isNoneType())
            continue;
          TypeRef Arg = Recurse(Args.ArgIndices[I]);
          Params.push_back(Arg ? Arg : NdType::makeVoid());
        }
      }
    }
    return NdType::makeFunc(Return, std::move(Params));
  }
  case LK::LF_BITFIELD: {
    llvm::codeview::BitFieldRecord Rec;
    if (auto Err =
            llvm::codeview::TypeDeserializer::deserializeAs(CV, Rec)) {
      llvm::consumeError(std::move(Err));
      return {};
    }
    return Recurse(Rec.Type);
  }
  case LK::LF_FUNC_ID: {
    llvm::codeview::FuncIdRecord Rec;
    if (auto Err =
            llvm::codeview::TypeDeserializer::deserializeAs(CV, Rec)) {
      llvm::consumeError(std::move(Err));
      return {};
    }
    return resolveTpi(Rec.FunctionType, Depth + 1);
  }
  case LK::LF_MFUNC_ID: {
    llvm::codeview::MemberFuncIdRecord Rec;
    if (auto Err =
            llvm::codeview::TypeDeserializer::deserializeAs(CV, Rec)) {
      llvm::consumeError(std::move(Err));
      return {};
    }
    return resolveTpi(Rec.FunctionType, Depth + 1);
  }
  default:
    return {};
  }
}

TypeRef PDBDebugContext::Impl::resolveTpi(llvm::codeview::TypeIndex TI,
                                          int Depth, TpiWalk Walk) const {
  return resolveIndex(Tpi, TI, /*UseIpi=*/false, Depth, Walk);
}

namespace {

bool isDisplayFieldName(llvm::StringRef Name) {
  if (Name.empty() ||
      (!std::isalpha(static_cast<unsigned char>(Name.front())) &&
       Name.front() != '_'))
    return false;
  return llvm::all_of(Name, [](char Ch) {
    return std::isalnum(static_cast<unsigned char>(Ch)) || Ch == '_';
  });
}

} // namespace

void PDBDebugContext::Impl::attachDisplayFields(
    const TypeRef &Record, llvm::codeview::TypeIndex FieldList, bool UseIpi,
    int Depth) const {
  if (!Record || Depth > 32 || FieldList.isNoneType() || FieldList.isSimple())
    return;
  // Do not bail when names are already present: LF_FIELDLIST
  // continuations append onto the same record. A nonempty first chunk
  // used to drop later enumerators (string-id enums past ~1.8k).
  if (Record->FieldDisplayNames.size() >= limits::kMaxTpiDisplayFields)
    return;

  auto CV = loadType(UseIpi ? Ipi : Tpi, UseIpi, FieldList);
  if (!CV || CV->kind() != llvm::codeview::TypeLeafKind::LF_FIELDLIST)
    return;
  llvm::codeview::FieldListRecord List;
  if (auto Err = llvm::codeview::TypeDeserializer::deserializeAs(*CV, List)) {
    llvm::consumeError(std::move(Err));
    return;
  }

  struct Collector : llvm::codeview::TypeVisitorCallbacks {
    const Impl *Owner = nullptr;
    TypeRef Record;
    bool UseIpi = false;
    int Depth = 0;
    bool Failed = false;

    void add(uint64_t Off, llvm::StringRef Name, TypeRef Ty = {}) {
      if (Failed || Off > std::numeric_limits<uint16_t>::max() ||
          !isDisplayFieldName(Name) ||
          Record->FieldDisplayNames.size() >= limits::kMaxTpiDisplayFields)
        return;
      const uint16_t ByteOff = static_cast<uint16_t>(Off);
      const std::string Id = Name.str();
      for (size_t I = 0; I < Record->FieldDisplayOffsets.size(); ++I) {
        if (Record->FieldDisplayOffsets[I] != ByteOff)
          continue;
        if (Record->FieldDisplayNames[I] == Id)
          return;
        // Same offset, different name: keep both. A union overlay
        // (`_m_nRef` / `_m_pNext`) is resolved later by access width.
        // Clearing used to drop every name at the slot.
      }
      Record->FieldDisplayOffsets.push_back(ByteOff);
      Record->FieldDisplayNames.push_back(Id);
      Record->FieldDisplayTypes.push_back(std::move(Ty));
    }

    llvm::Error visitKnownMember(llvm::codeview::CVMemberRecord &,
                                 llvm::codeview::DataMemberRecord &Field) override {
      add(Field.getFieldOffset(), Field.getName(),
          Owner->resolveTpi(Field.getType(), Depth + 1));
      return llvm::Error::success();
    }

    llvm::Error visitKnownMember(llvm::codeview::CVMemberRecord &,
                                 llvm::codeview::EnumeratorRecord &Field) override {
      const llvm::APSInt Value = Field.getValue();
      if (Value.isNegative() || Value.getActiveBits() > 16)
        return llvm::Error::success();
      add(Value.getZExtValue(), Field.getName());
      return llvm::Error::success();
    }

    llvm::Error visitKnownMember(llvm::codeview::CVMemberRecord &,
                                 llvm::codeview::BaseClassRecord &Base) override {
      if (Failed)
        return llvm::Error::success();
      TypeRef BaseTy = Owner->resolveTpi(Base.getBaseType(), Depth + 1);
      if (!BaseTy)
        return llvm::Error::success();
      const uint64_t BaseOff = Base.getBaseOffset();
      for (size_t I = 0; I < BaseTy->FieldDisplayOffsets.size(); ++I) {
        if (BaseTy->FieldDisplayNames[I].empty())
          continue;
        if (BaseOff > static_cast<uint64_t>(std::numeric_limits<uint16_t>::max()) -
                          BaseTy->FieldDisplayOffsets[I]) {
          Failed = true;
          return llvm::Error::success();
        }
        TypeRef InnerTy;
        if (BaseTy->FieldDisplayTypes.size() ==
            BaseTy->FieldDisplayOffsets.size())
          InnerTy = BaseTy->FieldDisplayTypes[I];
        add(BaseOff + BaseTy->FieldDisplayOffsets[I],
            BaseTy->FieldDisplayNames[I], InnerTy);
      }
      return llvm::Error::success();
    }

    llvm::Error
    visitKnownMember(llvm::codeview::CVMemberRecord &,
                     llvm::codeview::ListContinuationRecord &Cont) override {
      if (!Failed)
        Owner->attachDisplayFields(Record, Cont.getContinuationIndex(), UseIpi,
                                   Depth + 1);
      return llvm::Error::success();
    }
  };

  Collector Visit;
  Visit.Owner = this;
  Visit.Record = Record;
  Visit.UseIpi = UseIpi;
  Visit.Depth = Depth;
  if (auto Err = llvm::codeview::visitMemberRecordStream(List.Data, Visit)) {
    llvm::consumeError(std::move(Err));
    Record->FieldDisplayNames.clear();
    Record->FieldDisplayOffsets.clear();
    Record->FieldDisplayTypes.clear();
    return;
  }
  if (Visit.Failed) {
    Record->FieldDisplayNames.clear();
    Record->FieldDisplayOffsets.clear();
    Record->FieldDisplayTypes.clear();
  }
}

TypeRef PDBDebugContext::Impl::resolveIpi(llvm::codeview::TypeIndex TI,
                                          int Depth, TpiWalk Walk) const {
  return resolveIndex(Ipi, TI, /*UseIpi=*/true, Depth, Walk);
}

DebugCallConv debugCallConv(llvm::codeview::CallingConvention Call) {
  using CC = llvm::codeview::CallingConvention;
  switch (Call) {
  case CC::NearC:
  case CC::FarC:
    return DebugCallConv::Cdecl;
  case CC::NearStdCall:
  case CC::FarStdCall:
    return DebugCallConv::Stdcall;
  case CC::ThisCall:
    return DebugCallConv::Thiscall;
  case CC::NearFast:
  case CC::FarFast:
    return DebugCallConv::Fastcall;
  default:
    return DebugCallConv::Unknown;
  }
}

std::optional<llvm::codeview::CVType>
PDBDebugContext::Impl::loadType(TypeGraph &Graph, bool UseIpi,
                                llvm::codeview::TypeIndex TI) const {
  joinTypePrefetch();
  std::lock_guard<std::mutex> Guard(TypeMutex);
  ensureGraph(Graph, UseIpi);
  if (!Graph.Valid || !Graph.Stream || TI.isNoneType() || TI.isSimple())
    return std::nullopt;
  auto TypeOr = Graph.Stream->tryGetType(TI);
  if (!TypeOr)
    return std::nullopt;
  return *TypeOr;
}

bool PDBDebugContext::Impl::visitProcedureType(
    llvm::codeview::TypeIndex TI, bool FromIpi,
    const llvm::function_ref<void(llvm::codeview::TypeIndex ReturnTI,
                                  llvm::codeview::CallingConvention)>
        &Fn) const {
  llvm::codeview::TypeIndex Cur = TI;
  bool UseIpi = FromIpi;
  for (int Depth = 0; Depth < 8; ++Depth) {
    auto CV = loadType(UseIpi ? Ipi : Tpi, UseIpi, Cur);
    if (!CV)
      return false;
    using LK = llvm::codeview::TypeLeafKind;
    if (CV->kind() == LK::LF_FUNC_ID) {
      llvm::codeview::FuncIdRecord Rec;
      if (auto Err = llvm::codeview::TypeDeserializer::deserializeAs(*CV, Rec)) {
        llvm::consumeError(std::move(Err));
        return false;
      }
      Cur = Rec.FunctionType;
      UseIpi = false;
      continue;
    }
    if (CV->kind() == LK::LF_MFUNC_ID) {
      llvm::codeview::MemberFuncIdRecord Rec;
      if (auto Err = llvm::codeview::TypeDeserializer::deserializeAs(*CV, Rec)) {
        llvm::consumeError(std::move(Err));
        return false;
      }
      Cur = Rec.FunctionType;
      UseIpi = false;
      continue;
    }
    if (CV->kind() == LK::LF_PROCEDURE) {
      llvm::codeview::ProcedureRecord Rec;
      if (auto Err = llvm::codeview::TypeDeserializer::deserializeAs(*CV, Rec)) {
        llvm::consumeError(std::move(Err));
        return false;
      }
      Fn(Rec.ReturnType, Rec.CallConv);
      return true;
    }
    if (CV->kind() == LK::LF_MFUNCTION) {
      llvm::codeview::MemberFunctionRecord Rec;
      if (auto Err = llvm::codeview::TypeDeserializer::deserializeAs(*CV, Rec)) {
        llvm::consumeError(std::move(Err));
        return false;
      }
      Fn(Rec.ReturnType, Rec.CallConv);
      return true;
    }
    return false;
  }
  return false;
}

TypeRef PDBDebugContext::Impl::functionReturnFromType(
    llvm::codeview::TypeIndex TI, bool FromIpi) const {
  TypeRef Return;
  if (visitProcedureType(TI, FromIpi,
                         [&](llvm::codeview::TypeIndex ReturnTI,
                             llvm::codeview::CallingConvention) {
                           Return = resolveTpi(ReturnTI);
                         }))
    return Return;
  const TypeRef Ty = FromIpi ? resolveIpi(TI) : resolveTpi(TI);
  if (Ty && Ty->Kind == NdTypeKind::Func)
    return Ty->RetType;
  return {};
}

std::vector<TypeRef> PDBDebugContext::Impl::functionParamTypesFromType(
    llvm::codeview::TypeIndex TI, bool FromIpi) const {
  const TypeRef Ty = FromIpi ? resolveIpi(TI) : resolveTpi(TI);
  if (Ty && Ty->Kind == NdTypeKind::Func)
    return Ty->ParamTypes;
  return {};
}

DebugCallConv PDBDebugContext::Impl::functionCallConvFromType(
    llvm::codeview::TypeIndex TI, bool FromIpi) const {
  DebugCallConv CC = DebugCallConv::Unknown;
  visitProcedureType(TI, FromIpi,
                     [&](llvm::codeview::TypeIndex,
                         llvm::codeview::CallingConvention RecCC) {
                       CC = debugCallConv(RecCC);
                     });
  return CC;
}

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
  Ctx->PImpl->Native = Native;
  Ctx->PImpl->CPU = cpuTypeFromMachine(DBI.getMachineType());
  Ctx->PImpl->PtrSize =
      (Image.Arch == Arch::X64 || Image.Arch == Arch::AArch64) ? 8 : 4;
  Ctx->PImpl->Sections = Image.Sections;
  {
    class Visitor : public llvm::pdb::ISectionContribVisitor {
    public:
      explicit Visitor(Impl &Owner) : Owner(Owner) {}
      void visit(const llvm::pdb::SectionContrib &C) override {
        if (C.Size <= 0)
          return;
        const va_t VA =
            Owner.resolveVA(C.ISect, static_cast<uint32_t>(C.Off));
        if (VA == 0)
          return;
        const uint64_t Size = static_cast<uint64_t>(C.Size);
        if (Size > InvalidVA - VA)
          return;
        Owner.Contribs.push_back({VA, VA + Size, C.Imod});
      }
      void visit(const llvm::pdb::SectionContrib2 &C) override {
        visit(C.Base);
      }
      Impl &Owner;
    };
    Visitor ContribVisitor(*Ctx->PImpl);
    DBI.visitSectionContributions(ContribVisitor);
    std::sort(Ctx->PImpl->Contribs.begin(), Ctx->PImpl->Contribs.end(),
              [](const Impl::ModuleRange &A, const Impl::ModuleRange &B) {
                return A.Start < B.Start;
              });
  }
  // Phase B walks TPI/IPI records on first type use.  Load still does not
  // enter LLVM's SymbolCache lazy type graph.

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
  const bool RestrictPublics = !Image.LoadOnlyFunctionEntries.empty();
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
      // `--func` keeps the stream and address map instead: a 328MB companion
      // must not copy every S_PUB32 to name one entry.
      std::unique_ptr<llvm::msf::MappedBlockStream> SymStream =
          PDB.createIndexedStream(static_cast<uint16_t>(SymIndex));
      if (!SymStream)
        return pdbLoadError("cannot read backing symbol stream");
      if (RestrictPublics) {
        Ctx->PImpl->PublicAddrBytes.assign(AddrBytes.begin(), AddrBytes.end());
        Ctx->PImpl->PublicSymStream = std::move(SymStream);
        const uint32_t Wanted = static_cast<uint32_t>(
            Image.LoadOnlyFunctionEntries.size());
        Progress.report("debug", 0, Wanted, "pdb publics");
        uint32_t Done = 0;
        for (va_t VA : Image.LoadOnlyFunctionEntries) {
          if (auto Err = Ctx->PImpl->ingestPublicAt(VA, /*Strict=*/true))
            return Err;
          ++Done;
          Progress.report("debug", Done, Wanted, "pdb publics");
        }
      } else {
        Progress.report("debug", 0, 1, "pdb symbol stream");
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
  }

  // Publics already named the linked functions.  Walking every module
  // symbol stream is what made a large PDB stall load for tens of seconds
  // with no UI; do that only when Publics did not contribute any function.
  // `--func` must not fall through to a full module walk after skipping
  // the publics scan.
  const bool NeedModuleWalk = FunctionAddresses.empty() && !RestrictPublics;
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
  Ctx->PImpl->Loaded =
      !Ctx->PImpl->Functions.empty() || Ctx->PImpl->hasLazyPublics();
  Ctx->PImpl->Session = std::move(Session);
  if (Ctx->PImpl->Loaded && Ctx->PImpl->Native)
    Ctx->PImpl->startTypePrefetch();
  LLVM_DEBUG(llvm::dbgs() << "pdb: loaded " << Ctx->PImpl->Functions.size()
                          << " function symbols from "
                          << PdbPath.filename().string() << "\n");
  return Ctx;
}

void PDBDebugContext::ensureLocalsForAddress(va_t Addr) const {
  if (!PImpl || !PImpl->Native || Addr == 0)
    return;
  PImpl->joinTypePrefetch();

  const std::optional<uint16_t> Module = PImpl->moduleForVA(Addr);
  if (!Module)
    return;

  const uint32_t ModuleIndex = *Module;
  std::lock_guard<std::mutex> Guard(PImpl->LocalMutex);
  if (PImpl->LoadedLocalVAs.count(Addr))
    return;
  if (PImpl->FullyIndexedLocalModules.count(ModuleIndex) &&
      !PImpl->ModuleProcedureVAs[ModuleIndex].count(Addr)) {
    PImpl->LoadedLocalVAs.insert(Addr);
    return;
  }
  PImpl->LoadedModules.insert(ModuleIndex);

  llvm::Expected<llvm::pdb::ModuleDebugStreamRef> ModuleOr =
      PImpl->Native->getModuleDebugStream(ModuleIndex);
  if (!ModuleOr) {
    llvm::consumeError(ModuleOr.takeError());
    return;
  }

  const llvm::codeview::CVSymbolArray &Records = ModuleOr->getSymbolArray();
  bool HadError = false;
  uint32_t StartOff = Records.skew();
  // Seek a ProcSym we already saw. Resume only for a VA the prefix walk
  // has not already seen. A callee whose ProcSym sits before the last
  // requested function must not start after that function or its TPI
  // params stay empty even when its type information exists.
  if (auto Offs = PImpl->ModuleProcedureOffs.find(ModuleIndex);
      Offs != PImpl->ModuleProcedureOffs.end()) {
    if (auto ProcOff = Offs->second.find(Addr); ProcOff != Offs->second.end())
      StartOff = ProcOff->second;
  }
  if (StartOff == Records.skew() &&
      !PImpl->ModuleProcedureVAs[ModuleIndex].count(Addr)) {
    if (auto Resume = PImpl->LocalIndexResume.find(ModuleIndex);
        Resume != PImpl->LocalIndexResume.end())
      StartOff = Resume->second;
  }
  auto It = llvm::codeview::CVSymbolArray::Iterator(
      Records, Records.getExtractor(), StartOff, &HadError);
  const auto End = Records.end();

  struct OpenProc {
    va_t VA = 0;
    uint32_t End = 0;
    uint32_t FrameBytes = 0;
    llvm::codeview::RegisterId LocalFP = llvm::codeview::RegisterId::NONE;
    std::optional<std::string> PendingName;
    llvm::codeview::TypeIndex PendingType;
    llvm::codeview::TypeIndex FunctionType;
    bool FunctionTypeFromIpi = false;
    bool PendingIsParam = false;
    bool PendingPlaced = false;
    bool Wanted = false;
    // Names only during the CU walk. Resolving TPI per S_LOCAL IsParameter
    // was the `--func` LowIR cost of a moderately sized helper (200+
    // records). `place()` already stores TypeId; finish() applies LF_PROCEDURE
    // types, or resolves these indices once if TPI arity is empty.
    std::vector<std::pair<std::string, llvm::codeview::TypeIndex>> ParamIds;
  };
  std::vector<OpenProc> Open;

  auto finish = [&](OpenProc &Proc) {
    if (Proc.VA == 0)
      return;
    auto FuncIt = PImpl->Functions.find(Proc.VA);
    if (FuncIt == PImpl->Functions.end())
      return;
    if (!Proc.FunctionType.isNoneType()) {
      if (!FuncIt->second.ReturnType)
        FuncIt->second.ReturnType = PImpl->functionReturnFromType(
            Proc.FunctionType, Proc.FunctionTypeFromIpi);
      if (FuncIt->second.CallConv == DebugCallConv::Unknown)
        FuncIt->second.CallConv = PImpl->functionCallConvFromType(
            Proc.FunctionType, Proc.FunctionTypeFromIpi);
    }
    // Template / optimized procs often have LF_PROCEDURE arity but no
    // S_LOCAL IsParameter names. TPI still owns display params.
    if (!FuncIt->second.Params.empty())
      return;
    std::vector<std::pair<std::string, TypeRef>> Params;
    for (const auto &Param : Proc.ParamIds)
      Params.emplace_back(Param.first, TypeRef{});
    if (!Proc.FunctionType.isNoneType()) {
      const auto TpiParams = PImpl->functionParamTypesFromType(
          Proc.FunctionType, Proc.FunctionTypeFromIpi);
      if (!TpiParams.empty())
        Params = bindDebugParamsToTpi(std::move(Params), TpiParams);
    }
    size_t Id = 0;
    for (auto &Param : Params) {
      if (Param.second)
        continue;
      while (Id < Proc.ParamIds.size() && Proc.ParamIds[Id].first != Param.first)
        ++Id;
      if (Id < Proc.ParamIds.size())
        Param.second = PImpl->resolveTpi(Proc.ParamIds[Id++].second);
    }
    if (Params.empty())
      return;
    FuncIt->second.Params = std::move(Params);
  };

  auto place = [&](OpenProc &Proc, int64_t Rel,
                   llvm::codeview::RegisterId Register, bool FrameProcRel) {
    if (Proc.VA == 0 || !Proc.PendingName || Proc.PendingPlaced)
      return;
    VariableSym VS;
    VS.Name = *Proc.PendingName;
    VS.TypeId = Proc.PendingType.getIndex();
    VS.IsParam = Proc.PendingIsParam;
    const llvm::codeview::RegisterId Effective =
        FrameProcRel ? Proc.LocalFP : Register;
    int64_t FrameOff = Rel;
    if (isStackPointerRegister(Effective) && Proc.FrameBytes != 0)
      FrameOff = Rel - static_cast<int64_t>(Proc.FrameBytes);
    VS.StackOffset = FrameOff;
    pdb_loader_detail::publishNamedOffset(PImpl->Locals[Proc.VA],
                                          PImpl->AmbiguousLocalOffsets[Proc.VA],
                                          FrameOff, VS);
    if (isStackPointerRegister(Effective)) {
      VS.StackOffset = Rel;
      pdb_loader_detail::publishNamedOffset(
          PImpl->StackPointerLocals[Proc.VA],
          PImpl->AmbiguousStackPointerOffsets[Proc.VA], Rel, VS);
    }
    Proc.PendingPlaced = true;
  };

  bool TargetDone = false;
  for (; It != End; ++It) {
    const uint32_t Off = It.offset();
    while (!Open.empty() && Open.back().End != 0 && Off >= Open.back().End) {
      const bool Done = Open.back().Wanted && Open.back().VA == Addr;
      if (Open.back().Wanted)
        finish(Open.back());
      Open.pop_back();
      if (Done)
        TargetDone = true;
    }
    if (TargetDone) {
      auto &Resume = PImpl->LocalIndexResume[ModuleIndex];
      if (Off > Resume)
        Resume = Off;
      break;
    }

    const llvm::codeview::SymbolKind Kind = It->kind();
    if (isProcedureSymbol(Kind)) {
      auto ProcOr = llvm::codeview::SymbolDeserializer::deserializeAs<
          llvm::codeview::ProcSym>(*It);
      if (!ProcOr) {
        llvm::consumeError(ProcOr.takeError());
        continue;
      }
      OpenProc Next;
      Next.VA = PImpl->resolveVA(ProcOr->Segment, ProcOr->CodeOffset);
      Next.End = ProcOr->End;
      Next.FunctionType = ProcOr->FunctionType;
      Next.FunctionTypeFromIpi = isProcedureIdSymbol(Kind);
      if (Next.VA != 0) {
        PImpl->ModuleProcedureVAs[ModuleIndex].insert(Next.VA);
        PImpl->ModuleProcedureOffs[ModuleIndex].emplace(Next.VA, Off);
      }
      Next.Wanted = Next.VA == Addr;
      if (Next.Wanted && Next.VA != 0 && !ProcOr->Name.empty()) {
        FunctionSym &FS = PImpl->Functions[Next.VA];
        FS.Addr = Next.VA;
        if (FS.Name.empty())
          FS.Name = ProcOr->Name.str();
      }
      // `End` is the matching S_END. Skip unrequested bodies so a late
      // `--func` hit does not deserialize every prefix S_LOCAL.
      if (!Next.Wanted && Next.End > Off && Records.isOffsetValid(Next.End)) {
        It = llvm::codeview::CVSymbolArray::Iterator(
            Records, Records.getExtractor(), Next.End, &HadError);
        continue;
      }
      Open.push_back(std::move(Next));
      continue;
    }
    if (Open.empty() || !Open.back().Wanted)
      continue;

    OpenProc &Proc = Open.back();
    if (Kind == llvm::codeview::SymbolKind::S_FRAMEPROC) {
      auto FrameOr = llvm::codeview::SymbolDeserializer::deserializeAs<
          llvm::codeview::FrameProcSym>(*It);
      if (!FrameOr) {
        llvm::consumeError(FrameOr.takeError());
        continue;
      }
      Proc.FrameBytes = FrameOr->TotalFrameBytes;
      if (PImpl->CPU != llvm::codeview::CPUType::Unknown)
        Proc.LocalFP = FrameOr->getLocalFramePtrReg(PImpl->CPU);
      continue;
    }
    if (Kind == llvm::codeview::SymbolKind::S_LOCAL) {
      auto LocalOr = llvm::codeview::SymbolDeserializer::deserializeAs<
          llvm::codeview::LocalSym>(*It);
      if (!LocalOr) {
        llvm::consumeError(LocalOr.takeError());
        continue;
      }
      if (LocalOr->Name.empty())
        continue;
      Proc.PendingName = LocalOr->Name.str();
      Proc.PendingType = LocalOr->Type;
      Proc.PendingIsParam =
          (LocalOr->Flags & llvm::codeview::LocalSymFlags::IsParameter) !=
          llvm::codeview::LocalSymFlags::None;
      Proc.PendingPlaced = false;
      if (Proc.PendingIsParam)
        Proc.ParamIds.emplace_back(*Proc.PendingName, LocalOr->Type);
      continue;
    }
    if (Kind == llvm::codeview::SymbolKind::S_DEFRANGE_FRAMEPOINTER_REL) {
      auto RangeOr = llvm::codeview::SymbolDeserializer::deserializeAs<
          llvm::codeview::DefRangeFramePointerRelSym>(*It);
      if (!RangeOr) {
        llvm::consumeError(RangeOr.takeError());
        continue;
      }
      place(Proc, RangeOr->Hdr.Offset, llvm::codeview::RegisterId::NONE, true);
      continue;
    }
    if (Kind ==
        llvm::codeview::SymbolKind::S_DEFRANGE_FRAMEPOINTER_REL_FULL_SCOPE) {
      auto RangeOr = llvm::codeview::SymbolDeserializer::deserializeAs<
          llvm::codeview::DefRangeFramePointerRelFullScopeSym>(*It);
      if (!RangeOr) {
        llvm::consumeError(RangeOr.takeError());
        continue;
      }
      place(Proc, RangeOr->Offset, llvm::codeview::RegisterId::NONE, true);
      continue;
    }
    if (Kind == llvm::codeview::SymbolKind::S_DEFRANGE_REGISTER_REL) {
      auto RangeOr = llvm::codeview::SymbolDeserializer::deserializeAs<
          llvm::codeview::DefRangeRegisterRelSym>(*It);
      if (!RangeOr) {
        llvm::consumeError(RangeOr.takeError());
        continue;
      }
      if (RangeOr->hasSpilledUDTMember())
        continue;
      place(Proc,
            static_cast<int32_t>(RangeOr->Hdr.BasePointerOffset),
            static_cast<llvm::codeview::RegisterId>(
                static_cast<uint16_t>(RangeOr->Hdr.Register)),
            false);
      continue;
    }
    if (Kind == llvm::codeview::SymbolKind::S_BPREL32) {
      auto RelOr = llvm::codeview::SymbolDeserializer::deserializeAs<
          llvm::codeview::BPRelativeSym>(*It);
      if (!RelOr) {
        llvm::consumeError(RelOr.takeError());
        continue;
      }
      if (RelOr->Name.empty())
        continue;
      Proc.PendingName = RelOr->Name.str();
      Proc.PendingType = RelOr->Type;
      Proc.PendingIsParam = RelOr->Offset >= 0;
      Proc.PendingPlaced = false;
      if (Proc.PendingIsParam)
        Proc.ParamIds.emplace_back(*Proc.PendingName, RelOr->Type);
      place(Proc, RelOr->Offset, llvm::codeview::RegisterId::EBP, false);
      continue;
    }
    if (Kind == llvm::codeview::SymbolKind::S_REGREL32) {
      auto RelOr = llvm::codeview::SymbolDeserializer::deserializeAs<
          llvm::codeview::RegRelativeSym>(*It);
      if (!RelOr) {
        llvm::consumeError(RelOr.takeError());
        continue;
      }
      if (RelOr->Name.empty())
        continue;
      Proc.PendingName = RelOr->Name.str();
      Proc.PendingType = RelOr->Type;
      Proc.PendingIsParam = isIntegerArgumentRegister(RelOr->Register);
      Proc.PendingPlaced = false;
      if (Proc.PendingIsParam)
        Proc.ParamIds.emplace_back(*Proc.PendingName, RelOr->Type);
      place(Proc, static_cast<int32_t>(RelOr->Offset), RelOr->Register, false);
      continue;
    }
  }
  while (!Open.empty()) {
    if (Open.back().Wanted)
      finish(Open.back());
    Open.pop_back();
  }
  if (!TargetDone && !HadError) {
    PImpl->FullyIndexedLocalModules.insert(ModuleIndex);
    PImpl->LocalIndexResume.erase(ModuleIndex);
  }
  PImpl->LoadedLocalVAs.insert(Addr);
  (void)HadError;
}

std::optional<std::string> PDBDebugContext::functionName(va_t Addr) const {
  if (!PImpl)
    return std::nullopt;
  if (auto Err = PImpl->ingestPublicAt(Addr, /*Strict=*/false))
    llvm::consumeError(std::move(Err));
  std::lock_guard<std::mutex> Guard(PImpl->PublicMutex);
  const auto It = PImpl->Functions.find(Addr);
  if (It != PImpl->Functions.end() && !It->second.Name.empty())
    return It->second.Name;
  return std::nullopt;
}

std::optional<FunctionSym> PDBDebugContext::resolveFunction(va_t Addr) const {
  if (!PImpl)
    return std::nullopt;
  if (auto Err = PImpl->ingestPublicAt(Addr, /*Strict=*/false))
    llvm::consumeError(std::move(Err));
  ensureLocalsForAddress(Addr);
  std::lock_guard<std::mutex> Guard(PImpl->PublicMutex);
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
  ensureLocalsForAddress(FuncAddr);
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
  PImpl->materializeVariableType(OffIt->second);
  return OffIt->second;
}

std::optional<VariableSym>
PDBDebugContext::resolveStackPointerVariable(va_t FuncAddr,
                                             int64_t Offset) const {
  if (!PImpl)
    return std::nullopt;
  ensureLocalsForAddress(FuncAddr);
  const auto AmbIt = PImpl->AmbiguousStackPointerOffsets.find(FuncAddr);
  if (AmbIt != PImpl->AmbiguousStackPointerOffsets.end() &&
      AmbIt->second.count(Offset))
    return std::nullopt;
  const auto FuncIt = PImpl->StackPointerLocals.find(FuncAddr);
  if (FuncIt == PImpl->StackPointerLocals.end())
    return std::nullopt;
  const auto OffIt = FuncIt->second.find(Offset);
  if (OffIt == FuncIt->second.end())
    return std::nullopt;
  PImpl->materializeVariableType(OffIt->second);
  return OffIt->second;
}

void PDBDebugContext::completeType(const TypeRef &Ty) const {
  if (!PImpl || !Ty)
    return;
  PImpl->ensureRecordFields(Ty);
}

std::optional<TypeSym> PDBDebugContext::resolveType(uint64_t TypeId) const {
  if (!PImpl)
    return std::nullopt;
  const TypeRef Ty =
      PImpl->resolveTpi(llvm::codeview::TypeIndex(static_cast<uint32_t>(TypeId)));
  if (!Ty)
    return std::nullopt;
  TypeSym Out;
  Out.Type = Ty;
  Out.Name = Ty->SourceName;
  return Out;
}

std::optional<SourceLoc> PDBDebugContext::sourceLocation(va_t) const {
  return std::nullopt;
}

std::vector<FunctionSym> PDBDebugContext::allFunctions() const {
  std::vector<FunctionSym> Result;
  if (!PImpl)
    return Result;
  std::lock_guard<std::mutex> Guard(PImpl->PublicMutex);
  Result.reserve(PImpl->Functions.size());
  for (auto &[_, FS] : PImpl->Functions)
    Result.push_back(FS);
  return Result;
}

std::vector<DataObjectSym> PDBDebugContext::allDataObjects() const {
  std::vector<DataObjectSym> Result;
  if (!PImpl)
    return Result;
  std::lock_guard<std::mutex> Guard(PImpl->PublicMutex);
  Result.reserve(PImpl->DataObjects.size());
  for (auto &[_, Obj] : PImpl->DataObjects)
    Result.push_back(Obj);
  return Result;
}

std::optional<DataObjectSym>
PDBDebugContext::resolveDataObject(va_t Addr) const {
  if (!PImpl)
    return std::nullopt;
  if (auto Err = PImpl->ingestPublicAt(Addr, /*Strict=*/false))
    llvm::consumeError(std::move(Err));
  std::lock_guard<std::mutex> Guard(PImpl->PublicMutex);
  const auto It = PImpl->DataObjects.find(Addr);
  if (It == PImpl->DataObjects.end() || It->second.Name.empty())
    return std::nullopt;
  return It->second;
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
  // RSDS Phase A remains names-only, including S_LOCAL frame names.  JG TPI
  // + S_BPREL32_ST recovery is the only PDB path that currently authorizes
  // exact locals/params.
  return PImpl && PImpl->ObjectExtentsAuthenticated;
}

} // namespace neverd
