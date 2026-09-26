//===- PDBFunctionNameHint.cpp - Cheap exact public-name lookup -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/debug/PDBFunctionNameHint.h"

#include "neverd/Limits.h"
#include "neverd/debug/PDBLoader.h"

#include "llvm/BinaryFormat/COFF.h"
#include "llvm/DebugInfo/MSF/MappedBlockStream.h"
#include "llvm/DebugInfo/PDB/IPDBSession.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/GlobalsStream.h"
#include "llvm/DebugInfo/PDB/Native/Hash.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/NativeSession.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/PublicsStream.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/PDB.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/CVDebugRecord.h"
#include "llvm/Support/BinaryStreamRef.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <vector>

namespace neverd {

namespace {

bool machineMatches(uint16_t Machine, llvm::pdb::PDB_Machine PDBMachine) {
  using llvm::pdb::PDB_Machine;
  switch (Machine) {
  case llvm::COFF::IMAGE_FILE_MACHINE_AMD64:
    return PDBMachine == PDB_Machine::Amd64;
  case llvm::COFF::IMAGE_FILE_MACHINE_I386:
    return PDBMachine == PDB_Machine::x86;
  case llvm::COFF::IMAGE_FILE_MACHINE_ARM64:
    return PDBMachine == PDB_Machine::Arm64;
  case llvm::COFF::IMAGE_FILE_MACHINE_ARMNT:
    return PDBMachine == PDB_Machine::ArmNT;
  default:
    return false;
  }
}

bool sectionsMatch(const llvm::object::COFFObjectFile &Object,
                   const llvm::pdb::DbiStream &DBI,
                   std::vector<const llvm::object::coff_section *> &Sections) {
  const auto Recorded = DBI.getSectionHeaders();
  if (Recorded.size() != Object.getNumberOfSections())
    return false;
  Sections.reserve(Recorded.size());
  for (const llvm::object::SectionRef &Ref : Object.sections()) {
    const unsigned ID = Object.getSectionID(Ref);
    if (ID != Sections.size() + 1 || ID > Recorded.size())
      return false;
    const auto *Actual = Object.getCOFFSection(Ref);
    const auto &Expected = Recorded[ID - 1];
    if (std::memcmp(Actual->Name, Expected.Name, llvm::COFF::NameSize) != 0 ||
        Actual->VirtualAddress != Expected.VirtualAddress ||
        Actual->VirtualSize != Expected.VirtualSize ||
        Actual->PointerToRawData != Expected.PointerToRawData ||
        Actual->SizeOfRawData != Expected.SizeOfRawData ||
        Actual->Characteristics != Expected.Characteristics)
      return false;
    Sections.push_back(Actual);
  }
  return Sections.size() == Recorded.size();
}

bool readPublic(llvm::BinaryStreamRef Stream, uint32_t Offset,
                pdb_loader_detail::ParsedPublicSym32 &Public) {
  if (Offset > Stream.getLength() || Stream.getLength() - Offset < 4)
    return false;
  llvm::ArrayRef<uint8_t> Prefix;
  if (auto Err = Stream.readBytes(Offset, 4, Prefix)) {
    llvm::consumeError(std::move(Err));
    return false;
  }
  const uint32_t Size = llvm::support::endian::read16le(Prefix.data()) + 2u;
  if (Size > Stream.getLength() - Offset)
    return false;
  llvm::ArrayRef<uint8_t> Bytes;
  if (auto Err = Stream.readBytes(Offset, Size, Bytes)) {
    llvm::consumeError(std::move(Err));
    return false;
  }
  if (auto Err = pdb_loader_detail::parsePublicSym32At(Bytes, 0, Public)) {
    llvm::consumeError(std::move(Err));
    return false;
  }
  return true;
}

std::optional<va_t> resolve(const std::filesystem::path &BinaryPath,
                            const std::filesystem::path &PDBPath,
                            llvm::StringRef Name) {
  if (Name.empty() || BinaryPath.empty() || PDBPath.empty())
    return std::nullopt;

  auto BufferOr = llvm::MemoryBuffer::getFile(BinaryPath.string());
  if (!BufferOr)
    return std::nullopt;
  auto ObjectOr =
      llvm::object::COFFObjectFile::create((*BufferOr)->getMemBufferRef());
  if (!ObjectOr) {
    llvm::consumeError(ObjectOr.takeError());
    return std::nullopt;
  }
  const auto &Object = **ObjectOr;
  if (!Object.getPE32Header() && !Object.getPE32PlusHeader())
    return std::nullopt;

  // The full PE loader rejects ambiguous CodeView identities.  An early hint
  // must be at least as conservative before it maps a PDB address to a VA.
  const llvm::object::debug_directory *CodeView = nullptr;
  for (const auto &Entry : Object.debug_directories()) {
    if (Entry.Type != llvm::COFF::IMAGE_DEBUG_TYPE_CODEVIEW)
      continue;
    if (CodeView)
      return std::nullopt;
    CodeView = &Entry;
  }
  if (!CodeView)
    return std::nullopt;
  const llvm::codeview::DebugInfo *PEInfo = nullptr;
  llvm::StringRef RecordedPath;
  if (auto Err = Object.getDebugPDBInfo(CodeView, PEInfo, RecordedPath)) {
    llvm::consumeError(std::move(Err));
    return std::nullopt;
  }
  if (!PEInfo || PEInfo->PDB70.CVSignature != llvm::OMF::Signature::PDB70)
    return std::nullopt;

  std::unique_ptr<llvm::pdb::IPDBSession> Session;
  if (auto Err = llvm::pdb::loadDataForPDB(llvm::pdb::PDB_ReaderType::Native,
                                           PDBPath.string(), Session)) {
    llvm::consumeError(std::move(Err));
    return std::nullopt;
  }
  auto &PDB =
      static_cast<llvm::pdb::NativeSession *>(Session.get())->getPDBFile();
  auto InfoOr = PDB.getPDBInfoStream();
  if (!InfoOr) {
    llvm::consumeError(InfoOr.takeError());
    return std::nullopt;
  }
  const auto &Info = *InfoOr;
  const auto GUID = Info.getGuid();
  if (Info.getAge() != PEInfo->PDB70.Age ||
      std::memcmp(GUID.Guid, PEInfo->PDB70.Signature,
                  sizeof(PEInfo->PDB70.Signature)) != 0)
    return std::nullopt;

  auto DBIOr = PDB.getPDBDbiStream();
  if (!DBIOr) {
    llvm::consumeError(DBIOr.takeError());
    return std::nullopt;
  }
  const auto &DBI = *DBIOr;
  if (DBI.getAge() != Info.getAge() ||
      !machineMatches(Object.getMachine(), DBI.getMachineType()))
    return std::nullopt;
  std::vector<const llvm::object::coff_section *> Sections;
  if (!sectionsMatch(Object, DBI, Sections))
    return std::nullopt;

  if (!PDB.hasPDBPublicsStream())
    return std::nullopt;
  auto PublicsOr = PDB.getPDBPublicsStream();
  if (!PublicsOr) {
    llvm::consumeError(PublicsOr.takeError());
    return std::nullopt;
  }
  const auto &Table = PublicsOr->getPublicsTable();
  const uint32_t Hash = llvm::pdb::hashStringV1(Name) % llvm::pdb::IPHR_HASH;
  const int32_t Bucket = Table.BucketMap[Hash];
  if (Bucket < 0)
    return std::nullopt;
  const uint32_t Index = static_cast<uint32_t>(Bucket);
  if (Index >= Table.HashBuckets.size())
    return std::nullopt;

  // GSI bucket offsets use twelve-byte slots; LLVM's GlobalsStream uses the
  // same conversion for its indexed lookup.  Validate before indexing the
  // records because this is untrusted debug data.
  const uint64_t FirstByte = Table.HashBuckets[Index];
  const uint64_t LastByte =
      Index + 1 < Table.HashBuckets.size()
          ? static_cast<uint32_t>(Table.HashBuckets[Index + 1])
          : static_cast<uint64_t>(Table.HashRecords.size()) * 12;
  if (FirstByte % 12 != 0 || LastByte % 12 != 0 || FirstByte > LastByte ||
      LastByte / 12 > Table.HashRecords.size())
    return std::nullopt;

  const uint32_t SymIndex = DBI.getSymRecordStreamIndex();
  if (SymIndex == llvm::pdb::kInvalidStreamIndex ||
      SymIndex >= PDB.getNumStreams())
    return std::nullopt;
  auto SymbolStream = PDB.createIndexedStream(static_cast<uint16_t>(SymIndex));
  if (!SymbolStream)
    return std::nullopt;
  llvm::BinaryStreamRef Stream(*SymbolStream);

  std::optional<va_t> Found;
  for (uint64_t I = FirstByte / 12; I < LastByte / 12; ++I) {
    const uint32_t OnDiskOffset = Table.HashRecords[I].Off;
    if (OnDiskOffset == 0)
      return std::nullopt;
    pdb_loader_detail::ParsedPublicSym32 Public;
    if (!readPublic(Stream, OnDiskOffset - 1, Public))
      return std::nullopt;
    if (!Public.IsFunction || Public.Name != Name)
      continue;
    if (Public.Segment == 0 || Public.Segment > Sections.size())
      return std::nullopt;
    const auto *Section = Sections[Public.Segment - 1];
    if (Public.Offset >= Section->VirtualSize)
      return std::nullopt;
    const uint64_t Base = Object.getImageBase();
    const uint64_t RVA = Section->VirtualAddress;
    if (RVA > InvalidVA - Base || Public.Offset > InvalidVA - (Base + RVA))
      return std::nullopt;
    const va_t Address = Base + RVA + Public.Offset;
    if (Address == 0 || (Found && *Found != Address))
      return std::nullopt;
    Found = Address;
  }
  return Found;
}

} // namespace

std::optional<va_t>
resolvePDBPublicFunctionNameHint(const std::filesystem::path &BinaryPath,
                                 const std::filesystem::path &PDBPath,
                                 llvm::StringRef Name) {
  return resolve(BinaryPath, PDBPath, Name);
}

} // namespace neverd
