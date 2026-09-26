//===- PDB20Loader.cpp - VC6 JG / PDB 2.00 names-only loader -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/debug/PDBLoader.h"

#include "neverd/loader/BinaryImage.h"

#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/MSF/MappedBlockStream.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/Native/RawTypes.h"
#include "llvm/DebugInfo/PDB/PDB.h"
#include "llvm/DebugInfo/PDB/PDBTypes.h"
#include "llvm/Object/COFF.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
namespace {

llvm::Error pdb20Error(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                 "pdb20: " + Message);
}

llvm::Expected<std::vector<uint8_t>>
readIndexedStream(llvm::pdb::PDBFile &File, uint32_t Index) {
  if (Index >= File.getNumStreams())
    return pdb20Error("stream index out of range");
  const uint32_t Size = File.getStreamByteSize(Index);
  if (Size == 0 || Size == UINT32_MAX)
    return std::vector<uint8_t>();
  auto StreamOr = File.safelyCreateIndexedStream(Index);
  if (!StreamOr)
    return StreamOr.takeError();
  llvm::BinaryStreamReader Reader(**StreamOr);
  llvm::ArrayRef<uint8_t> Bytes;
  if (auto EC = Reader.readBytes(Bytes, Size))
    return std::move(EC);
  return std::vector<uint8_t>(Bytes.begin(), Bytes.end());
}

uint32_t read32(const std::vector<uint8_t> &Bytes, uint32_t Off) {
  return llvm::support::endian::read32le(Bytes.data() + Off);
}

struct SectionHdr {
  std::string Name;
  uint32_t VirtualAddress = 0;
  uint32_t VirtualSize = 0;
  uint32_t SizeOfRawData = 0;
  uint32_t PointerToRawData = 0;
  uint32_t Characteristics = 0;
};

llvm::Expected<std::vector<SectionHdr>>
parseSectionHeaders(const std::vector<uint8_t> &Bytes) {
  if (Bytes.size() % 40 != 0)
    return pdb20Error("section header stream is not a multiple of 40 bytes");
  std::vector<SectionHdr> Sections;
  Sections.reserve(Bytes.size() / 40);
  for (size_t I = 0; I < Bytes.size(); I += 40) {
    SectionHdr S;
    char Name[9] = {};
    std::memcpy(Name, Bytes.data() + I, 8);
    S.Name = Name;
    S.VirtualSize = read32(Bytes, static_cast<uint32_t>(I + 8));
    S.VirtualAddress = read32(Bytes, static_cast<uint32_t>(I + 12));
    S.SizeOfRawData = read32(Bytes, static_cast<uint32_t>(I + 16));
    S.PointerToRawData = read32(Bytes, static_cast<uint32_t>(I + 20));
    S.Characteristics = read32(Bytes, static_cast<uint32_t>(I + 36));
    Sections.push_back(std::move(S));
  }
  return Sections;
}

llvm::Error validateSections(const BinaryImage &Image,
                             llvm::ArrayRef<SectionHdr> PDBSections) {
  if (PDBSections.size() != Image.Sections.size())
    return pdb20Error("section table does not match loaded PE image");
  for (size_t I = 0; I < Image.Sections.size(); ++I) {
    const Section &Loaded = Image.Sections[I];
    const SectionHdr &Recorded = PDBSections[I];
    constexpr uint64_t MaxCOFFField = std::numeric_limits<uint32_t>::max();
    if (Loaded.VA < Image.Base || Loaded.Size > MaxCOFFField ||
        Loaded.FileOff > MaxCOFFField || Loaded.FileSz > MaxCOFFField ||
        Recorded.Name != Loaded.Name ||
        static_cast<uint64_t>(Recorded.VirtualAddress) !=
            Loaded.VA - Image.Base ||
        static_cast<uint64_t>(Recorded.VirtualSize) != Loaded.Size ||
        static_cast<uint64_t>(Recorded.PointerToRawData) != Loaded.FileOff ||
        static_cast<uint64_t>(Recorded.SizeOfRawData) != Loaded.FileSz ||
        Recorded.Characteristics != Loaded.Type)
      return pdb20Error("section table does not match loaded PE image");
  }
  return llvm::Error::success();
}

struct ModuleRec {
  int16_t Stream = -1;
  uint32_t SymBytes = 0;
};

llvm::Expected<std::vector<ModuleRec>>
parseModules(llvm::ArrayRef<uint8_t> Modi) {
  std::vector<ModuleRec> Mods;
  uint32_t Off = 0;
  while (Off + 64 <= Modi.size()) {
    ModuleRec M;
    M.Stream = static_cast<int16_t>(
        llvm::support::endian::read16le(Modi.data() + Off + 34));
    M.SymBytes =
        llvm::support::endian::read32le(Modi.data() + Off + 36);
    uint32_t P = Off + 64;
    while (P < Modi.size() && Modi[P] != 0)
      ++P;
    if (P >= Modi.size())
      return pdb20Error("truncated module name");
    ++P;
    while (P < Modi.size() && Modi[P] != 0)
      ++P;
    if (P >= Modi.size())
      return pdb20Error("truncated module object name");
    uint32_t End = (P + 1 + 3) & ~3u;
    if (End <= Off)
      break;
    Mods.push_back(M);
    Off = End;
  }
  return Mods;
}

struct ProcRec {
  uint16_t Segment = 0;
  uint32_t Offset = 0;
  uint32_t Size = 0;
  uint32_t TypeIndex = 0;
  std::string Name;
};

struct BpRelRec {
  int32_t Offset = 0;
  uint32_t TypeIndex = 0;
  std::string Name;
};

struct TypeLeaf {
  uint16_t Kind = 0;
  llvm::ArrayRef<uint8_t> Data;
};

bool parseLengthPrefixed(llvm::ArrayRef<uint8_t> Rec, uint32_t NameOff,
                         std::string &Name) {
  if (NameOff >= Rec.size())
    return false;
  const uint8_t Len = Rec[NameOff];
  if (NameOff + 1u + Len > Rec.size())
    return false;
  Name.assign(reinterpret_cast<const char *>(Rec.data() + NameOff + 1), Len);
  return !Name.empty();
}

int32_t readI32(llvm::ArrayRef<uint8_t> Bytes, uint32_t Off) {
  return static_cast<int32_t>(
      llvm::support::endian::read32le(Bytes.data() + Off));
}

DebugCallConv debugCallConv(uint8_t Call) {
  using CC = llvm::codeview::CallingConvention;
  switch (static_cast<CC>(Call)) {
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

uint16_t pointerBytes(Bitness Bits) {
  return Bits == Bitness::Bits64 ? 8 : 4;
}

TypeRef pointerTo(TypeRef Pointee, uint16_t Size) {
  auto Ptr = NdType::makePtr(Pointee ? Pointee : NdType::makeVoid());
  Ptr->Size = Size;
  return Ptr;
}

TypeRef primitiveType(uint32_t Index, uint16_t PtrSize) {
  using llvm::codeview::SimpleTypeKind;
  using llvm::codeview::SimpleTypeMode;
  using llvm::codeview::TypeIndex;
  if (Index >= TypeIndex::FirstNonSimpleIndex)
    return {};
  const auto Kind =
      static_cast<SimpleTypeKind>(Index & TypeIndex::SimpleKindMask);
  const auto Mode = static_cast<SimpleTypeMode>(Index & 0x700);
  TypeRef Base;
  switch (Kind) {
  case SimpleTypeKind::Void:
    Base = NdType::makeVoid();
    break;
  case SimpleTypeKind::HResult:
  case SimpleTypeKind::Int32Long:
  case SimpleTypeKind::Int32:
    Base = NdType::makeInt(4, true);
    break;
  case SimpleTypeKind::UInt32Long:
  case SimpleTypeKind::UInt32:
    Base = NdType::makeInt(4, false);
    break;
  case SimpleTypeKind::SignedCharacter:
  case SimpleTypeKind::SByte:
  case SimpleTypeKind::NarrowCharacter:
    Base = NdType::makeInt(1, true);
    break;
  case SimpleTypeKind::UnsignedCharacter:
  case SimpleTypeKind::Byte:
  case SimpleTypeKind::Boolean8:
    Base = NdType::makeInt(1, false);
    break;
  case SimpleTypeKind::Int16Short:
  case SimpleTypeKind::Int16:
  case SimpleTypeKind::WideCharacter:
    Base = NdType::makeInt(2, true);
    break;
  case SimpleTypeKind::UInt16Short:
  case SimpleTypeKind::UInt16:
  case SimpleTypeKind::Boolean16:
    Base = NdType::makeInt(2, false);
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
  if (Mode == SimpleTypeMode::Direct)
    return Base;
  uint16_t Size = PtrSize;
  if (Mode == SimpleTypeMode::NearPointer64)
    Size = 8;
  else if (Mode == SimpleTypeMode::NearPointer128)
    Size = 16;
  else if (Mode == SimpleTypeMode::NearPointer ||
           Mode == SimpleTypeMode::FarPointer ||
           Mode == SimpleTypeMode::HugePointer)
    Size = 2;
  return pointerTo(Base, Size);
}

struct TypeStore {
  std::map<uint32_t, TypeLeaf> Records;
  uint16_t PtrSize = 4;

  TypeRef resolve(uint32_t Index, int Depth = 0) const {
    if (Depth > 32)
      return {};
    if (Index < llvm::codeview::TypeIndex::FirstNonSimpleIndex)
      return primitiveType(Index, PtrSize);
    const auto It = Records.find(Index);
    if (It == Records.end())
      return {};
    const TypeLeaf &Leaf = It->second;
    using LK = llvm::codeview::TypeLeafKind;
    const llvm::ArrayRef<uint8_t> D = Leaf.Data;
    switch (Leaf.Kind) {
    case LK::LF_MODIFIER:
      if (D.size() < 4)
        return {};
      return resolve(llvm::support::endian::read32le(D.data()), Depth + 1);
    case LK::LF_POINTER:
      if (D.size() < 4)
        return {};
      return pointerTo(
          resolve(llvm::support::endian::read32le(D.data()), Depth + 1),
          PtrSize);
    case LK::LF_CLASS:
    case LK::LF_STRUCTURE:
    case LK::LF_UNION:
    case LK::LF_ENUM:
      return NdType::makeInt(4, true);
    default:
      return {};
    }
  }

  struct ProcType {
    TypeRef Return;
    DebugCallConv CallConv = DebugCallConv::Unknown;
    std::vector<TypeRef> Args;
    TypeRef ThisType;
  };

  std::optional<ProcType> procedure(uint32_t Index) const {
    if (Index < llvm::codeview::TypeIndex::FirstNonSimpleIndex)
      return std::nullopt;
    const auto It = Records.find(Index);
    if (It == Records.end())
      return std::nullopt;
    const TypeLeaf &Leaf = It->second;
    const llvm::ArrayRef<uint8_t> D = Leaf.Data;
    using LK = llvm::codeview::TypeLeafKind;
    ProcType Out;
    uint32_t ArgList = 0;
    uint16_t Argc = 0;
    if (Leaf.Kind == LK::LF_PROCEDURE) {
      if (D.size() < 12)
        return std::nullopt;
      Out.Return = resolve(llvm::support::endian::read32le(D.data()));
      Out.CallConv = debugCallConv(D[4]);
      Argc = llvm::support::endian::read16le(D.data() + 6);
      ArgList = llvm::support::endian::read32le(D.data() + 8);
    } else if (Leaf.Kind == LK::LF_MFUNCTION) {
      if (D.size() < 24)
        return std::nullopt;
      Out.Return = resolve(llvm::support::endian::read32le(D.data()));
      Out.ThisType = resolve(llvm::support::endian::read32le(D.data() + 8));
      Out.CallConv = debugCallConv(D[12]);
      Argc = llvm::support::endian::read16le(D.data() + 14);
      ArgList = llvm::support::endian::read32le(D.data() + 16);
    } else {
      return std::nullopt;
    }
    const auto ArgIt = Records.find(ArgList);
    if (ArgIt != Records.end() && ArgIt->second.Kind == LK::LF_ARGLIST &&
        ArgIt->second.Data.size() >= 4) {
      const uint32_t N =
          llvm::support::endian::read32le(ArgIt->second.Data.data());
      const uint32_t Limit = std::min<uint32_t>(N, Argc);
      for (uint32_t I = 0; I < Limit; ++I) {
        const uint32_t Off = 4 + I * 4;
        if (Off + 4 > ArgIt->second.Data.size())
          break;
        const uint32_t TI =
            llvm::support::endian::read32le(ArgIt->second.Data.data() + Off);
        if (TI == 0)
          continue;
        TypeRef T = resolve(TI);
        Out.Args.push_back(T ? T : NdType::makeInt(4, true));
      }
    }
    return Out;
  }
};

llvm::Expected<TypeStore> parseTpi(const std::vector<uint8_t> &Bytes,
                                   uint16_t PtrSize) {
  TypeStore Store;
  Store.PtrSize = PtrSize;
  if (Bytes.empty())
    return Store;
  if (Bytes.size() < 16)
    return pdb20Error("TPI stream is truncated");
  const uint32_t Version = read32(Bytes, 0);
  uint32_t HeaderLen = 0;
  uint32_t TypeMin = 0;
  uint32_t TypeMax = 0;
  uint32_t DataLen = 0;
  // Ghidra TypeProgramInterfaceParser: TI50/TI70/TI80 use the 800 header
  // (header length is a field).  TI20/40/41 use the 16-bit 200 header.
  switch (Version) {
  case 19961031: // TI50
  case 19990903: // TI70
  case 20040203: // TI80
    if (Bytes.size() < 20)
      return pdb20Error("TPI 800 header is truncated");
    HeaderLen = read32(Bytes, 4);
    TypeMin = read32(Bytes, 8);
    TypeMax = read32(Bytes, 12);
    DataLen = read32(Bytes, 16);
    break;
  case 19951204: // TI42
  case 19960307: // TI50DEP
    if (Bytes.size() < 18)
      return pdb20Error("TPI 500 header is truncated");
    HeaderLen = 20;
    TypeMin = read32(Bytes, 4);
    TypeMax = read32(Bytes, 8);
    DataLen = read32(Bytes, 12);
    break;
  case 920924:
  case 19950410:
  case 19951122:
    HeaderLen = 16;
    TypeMin = llvm::support::endian::read16le(Bytes.data() + 4);
    TypeMax = llvm::support::endian::read16le(Bytes.data() + 6);
    DataLen = read32(Bytes, 8);
    break;
  default:
    return pdb20Error("unknown TPI version");
  }
  if (HeaderLen < 16 || HeaderLen > Bytes.size() ||
      static_cast<uint64_t>(HeaderLen) + DataLen > Bytes.size() ||
      TypeMax < TypeMin)
    return pdb20Error("TPI header is malformed");
  uint32_t Off = HeaderLen;
  const uint32_t End = HeaderLen + DataLen;
  uint32_t TI = TypeMin;
  while (Off + 4 <= End && TI < TypeMax) {
    const uint16_t Len =
        llvm::support::endian::read16le(Bytes.data() + Off);
    const uint16_t Kind =
        llvm::support::endian::read16le(Bytes.data() + Off + 2);
    if (Len < 2 || Off + 2u + Len > End)
      return pdb20Error("TPI record overruns the type stream");
    TypeLeaf Leaf;
    Leaf.Kind = Kind;
    Leaf.Data = llvm::ArrayRef<uint8_t>(Bytes.data() + Off + 4, Len - 2);
    Store.Records.emplace(TI, Leaf);
    Off += 2u + Len;
    ++TI;
  }
  if (TI != TypeMax)
    return pdb20Error("TPI record count does not match the header");
  return Store;
}

void walkSymbols(llvm::ArrayRef<uint8_t> Bytes, uint32_t Start, uint32_t Limit,
                 std::vector<ProcRec> &Out,
                 std::map<uint64_t, std::vector<BpRelRec>> *Locals,
                 bool PublicsAsFunctions) {
  uint32_t Off = Start;
  const uint32_t End = std::min<uint32_t>(Limit, Bytes.size());
  uint64_t CurrentKey = 0;
  bool InProc = false;
  while (Off + 4 <= End) {
    const uint16_t Len =
        llvm::support::endian::read16le(Bytes.data() + Off);
    const uint16_t Kind =
        llvm::support::endian::read16le(Bytes.data() + Off + 2);
    if (Len < 2 || Off + 2u + Len > Bytes.size())
      break;
    llvm::ArrayRef<uint8_t> Rec(Bytes.data() + Off + 4, Len - 2);
    using SK = llvm::codeview::SymbolKind;
    if ((Kind == static_cast<uint16_t>(SK::S_GPROC32_ST) ||
         Kind == static_cast<uint16_t>(SK::S_LPROC32_ST)) &&
        Rec.size() >= 36) {
      ProcRec P;
      P.Size = llvm::support::endian::read32le(Rec.data() + 12);
      P.TypeIndex = llvm::support::endian::read32le(Rec.data() + 24);
      P.Offset = llvm::support::endian::read32le(Rec.data() + 28);
      P.Segment = llvm::support::endian::read16le(Rec.data() + 32);
      if (parseLengthPrefixed(Rec, 35, P.Name)) {
        CurrentKey = (static_cast<uint64_t>(P.Segment) << 32) | P.Offset;
        InProc = true;
        Out.push_back(std::move(P));
      }
    } else if (Kind == static_cast<uint16_t>(SK::S_END)) {
      InProc = false;
    } else if (Locals && InProc &&
               Kind == static_cast<uint16_t>(SK::S_BPREL32_ST) &&
               Rec.size() >= 9) {
      BpRelRec B;
      B.Offset = readI32(Rec, 0);
      B.TypeIndex = llvm::support::endian::read32le(Rec.data() + 4);
      if (parseLengthPrefixed(Rec, 8, B.Name))
        (*Locals)[CurrentKey].push_back(std::move(B));
    } else if (PublicsAsFunctions &&
               Kind == static_cast<uint16_t>(SK::S_PUB32_ST) &&
               Rec.size() >= 11) {
      const uint32_t Flags = llvm::support::endian::read32le(Rec.data());
      const bool IsFunc =
          (Flags & static_cast<uint32_t>(
                       llvm::codeview::PublicSymFlags::Function)) != 0;
      if (IsFunc) {
        ProcRec P;
        P.Offset = llvm::support::endian::read32le(Rec.data() + 4);
        P.Segment = llvm::support::endian::read16le(Rec.data() + 8);
        if (parseLengthPrefixed(Rec, 10, P.Name))
          Out.push_back(std::move(P));
      }
    }
    Off += 2u + Len;
  }
}

bool machineAcceptable(uint16_t Machine, Arch ImageArch) {
  if (Machine == 0)
    return ImageArch == Arch::X86;
  using llvm::pdb::PDB_Machine;
  switch (ImageArch) {
  case Arch::X64:
    return Machine == static_cast<uint16_t>(PDB_Machine::Amd64);
  case Arch::X86:
    return Machine == static_cast<uint16_t>(PDB_Machine::x86);
  case Arch::AArch64:
    return Machine == static_cast<uint16_t>(PDB_Machine::Arm64);
  case Arch::ARM:
    return Machine == static_cast<uint16_t>(PDB_Machine::ArmNT);
  default:
    return false;
  }
}

} // namespace

llvm::Expected<std::unique_ptr<PDBDebugContext>>
loadPdb20DebugContext(const std::filesystem::path &PdbPath,
                      const BinaryImage &Image) {
  if (Image.Format != BinaryFormat::COFF || Image.IsRelocatable)
    return pdb20Error("strict PDB loading requires a linked PE image");
  if (Image.DynInfo.CodeViewPDBIdentityState != PDBIdentityState::Unique ||
      !Image.DynInfo.CodeViewPDBIdentity ||
      Image.DynInfo.CodeViewPDBIdentity->Kind != PDBIdentityKind::NB10)
    return pdb20Error(Image.DynInfo.CodeViewPDBIdentityState ==
                              PDBIdentityState::Ambiguous
                          ? "PE CodeView identity is malformed or ambiguous"
                          : "PE image has no unique CodeView NB10 identity");

  std::unique_ptr<llvm::pdb::IPDBSession> Session;
  if (auto Err = llvm::pdb::loadDataForPDB(llvm::pdb::PDB_ReaderType::Native,
                                           llvm::StringRef(PdbPath.string()),
                                           Session))
    return llvm::createFileError(PdbPath.string(), std::move(Err));

  auto *Native = static_cast<llvm::pdb::NativeSession *>(Session.get());
  llvm::pdb::PDBFile &File = Native->getPDBFile();
  if (!File.isPdb20())
    return pdb20Error("container is not PDB 2.00 JG");

  auto InfoOr = File.getPDBInfoStream();
  if (!InfoOr)
    return pdb20Error("cannot read PDB Info stream: " +
                      llvm::toString(InfoOr.takeError()));
  auto &Info = *InfoOr;
  PDBBuildIdentity Actual;
  Actual.Kind = PDBIdentityKind::NB10;
  Actual.Signature = Info.getSignature();
  Actual.Age = Info.getAge();
  if (!Actual.isValid())
    return pdb20Error("PDB Info stream has an invalid signature/age");
  if (Actual != *Image.DynInfo.CodeViewPDBIdentity)
    return pdb20Error(
        "PDB Info signature/age does not match PE CodeView NB10");

  auto DbiOr = readIndexedStream(File, llvm::pdb::StreamDBI);
  if (!DbiOr)
    return DbiOr.takeError();
  const std::vector<uint8_t> &DBI = *DbiOr;
  if (DBI.size() < sizeof(llvm::pdb::DbiStreamHeader))
    return pdb20Error("DBI stream is truncated");

  llvm::pdb::DbiStreamHeader Header{};
  std::memcpy(&Header, DBI.data(), sizeof(Header));
  if (static_cast<int32_t>(Header.VersionSignature) != -1)
    return pdb20Error("DBI signature is not 0xFFFFFFFF");
  if (Header.Age != Info.getAge())
    return pdb20Error("DBI age does not match PDB Info age");
  if (!machineAcceptable(Header.MachineType, Image.Arch))
    return pdb20Error("DBI machine does not match loaded PE image");

  const int32_t ModiSize = Header.ModiSubstreamSize;
  const int32_t ScSize = Header.SecContrSubstreamSize;
  const int32_t SecMapSize = Header.SectionMapSize;
  const int32_t FileInfoSize = Header.FileInfoSize;
  const int32_t TypeServerSize = Header.TypeServerSize;
  const int32_t ECSize = Header.ECSubstreamSize;
  const int32_t DbgSize = Header.OptionalDbgHdrSize;
  if (ModiSize < 0 || ScSize < 0 || SecMapSize < 0 || FileInfoSize < 0 ||
      TypeServerSize < 0 || ECSize < 0 || DbgSize < 0)
    return pdb20Error("DBI substream size is negative");
  const uint64_t AfterHeader = 64ull + static_cast<uint32_t>(ModiSize) +
                               static_cast<uint32_t>(ScSize) +
                               static_cast<uint32_t>(SecMapSize) +
                               static_cast<uint32_t>(FileInfoSize) +
                               static_cast<uint32_t>(TypeServerSize) +
                               static_cast<uint32_t>(ECSize);
  if (AfterHeader + static_cast<uint32_t>(DbgSize) > DBI.size())
    return pdb20Error("DBI substreams overrun the stream");

  uint32_t SectionStream = llvm::pdb::kInvalidStreamIndex;
  if (DbgSize >= 12) {
    SectionStream = llvm::support::endian::read16le(
        DBI.data() + static_cast<size_t>(AfterHeader) + 10);
  }
  if (SectionStream == llvm::pdb::kInvalidStreamIndex)
    return pdb20Error("DBI has no section header debug stream");
  auto SecOr = readIndexedStream(File, SectionStream);
  if (!SecOr)
    return SecOr.takeError();
  auto SectionsOr = parseSectionHeaders(*SecOr);
  if (!SectionsOr)
    return SectionsOr.takeError();
  if (auto EC = validateSections(Image, *SectionsOr))
    return EC;

  auto ModsOr = parseModules(llvm::ArrayRef<uint8_t>(
      DBI.data() + 64, static_cast<size_t>(ModiSize)));
  if (!ModsOr)
    return ModsOr.takeError();

  auto TpiOr = readIndexedStream(File, llvm::pdb::StreamTPI);
  if (!TpiOr)
    return TpiOr.takeError();
  auto TypesOr = parseTpi(*TpiOr, pointerBytes(Image.Bits));
  if (!TypesOr)
    return TypesOr.takeError();
  const TypeStore &Types = *TypesOr;

  std::vector<ProcRec> Procs;
  std::map<uint64_t, std::vector<BpRelRec>> BpRels;
  for (const ModuleRec &Mod : *ModsOr) {
    if (Mod.Stream <= 0 || Mod.SymBytes < 8)
      continue;
    auto ModStream = readIndexedStream(File, static_cast<uint32_t>(Mod.Stream));
    if (!ModStream)
      return ModStream.takeError();
    if (ModStream->size() < 4 || read32(*ModStream, 0) != 2)
      continue;
    walkSymbols(*ModStream, 4, Mod.SymBytes, Procs, &BpRels, false);
  }

  const uint16_t SymRecs = Header.SymRecordStreamIndex;
  if (SymRecs != 0 && SymRecs != llvm::pdb::kInvalidStreamIndex) {
    auto Gsym = readIndexedStream(File, SymRecs);
    if (!Gsym)
      return Gsym.takeError();
    walkSymbols(*Gsym, 0, static_cast<uint32_t>(Gsym->size()), Procs, nullptr,
                true);
  }

  auto Ctx = std::unique_ptr<PDBDebugContext>(new PDBDebugContext());
  pdb_loader_detail::FunctionNameRegistry Names;
  std::map<va_t, uint32_t> Sizes;
  std::map<va_t, uint32_t> TypeIndices;
  std::set<va_t> AmbiguousSizes;
  std::set<va_t> Addresses;
  auto ResolveVA = [&](uint16_t Seg, uint32_t Off) -> va_t {
    if (Seg == 0 || Seg > Image.Sections.size())
      return 0;
    const Section &Owner = Image.Sections[Seg - 1];
    if (Off >= Owner.Size)
      return 0;
    return Owner.VA + Off;
  };
  for (const ProcRec &P : Procs) {
    const va_t VA = ResolveVA(P.Segment, P.Offset);
    if (VA == 0)
      continue;
    Addresses.insert(VA);
    Names.observe(VA, P.Name);
    if (P.Size != 0) {
      auto [It, Inserted] = Sizes.emplace(VA, P.Size);
      if (!Inserted && It->second != P.Size)
        AmbiguousSizes.insert(VA);
    }
    if (P.TypeIndex != 0)
      TypeIndices.emplace(VA, P.TypeIndex);
  }

  std::map<va_t, std::map<int64_t, VariableSym>> Locals;
  bool AnyTypedLocal = false;
  bool AnyTypedSignature = false;
  std::map<uint64_t, va_t> KeyToVA;
  for (const ProcRec &P : Procs) {
    const va_t VA = ResolveVA(P.Segment, P.Offset);
    if (VA == 0)
      continue;
    KeyToVA[(static_cast<uint64_t>(P.Segment) << 32) | P.Offset] = VA;
  }

  std::map<va_t, std::vector<VariableSym>> ParamsByFunc;
  for (const auto &[Key, Relocs] : BpRels) {
    const auto VAIt = KeyToVA.find(Key);
    if (VAIt == KeyToVA.end())
      continue;
    const va_t VA = VAIt->second;
    for (const BpRelRec &B : Relocs) {
      VariableSym VS;
      VS.Name = B.Name;
      VS.StackOffset = B.Offset;
      VS.Type = Types.resolve(B.TypeIndex);
      VS.IsParam = B.Offset >= 8;
      if (!VS.Type)
        continue;
      AnyTypedLocal = true;
      auto &Slot = Locals[VA];
      auto [It, Inserted] = Slot.emplace(B.Offset, VS);
      if (!Inserted && It->second.Name != VS.Name)
        Slot.erase(It);
      else if (VS.IsParam)
        ParamsByFunc[VA].push_back(VS);
    }
  }

  std::vector<FunctionSym> Functions;
  for (const va_t VA : Addresses) {
    const std::optional<std::string> Name = Names.name(VA);
    if (!Name)
      continue;
    FunctionSym FS;
    FS.Addr = VA;
    FS.Name = *Name;
    if (!AmbiguousSizes.count(VA)) {
      const auto SizeIt = Sizes.find(VA);
      if (SizeIt != Sizes.end())
        FS.Size = SizeIt->second;
    }
    const auto TypeIt = TypeIndices.find(VA);
    std::optional<TypeStore::ProcType> Proc;
    if (TypeIt != TypeIndices.end())
      Proc = Types.procedure(TypeIt->second);
    if (Proc) {
      FS.CallConv = Proc->CallConv;
      FS.ReturnType = Proc->Return;
      AnyTypedSignature = AnyTypedSignature || static_cast<bool>(Proc->Return);
      if (FS.CallConv == DebugCallConv::Thiscall && Proc->ThisType)
        FS.Params.emplace_back("this", Proc->ThisType);
    }
    auto ParamIt = ParamsByFunc.find(VA);
    if (ParamIt != ParamsByFunc.end()) {
      std::sort(ParamIt->second.begin(), ParamIt->second.end(),
                [](const VariableSym &A, const VariableSym &B) {
                  return A.StackOffset < B.StackOffset;
                });
      size_t Idx = 0;
      for (const VariableSym &P : ParamIt->second) {
        TypeRef Ty = P.Type;
        if (Proc && Idx < Proc->Args.size() && Proc->Args[Idx])
          Ty = Proc->Args[Idx];
        FS.Params.emplace_back(P.Name, Ty);
        ++Idx;
      }
      if (Proc && !Proc->Args.empty())
        FS.Params = bindDebugParamsToTpi(std::move(FS.Params), Proc->Args);
    } else if (Proc) {
      for (size_t I = 0; I < Proc->Args.size(); ++I)
        FS.Params.emplace_back("arg" + std::to_string(I), Proc->Args[I]);
    }
    Functions.push_back(std::move(FS));
  }

  Ctx->commitDebugFacts(std::move(Functions), std::move(Locals), true,
                        AnyTypedSignature, AnyTypedLocal);
  return Ctx;
}

} // namespace neverd
