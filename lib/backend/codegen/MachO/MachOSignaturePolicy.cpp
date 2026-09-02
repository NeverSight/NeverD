//===- MachOSignaturePolicy.cpp - Strict Mach-O signing policy ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/backend/codegen/MachO/MachOSignaturePolicy.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/SHA256.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace neverd::macho_signature {

namespace {

using llvm::MachO::CS_BlobIndex;
using llvm::MachO::CS_CodeDirectory;
using llvm::MachO::CS_SuperBlob;

// LLVM 20 has the XML entitlement constants but not Apple's later DER slot.
// The on-disk structures still come exclusively from llvm::MachO above.
constexpr uint32_t DerEntitlementsSlot = 7;
constexpr uint32_t DerEntitlementsMagic = 0xfade7172;

llvm::Error malformed(llvm::StringRef Detail) {
  return llvm::make_error<llvm::StringError>("macho signature: " + Detail,
                                             llvm::inconvertibleErrorCode());
}

bool rangeInBounds(uint64_t Offset, uint64_t Size, uint64_t Total) {
  return Offset <= Total && Size <= Total - Offset;
}

std::optional<uint32_t> readBE32(llvm::ArrayRef<uint8_t> Bytes,
                                 uint64_t Offset) {
  if (!rangeInBounds(Offset, sizeof(uint32_t), Bytes.size()))
    return std::nullopt;
  return llvm::support::endian::read32be(Bytes.data() + Offset);
}

std::optional<uint64_t> readBE64(llvm::ArrayRef<uint8_t> Bytes,
                                 uint64_t Offset) {
  if (!rangeInBounds(Offset, sizeof(uint64_t), Bytes.size()))
    return std::nullopt;
  return llvm::support::endian::read64be(Bytes.data() + Offset);
}

bool isCodeDirectorySlot(uint32_t Type) {
  using namespace llvm::MachO;
  return Type == CSSLOT_CODEDIRECTORY ||
         (Type >= CSSLOT_ALTERNATE_CODEDIRECTORIES &&
          Type < CSSLOT_ALTERNATE_CODEDIRECTORY_LIMIT);
}

struct ParsedCodeDirectory {
  uint32_t Version = 0;
  uint32_t Flags = 0;
  uint32_t SpecialSlots = 0;
  uint8_t HashType = 0;
  uint8_t HashSize = 0;
  uint8_t PageSize = 0;
  uint64_t ExecSegmentFlags = 0;
  bool SpecialSlotOneIsZero = false;
  bool HasSpecialSlotTwoSHA256 = false;
  std::array<uint8_t, llvm::MachO::CS_SHA256_LEN> SpecialSlotTwoSHA256{};
  std::string Identifier;
};

llvm::Expected<ParsedCodeDirectory>
parseCodeDirectory(llvm::ArrayRef<uint8_t> Blob, uint64_t ExpectedCodeLimit) {
  using namespace llvm::MachO;
  constexpr uint32_t EarliestSupportedVersion = 0x20000;
  constexpr size_t BaseHeaderSize = offsetof(CS_CodeDirectory, scatterOffset);
  if (Blob.size() < BaseHeaderSize)
    return malformed("CodeDirectory is truncated");

  const std::optional<uint32_t> Magic =
      readBE32(Blob, offsetof(CS_CodeDirectory, magic));
  const std::optional<uint32_t> Length =
      readBE32(Blob, offsetof(CS_CodeDirectory, length));
  const std::optional<uint32_t> Version =
      readBE32(Blob, offsetof(CS_CodeDirectory, version));
  const std::optional<uint32_t> Flags =
      readBE32(Blob, offsetof(CS_CodeDirectory, flags));
  const std::optional<uint32_t> HashOffset =
      readBE32(Blob, offsetof(CS_CodeDirectory, hashOffset));
  const std::optional<uint32_t> IdentifierOffset =
      readBE32(Blob, offsetof(CS_CodeDirectory, identOffset));
  const std::optional<uint32_t> SpecialSlots =
      readBE32(Blob, offsetof(CS_CodeDirectory, nSpecialSlots));
  const std::optional<uint32_t> CodeSlots =
      readBE32(Blob, offsetof(CS_CodeDirectory, nCodeSlots));
  const std::optional<uint32_t> CodeLimit =
      readBE32(Blob, offsetof(CS_CodeDirectory, codeLimit));
  if (!Magic || !Length || !Version || !Flags || !HashOffset ||
      !IdentifierOffset || !SpecialSlots || !CodeSlots || !CodeLimit ||
      *Magic != CSMAGIC_CODEDIRECTORY || *Length != Blob.size())
    return malformed("CodeDirectory header is invalid");

  size_t MinimumHeaderSize = BaseHeaderSize;
  if (*Version < EarliestSupportedVersion)
    return malformed("CodeDirectory version is invalid");
  if (*Version >= CS_SUPPORTSSCATTER)
    MinimumHeaderSize = offsetof(CS_CodeDirectory, teamOffset);
  if (*Version >= CS_SUPPORTSTEAMID)
    MinimumHeaderSize = offsetof(CS_CodeDirectory, spare3);
  if (*Version >= CS_SUPPORTSCODELIMIT64)
    MinimumHeaderSize = offsetof(CS_CodeDirectory, execSegBase);
  if (*Version >= CS_SUPPORTSEXECSEG)
    MinimumHeaderSize = sizeof(CS_CodeDirectory);
  if (*Version >= CS_SUPPORTSRUNTIME)
    MinimumHeaderSize = sizeof(CS_CodeDirectory) + 2 * sizeof(uint32_t);
  if (*Version > CS_SUPPORTSRUNTIME)
    return malformed("CodeDirectory version is unsupported");
  if (Blob.size() < MinimumHeaderSize)
    return malformed("CodeDirectory is shorter than its versioned header");

  const uint8_t Platform = Blob[offsetof(CS_CodeDirectory, platform)];
  const uint8_t PageSize = Blob[offsetof(CS_CodeDirectory, pageSize)];
  const std::optional<uint32_t> Spare2 =
      readBE32(Blob, offsetof(CS_CodeDirectory, spare2));
  if (Platform != 0 || !Spare2 || *Spare2 != 0)
    return malformed("CodeDirectory reserved policy field is nonzero");
  if (*Version >= CS_SUPPORTSSCATTER) {
    const std::optional<uint32_t> ScatterOffset =
        readBE32(Blob, offsetof(CS_CodeDirectory, scatterOffset));
    if (!ScatterOffset || *ScatterOffset != 0)
      return malformed("CodeDirectory reserved policy field is nonzero");
  }
  if (*Version >= CS_SUPPORTSTEAMID) {
    const std::optional<uint32_t> TeamOffset =
        readBE32(Blob, offsetof(CS_CodeDirectory, teamOffset));
    if (!TeamOffset || *TeamOffset != 0)
      return malformed("CodeDirectory reserved policy field is nonzero");
  }
  if (*Version >= CS_SUPPORTSCODELIMIT64) {
    const std::optional<uint32_t> Spare3 =
        readBE32(Blob, offsetof(CS_CodeDirectory, spare3));
    if (!Spare3 || *Spare3 != 0)
      return malformed("CodeDirectory reserved policy field is nonzero");
  }
  if (*Version >= CS_SUPPORTSRUNTIME) {
    const std::optional<uint32_t> PreEncryptOffset =
        readBE32(Blob, sizeof(CS_CodeDirectory) + sizeof(uint32_t));
    if (!PreEncryptOffset || *PreEncryptOffset != 0)
      return malformed("CodeDirectory pre-encryption hashes are unsupported");
  }

  if (*IdentifierOffset < MinimumHeaderSize || *IdentifierOffset >= Blob.size())
    return malformed("CodeDirectory identifier offset is invalid");
  if (!std::all_of(Blob.begin() + MinimumHeaderSize,
                   Blob.begin() + *IdentifierOffset,
                   [](uint8_t Byte) { return Byte == 0; }))
    return malformed("CodeDirectory has nonzero unclaimed bytes");

  const uint8_t *IdentifierBegin = Blob.data() + *IdentifierOffset;
  const size_t Remaining = Blob.size() - *IdentifierOffset;
  const void *Terminator = std::memchr(IdentifierBegin, 0, Remaining);
  if (!Terminator)
    return malformed("CodeDirectory identifier is not terminated");
  const auto *IdentifierEnd = static_cast<const uint8_t *>(Terminator);
  if (IdentifierEnd == IdentifierBegin)
    return malformed("CodeDirectory identifier is empty");

  const uint8_t HashSize = Blob[offsetof(CS_CodeDirectory, hashSize)];
  const uint8_t HashType = Blob[offsetof(CS_CodeDirectory, hashType)];
  uint8_t ExpectedHashSize = 0;
  switch (HashType) {
  case CS_HASHTYPE_SHA1:
    ExpectedHashSize = CS_SHA1_LEN;
    break;
  case CS_HASHTYPE_SHA256:
    ExpectedHashSize = CS_SHA256_LEN;
    break;
  case CS_HASHTYPE_SHA256_TRUNCATED:
    ExpectedHashSize = CS_SHA256_TRUNCATED_LEN;
    break;
  case CS_HASHTYPE_SHA384:
    ExpectedHashSize = CS_HASH_MAX_SIZE;
    break;
  default:
    break;
  }
  if (ExpectedHashSize == 0 || HashSize != ExpectedHashSize)
    return malformed("CodeDirectory hash encoding is unsupported");
  const uint64_t SpecialHashBytes = uint64_t(*SpecialSlots) * HashSize;
  const uint64_t CodeHashBytes = uint64_t(*CodeSlots) * HashSize;
  if (*HashOffset < SpecialHashBytes ||
      *HashOffset - SpecialHashBytes < MinimumHeaderSize ||
      !rangeInBounds(*HashOffset, CodeHashBytes, Blob.size()) ||
      uint64_t(*HashOffset) + CodeHashBytes != Blob.size())
    return malformed("CodeDirectory hash region is invalid");
  const uint64_t SpecialHashStart = *HashOffset - SpecialHashBytes;
  const uint64_t IdentifierEndOffset = IdentifierEnd - Blob.data();
  if (IdentifierEndOffset + 1 > SpecialHashStart)
    return malformed("CodeDirectory identifier overlaps its hash region");
  if (!std::all_of(Blob.begin() + IdentifierEndOffset + 1,
                   Blob.begin() + SpecialHashStart,
                   [](uint8_t Byte) { return Byte == 0; }))
    return malformed("CodeDirectory has nonzero unclaimed bytes");

  bool SpecialSlotOneIsZero = false;
  if (*SpecialSlots >= 1) {
    const uint64_t SlotOneOffset = *HashOffset - HashSize;
    SpecialSlotOneIsZero = std::all_of(Blob.begin() + SlotOneOffset,
                                       Blob.begin() + SlotOneOffset + HashSize,
                                       [](uint8_t Byte) { return Byte == 0; });
  }
  bool HasSpecialSlotTwoSHA256 = false;
  std::array<uint8_t, CS_SHA256_LEN> SpecialSlotTwoSHA256{};
  if (*SpecialSlots >= 2 && HashType == CS_HASHTYPE_SHA256 &&
      HashSize == CS_SHA256_LEN) {
    const uint64_t SlotTwoOffset = *HashOffset - 2 * HashSize;
    std::copy_n(Blob.begin() + SlotTwoOffset, CS_SHA256_LEN,
                SpecialSlotTwoSHA256.begin());
    HasSpecialSlotTwoSHA256 = true;
  }

  uint64_t EffectiveCodeLimit = *CodeLimit;
  if (*Version >= CS_SUPPORTSCODELIMIT64) {
    const std::optional<uint64_t> CodeLimit64 =
        readBE64(Blob, offsetof(CS_CodeDirectory, codeLimit64));
    if (!CodeLimit64)
      return malformed("CodeDirectory codeLimit64 is truncated");
    if (*CodeLimit == std::numeric_limits<uint32_t>::max()) {
      if (*CodeLimit64 == 0)
        return malformed("CodeDirectory codeLimit64 is invalid");
      EffectiveCodeLimit = *CodeLimit64;
    } else if (*CodeLimit64 != 0)
      return malformed("CodeDirectory has an unexpected codeLimit64");
  }
  if (EffectiveCodeLimit != ExpectedCodeLimit)
    return malformed("CodeDirectory codeLimit does not end at the signature");

  if (PageSize == 0 || PageSize >= 63)
    return malformed("CodeDirectory page size is unsupported");
  const uint64_t PageBytes = uint64_t{1} << PageSize;
  const uint64_t ExpectedCodeSlots =
      EffectiveCodeLimit / PageBytes +
      (EffectiveCodeLimit % PageBytes != 0 ? 1 : 0);
  if (ExpectedCodeSlots != *CodeSlots)
    return malformed("CodeDirectory page coverage is incomplete");

  uint64_t ExecSegmentFlags = 0;
  if (*Version >= CS_SUPPORTSEXECSEG) {
    const std::optional<uint64_t> ExecSegmentBase =
        readBE64(Blob, offsetof(CS_CodeDirectory, execSegBase));
    const std::optional<uint64_t> ExecSegmentLimit =
        readBE64(Blob, offsetof(CS_CodeDirectory, execSegLimit));
    const std::optional<uint64_t> ParsedExecSegmentFlags =
        readBE64(Blob, offsetof(CS_CodeDirectory, execSegFlags));
    if (!ExecSegmentBase || !ExecSegmentLimit || !ParsedExecSegmentFlags ||
        *ExecSegmentBase > EffectiveCodeLimit ||
        *ExecSegmentLimit > EffectiveCodeLimit - *ExecSegmentBase)
      return malformed("CodeDirectory executable segment is out of bounds");
    ExecSegmentFlags = *ParsedExecSegmentFlags;
  }

  ParsedCodeDirectory Result;
  Result.Version = *Version;
  Result.Flags = *Flags;
  Result.SpecialSlots = *SpecialSlots;
  Result.HashType = HashType;
  Result.HashSize = HashSize;
  Result.PageSize = PageSize;
  Result.ExecSegmentFlags = ExecSegmentFlags;
  Result.SpecialSlotOneIsZero = SpecialSlotOneIsZero;
  Result.HasSpecialSlotTwoSHA256 = HasSpecialSlotTwoSHA256;
  Result.SpecialSlotTwoSHA256 = SpecialSlotTwoSHA256;
  Result.Identifier.assign(
      reinterpret_cast<const char *>(IdentifierBegin),
      static_cast<size_t>(IdentifierEnd - IdentifierBegin));
  return Result;
}

llvm::Expected<SliceProfile>
inspectSlice(const llvm::object::MachOObjectFile &Object) {
  using namespace llvm::MachO;
  SliceProfile Result;
  Result.CPUType = static_cast<uint32_t>(Object.getHeader().cputype);
  Result.CPUSubtype = static_cast<uint32_t>(Object.getHeader().cpusubtype);

  std::optional<linkedit_data_command> SignatureCommand;
  for (const llvm::object::MachOObjectFile::LoadCommandInfo &Command :
       Object.load_commands()) {
    if (Command.C.cmd != LC_CODE_SIGNATURE)
      continue;
    if (SignatureCommand)
      return malformed("duplicate LC_CODE_SIGNATURE load command");
    if (Command.C.cmdsize != sizeof(linkedit_data_command))
      return malformed("LC_CODE_SIGNATURE cmdsize is invalid");
    SignatureCommand = Object.getLinkeditDataLoadCommand(Command);
  }
  if (!SignatureCommand)
    return Result;

  const llvm::StringRef ObjectData = Object.getData();
  const llvm::ArrayRef<uint8_t> Bytes(
      reinterpret_cast<const uint8_t *>(ObjectData.data()), ObjectData.size());
  const uint64_t HeaderSize =
      Object.is64Bit() ? sizeof(mach_header_64) : sizeof(mach_header);
  const uint64_t LoadCommandsEnd = HeaderSize + Object.getHeader().sizeofcmds;
  if (SignatureCommand->dataoff < LoadCommandsEnd ||
      !rangeInBounds(SignatureCommand->dataoff, SignatureCommand->datasize,
                     Bytes.size()) ||
      SignatureCommand->datasize < sizeof(CS_SuperBlob))
    return malformed("LC_CODE_SIGNATURE range is invalid");
  if (uint64_t(SignatureCommand->dataoff) + SignatureCommand->datasize !=
      Bytes.size())
    return malformed("LC_CODE_SIGNATURE must terminate the Mach-O slice");

  const llvm::ArrayRef<uint8_t> RawSignature =
      Bytes.slice(SignatureCommand->dataoff, SignatureCommand->datasize);
  const std::optional<uint32_t> SuperMagic =
      readBE32(RawSignature, offsetof(CS_SuperBlob, magic));
  const std::optional<uint32_t> SuperLength =
      readBE32(RawSignature, offsetof(CS_SuperBlob, length));
  const std::optional<uint32_t> SlotCount =
      readBE32(RawSignature, offsetof(CS_SuperBlob, count));
  if (!SuperMagic || *SuperMagic != CSMAGIC_EMBEDDED_SIGNATURE)
    return malformed("SuperBlob magic is invalid");
  if (!SuperLength || *SuperLength < sizeof(CS_SuperBlob) ||
      *SuperLength > RawSignature.size())
    return malformed("SuperBlob length is invalid");
  if (!std::all_of(RawSignature.begin() + *SuperLength, RawSignature.end(),
                   [](uint8_t Byte) { return Byte == 0; }))
    return malformed("SuperBlob padding is not zero");
  const llvm::ArrayRef<uint8_t> Signature =
      RawSignature.take_front(*SuperLength);
  if (!SlotCount || *SlotCount == 0)
    return malformed("SuperBlob has no slots");
  if (*SlotCount >
      (Signature.size() - sizeof(CS_SuperBlob)) / sizeof(CS_BlobIndex))
    return malformed("SuperBlob slot count exceeds its index table");

  const uint64_t IndexEnd =
      sizeof(CS_SuperBlob) + uint64_t(*SlotCount) * sizeof(CS_BlobIndex);
  struct BlobRange {
    uint32_t Type = 0;
    uint32_t Offset = 0;
    uint32_t Length = 0;
  };
  std::vector<BlobRange> Ranges;
  Ranges.reserve(*SlotCount);
  std::set<uint32_t> SeenTypes;
  for (uint32_t Index = 0; Index < *SlotCount; ++Index) {
    const uint64_t EntryOffset =
        sizeof(CS_SuperBlob) + uint64_t(Index) * sizeof(CS_BlobIndex);
    const std::optional<uint32_t> Type =
        readBE32(Signature, EntryOffset + offsetof(CS_BlobIndex, type));
    const std::optional<uint32_t> Offset =
        readBE32(Signature, EntryOffset + offsetof(CS_BlobIndex, offset));
    if (!Type || !Offset || *Offset < IndexEnd ||
        !rangeInBounds(*Offset, offsetof(CS_CodeDirectory, version),
                       Signature.size()))
      return malformed("SuperBlob index range is invalid");
    if (!SeenTypes.insert(*Type).second)
      return malformed("duplicate signature slot");
    const std::optional<uint32_t> Length =
        readBE32(Signature, *Offset + offsetof(CS_CodeDirectory, length));
    if (!Length || *Length < offsetof(CS_CodeDirectory, version) ||
        !rangeInBounds(*Offset, *Length, Signature.size()))
      return malformed("signature slot length is invalid");
    Ranges.push_back({*Type, *Offset, *Length});
  }

  std::vector<BlobRange> SortedRanges = Ranges;
  std::sort(SortedRanges.begin(), SortedRanges.end(),
            [](const BlobRange &Left, const BlobRange &Right) {
              return Left.Offset < Right.Offset;
            });
  for (size_t Index = 1; Index < SortedRanges.size(); ++Index) {
    const uint64_t PreviousEnd = uint64_t(SortedRanges[Index - 1].Offset) +
                                 SortedRanges[Index - 1].Length;
    if (PreviousEnd > SortedRanges[Index].Offset)
      return malformed("signature slots overlap");
  }
  uint64_t ClaimedEnd = IndexEnd;
  bool HasInternalGap = false;
  for (const BlobRange &Range : SortedRanges) {
    HasInternalGap |= ClaimedEnd != Range.Offset;
    if (!std::all_of(Signature.begin() + ClaimedEnd,
                     Signature.begin() + Range.Offset,
                     [](uint8_t Byte) { return Byte == 0; }))
      return malformed("SuperBlob has nonzero unclaimed bytes");
    ClaimedEnd = uint64_t(Range.Offset) + Range.Length;
  }
  HasInternalGap |= ClaimedEnd != Signature.size();
  if (!std::all_of(Signature.begin() + ClaimedEnd, Signature.end(),
                   [](uint8_t Byte) { return Byte == 0; }))
    return malformed("SuperBlob has nonzero unclaimed bytes");

  unsigned PrimaryCodeDirectories = 0;
  bool HasEntitlements = false;
  bool HasCMS = false;
  bool HasCanonicalEmptyRequirements = false;
  bool HasCanonicalEmptyCMSWrapper = false;
  std::optional<std::array<uint8_t, CS_SHA256_LEN>> RequirementsSHA256;
  std::optional<ParsedCodeDirectory> PrimaryDirectory;
  std::optional<std::string> Identifier;
  uint32_t CombinedFlags = 0;
  for (const BlobRange &Range : Ranges) {
    const llvm::ArrayRef<uint8_t> Blob =
        Signature.slice(Range.Offset, Range.Length);
    const std::optional<uint32_t> Magic =
        readBE32(Blob, offsetof(CS_CodeDirectory, magic));
    if (!Magic)
      return malformed("signature slot magic is missing");

    if (isCodeDirectorySlot(Range.Type)) {
      if (*Magic != CSMAGIC_CODEDIRECTORY)
        return malformed("CodeDirectory slot has the wrong magic");
      llvm::Expected<ParsedCodeDirectory> Parsed =
          parseCodeDirectory(Blob, SignatureCommand->dataoff);
      if (!Parsed)
        return Parsed.takeError();
      if (Range.Type == CSSLOT_CODEDIRECTORY)
        ++PrimaryCodeDirectories;
      else
        Result.HasUnsupportedSlots = true;
      if (Range.Type == CSSLOT_CODEDIRECTORY) {
        PrimaryDirectory = *Parsed;
        Result.CodeDirectoryVersion = Parsed->Version;
        Result.CodeDirectoryHashType = Parsed->HashType;
        Result.CodeDirectoryHashSize = Parsed->HashSize;
        Result.CodeDirectoryPageSize = Parsed->PageSize;
        Result.CodeDirectorySpecialSlots = Parsed->SpecialSlots;
        Result.ExecSegmentFlags = Parsed->ExecSegmentFlags;
      }
      if (Identifier && *Identifier != Parsed->Identifier)
        return malformed("CodeDirectory identifiers disagree");
      Identifier = Parsed->Identifier;
      CombinedFlags |= Parsed->Flags;
      continue;
    }

    if (Range.Type == CSSLOT_REQUIREMENTS) {
      if (*Magic != CSMAGIC_REQUIREMENTS)
        return malformed("requirements slot has the wrong magic");
      if (Blob.size() < sizeof(CS_SuperBlob))
        return malformed("requirements slot is truncated");
      const std::optional<uint32_t> RequirementCount =
          readBE32(Blob, offsetof(CS_SuperBlob, count));
      if (!RequirementCount)
        return malformed("requirements slot count is truncated");
      HasCanonicalEmptyRequirements =
          Blob.size() == sizeof(CS_SuperBlob) && *RequirementCount == 0;
      if (HasCanonicalEmptyRequirements)
        RequirementsSHA256 = llvm::SHA256::hash(Blob);
      Result.HasUnsupportedSlots = true;
      continue;
    }

    if (Range.Type == CSSLOT_ENTITLEMENTS ||
        Range.Type == DerEntitlementsSlot) {
      const uint32_t ExpectedMagic = Range.Type == CSSLOT_ENTITLEMENTS
                                         ? CSMAGIC_EMBEDDED_ENTITLEMENTS
                                         : DerEntitlementsMagic;
      if (*Magic != ExpectedMagic)
        return malformed("entitlements slot has the wrong magic");
      HasEntitlements = true;
      continue;
    }
    if (Range.Type == CSSLOT_SIGNATURESLOT) {
      if (*Magic != CSMAGIC_BLOBWRAPPER)
        return malformed("CMS signature slot has the wrong magic");
      HasCanonicalEmptyCMSWrapper =
          Blob.size() == offsetof(CS_CodeDirectory, version);
      HasCMS = !HasCanonicalEmptyCMSWrapper;
      Result.HasUnsupportedSlots = true;
      continue;
    }

    // Requirements, resource seals, tickets, identification records, and any
    // future slot are valid strong metadata but cannot be silently discarded
    // by NeverD's identityless re-signing policy.
    Result.HasUnsupportedSlots = true;
  }

  if (PrimaryCodeDirectories != 1)
    return malformed("signature must contain one primary CodeDirectory");
  if (!Identifier)
    return malformed("signature has no CodeDirectory identifier");

  Result.HasCanonicalCodesignOutputShape =
      Ranges.size() == 3 && !HasInternalGap && HasCanonicalEmptyRequirements &&
      HasCanonicalEmptyCMSWrapper && PrimaryDirectory && RequirementsSHA256 &&
      PrimaryDirectory->SpecialSlots == 2 &&
      PrimaryDirectory->SpecialSlotOneIsZero &&
      PrimaryDirectory->HasSpecialSlotTwoSHA256 &&
      PrimaryDirectory->SpecialSlotTwoSHA256 == *RequirementsSHA256;

  constexpr uint32_t SimpleFlags = CS_ADHOC | CS_LINKER_SIGNED;
  constexpr uint32_t HardenedFlags = CS_HARD | CS_KILL | CS_CHECK_EXPIRATION |
                                     CS_RESTRICT | CS_ENFORCEMENT |
                                     CS_REQUIRE_LV | CS_RUNTIME;
  constexpr uint32_t EntitlementFlags = CS_GET_TASK_ALLOW | CS_INSTALLER |
                                        CS_NVRAM_UNRESTRICTED |
                                        CS_DATAVAULT_CONTROLLER;
  Result.CodeDirectoryFlags = CombinedFlags;
  Result.LinkerSigned = (CombinedFlags & CS_LINKER_SIGNED) != 0;
  Result.HasUnsupportedFlags = (CombinedFlags & ~SimpleFlags) != 0;
  Result.Identifier = std::move(*Identifier);

  if (HasCMS || (CombinedFlags & CS_ADHOC) == 0)
    Result.SignatureKind = Kind::DeveloperSigned;
  else if ((CombinedFlags & HardenedFlags) != 0)
    Result.SignatureKind = Kind::Hardened;
  else if (HasEntitlements || (CombinedFlags & EntitlementFlags) != 0)
    Result.SignatureKind = Kind::Entitled;
  else
    Result.SignatureKind = Kind::IdentitylessAdHoc;
  return Result;
}

llvm::Expected<std::unique_ptr<llvm::object::Binary>>
parseContainer(llvm::ArrayRef<uint8_t> Binary) {
  const llvm::StringRef Bytes(reinterpret_cast<const char *>(Binary.data()),
                              Binary.size());
  llvm::Expected<std::unique_ptr<llvm::object::Binary>> Parsed =
      llvm::object::createBinary(llvm::MemoryBufferRef(Bytes, "<memory>"));
  if (!Parsed) {
    llvm::consumeError(Parsed.takeError());
    return malformed("input is not a valid Mach-O container");
  }
  return Parsed;
}

} // namespace

llvm::Expected<Profile> inspect(llvm::ArrayRef<uint8_t> Binary) {
  llvm::Expected<std::unique_ptr<llvm::object::Binary>> Parsed =
      parseContainer(Binary);
  if (!Parsed)
    return Parsed.takeError();

  Profile Result;
  if (const auto *Object =
          llvm::dyn_cast<llvm::object::MachOObjectFile>(Parsed->get())) {
    llvm::Expected<SliceProfile> Slice = inspectSlice(*Object);
    if (!Slice)
      return Slice.takeError();
    Result.Slices.push_back(std::move(*Slice));
    return Result;
  }

  const auto *Universal =
      llvm::dyn_cast<llvm::object::MachOUniversalBinary>(Parsed->get());
  if (!Universal)
    return malformed("input is not a thin or universal Mach-O");
  Result.Universal = true;
  for (const llvm::object::MachOUniversalBinary::ObjectForArch &Object :
       Universal->objects()) {
    llvm::Expected<std::unique_ptr<llvm::object::MachOObjectFile>> SliceObject =
        Object.getAsObjectFile();
    if (!SliceObject) {
      llvm::consumeError(SliceObject.takeError());
      return malformed("universal Mach-O contains a non-object slice");
    }
    llvm::Expected<SliceProfile> Slice = inspectSlice(**SliceObject);
    if (!Slice)
      return Slice.takeError();
    Result.Slices.push_back(std::move(*Slice));
  }
  if (Result.Slices.empty())
    return malformed("universal Mach-O contains no slices");
  return Result;
}

bool canTransactionallyAdHocResign(const Profile &Value) {
  if (Value.Slices.empty())
    return false;
  std::optional<std::string> Identifier;
  std::set<std::pair<uint32_t, uint32_t>> Architectures;
  for (const SliceProfile &Slice : Value.Slices) {
    if (Slice.SignatureKind != Kind::IdentitylessAdHoc ||
        Slice.HasUnsupportedFlags || Slice.HasUnsupportedSlots ||
        Slice.CodeDirectoryVersion > llvm::MachO::CS_SUPPORTSEXECSEG ||
        Slice.CodeDirectoryHashType != llvm::MachO::CS_HASHTYPE_SHA256 ||
        Slice.CodeDirectoryHashSize != llvm::MachO::CS_SHA256_LEN ||
        Slice.CodeDirectoryPageSize == 0 ||
        Slice.CodeDirectorySpecialSlots != 0 ||
        (Slice.ExecSegmentFlags &
         ~uint64_t(llvm::MachO::CS_EXECSEG_MAIN_BINARY)) != 0 ||
        Slice.Identifier.empty() ||
        !Architectures.emplace(Slice.CPUType, Slice.CPUSubtype).second)
      return false;
    if (Identifier && *Identifier != Slice.Identifier)
      return false;
    Identifier = Slice.Identifier;
  }
  return true;
}

bool isCanonicalCodesignAdHocOutput(const Profile &Value) {
  if (Value.Slices.empty())
    return false;
  std::optional<std::string> Identifier;
  std::set<std::pair<uint32_t, uint32_t>> Architectures;
  for (const SliceProfile &Slice : Value.Slices) {
    if (Slice.SignatureKind != Kind::IdentitylessAdHoc ||
        !Slice.HasCanonicalCodesignOutputShape || Slice.HasUnsupportedFlags ||
        Slice.CodeDirectoryVersion > llvm::MachO::CS_SUPPORTSEXECSEG ||
        Slice.CodeDirectoryHashType != llvm::MachO::CS_HASHTYPE_SHA256 ||
        Slice.CodeDirectoryHashSize != llvm::MachO::CS_SHA256_LEN ||
        Slice.CodeDirectoryPageSize == 0 ||
        Slice.CodeDirectorySpecialSlots != 2 ||
        (Slice.ExecSegmentFlags &
         ~uint64_t(llvm::MachO::CS_EXECSEG_MAIN_BINARY)) != 0 ||
        Slice.Identifier.empty() ||
        !Architectures.emplace(Slice.CPUType, Slice.CPUSubtype).second)
      return false;
    if (Identifier && *Identifier != Slice.Identifier)
      return false;
    Identifier = Slice.Identifier;
  }
  return true;
}

llvm::Error validateTransactionallyAdHocResigned(const Profile &Before,
                                                 const Profile &After) {
  if (!canTransactionallyAdHocResign(Before))
    return malformed("input is not eligible for identityless ad-hoc signing");
  if (!isCanonicalCodesignAdHocOutput(After))
    return malformed(
        "re-signed output is not a canonical codesign ad-hoc signature");
  if (Before.Slices.size() != After.Slices.size())
    return malformed("re-signed output slice set changed");

  for (const SliceProfile &BeforeSlice : Before.Slices) {
    const auto Match =
        std::find_if(After.Slices.begin(), After.Slices.end(),
                     [&](const SliceProfile &AfterSlice) {
                       return BeforeSlice.CPUType == AfterSlice.CPUType &&
                              BeforeSlice.CPUSubtype == AfterSlice.CPUSubtype;
                     });
    if (Match == After.Slices.end())
      return malformed("re-signed output slice set changed");
    if (Match->Identifier != BeforeSlice.Identifier)
      return malformed("re-signed output identifier changed");
    if (Match->CodeDirectoryHashType != BeforeSlice.CodeDirectoryHashType ||
        Match->CodeDirectoryHashSize != BeforeSlice.CodeDirectoryHashSize)
      return malformed("re-signed output hash strength changed");
    if (Match->CodeDirectoryPageSize != BeforeSlice.CodeDirectoryPageSize)
      return malformed("re-signed output page size changed");
    if (Match->ExecSegmentFlags != BeforeSlice.ExecSegmentFlags)
      return malformed("re-signed output executable-segment policy changed");
  }
  return llvm::Error::success();
}

} // namespace neverd::macho_signature
