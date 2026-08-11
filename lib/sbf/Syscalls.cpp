//===- Syscalls.cpp - Solana runtime syscall metadata -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/Syscalls.h"

#include "neverd/sbf/SBFConstants.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

namespace neverd::sbf {
namespace {

constexpr SyscallEffect None = SyscallEffect::None;
constexpr SyscallEffect ReadsMemory = SyscallEffect::ReadsMemory;
constexpr SyscallEffect WritesMemory = SyscallEffect::WritesMemory;
constexpr SyscallEffect Terminal = SyscallEffect::Terminal;
constexpr SyscallEffect CPI = SyscallEffect::CPI;

constexpr SyscallPointerArguments NoPointers = SyscallPointerArguments::None;
constexpr SyscallPointerArguments Arg1 = SyscallPointerArguments::Arg1;
constexpr SyscallPointerArguments Arg2 = SyscallPointerArguments::Arg2;
constexpr SyscallPointerArguments Arg3 = SyscallPointerArguments::Arg3;
constexpr SyscallPointerArguments Arg4 = SyscallPointerArguments::Arg4;
constexpr SyscallPointerArguments Arg5 = SyscallPointerArguments::Arg5;

constexpr unsigned kMurmurWordBits = std::numeric_limits<uint32_t>::digits;
constexpr unsigned kMurmurBlockRotation = 15;
constexpr unsigned kMurmurHashRotation = 13;
constexpr unsigned kMurmurFinalShift1 = 16;
constexpr unsigned kMurmurFinalShift2 = 13;
constexpr uint32_t kMurmurBlockMultiplier1 = 0xcc9e2d51u;
constexpr uint32_t kMurmurBlockMultiplier2 = 0x1b873593u;
constexpr uint32_t kMurmurFinalMultiplier1 = 0x85ebca6bu;
constexpr uint32_t kMurmurFinalMultiplier2 = 0xc2b2ae35u;
constexpr uint32_t kMurmurHashMultiplier = 5u;
constexpr uint32_t kMurmurHashIncrement = 0xe6546b64u;

constexpr uint32_t rotateLeft(uint32_t Value, unsigned Amount) {
  return (Value << Amount) | (Value >> (kMurmurWordBits - Amount));
}

/// The three window builders. They exist so the neutral value of the detail
/// field is written once here rather than at every row of the table.
constexpr SyscallMemoryInfo fixedWindow(Syscall ID, SyscallArgument Argument,
                                        SyscallMemoryAccess Access,
                                        uint64_t Bytes) {
  return {ID, Argument, Access, SyscallExtent::Fixed, Bytes};
}

constexpr SyscallMemoryInfo countedWindow(Syscall ID, SyscallArgument Argument,
                                          SyscallMemoryAccess Access,
                                          SyscallArgument Length) {
  return {ID, Argument, Access, SyscallExtent::Counted,
          argumentOrdinal(Length)};
}

constexpr SyscallMemoryInfo opaqueWindow(Syscall ID, SyscallArgument Argument,
                                         SyscallMemoryAccess Access) {
  return {ID, Argument, Access, SyscallExtent::Opaque, 0};
}

uint32_t fmix(uint32_t Value) {
  Value ^= Value >> kMurmurFinalShift1;
  Value *= kMurmurFinalMultiplier1;
  Value ^= Value >> kMurmurFinalShift2;
  Value *= kMurmurFinalMultiplier2;
  Value ^= Value >> kMurmurFinalShift1;
  return Value;
}

} // namespace

uint32_t hashSymbolName(llvm::StringRef Name) {
  uint32_t Hash = 0;
  const auto *Bytes = reinterpret_cast<const uint8_t *>(Name.data());
  size_t Offset = 0;
  while (Name.size() - Offset >= sizeof(uint32_t)) {
    uint32_t Block = llvm::support::endian::read32le(Bytes + Offset);
    Block *= kMurmurBlockMultiplier1;
    Block = rotateLeft(Block, kMurmurBlockRotation);
    Block *= kMurmurBlockMultiplier2;
    Hash ^= Block;
    Hash = rotateLeft(Hash, kMurmurHashRotation);
    Hash = Hash * kMurmurHashMultiplier + kMurmurHashIncrement;
    Offset += sizeof(uint32_t);
  }

  uint32_t Tail = 0;
  switch (Name.size() - Offset) {
  case 3:
    Tail ^= static_cast<uint32_t>(Bytes[Offset + 2]) << (2 * kBitsPerByte);
    [[fallthrough]];
  case 2:
    Tail ^= static_cast<uint32_t>(Bytes[Offset + 1]) << kBitsPerByte;
    [[fallthrough]];
  case 1:
    Tail ^= Bytes[Offset];
    Tail *= kMurmurBlockMultiplier1;
    Tail = rotateLeft(Tail, kMurmurBlockRotation);
    Tail *= kMurmurBlockMultiplier2;
    Hash ^= Tail;
    break;
  default:
    break;
  }

  Hash ^= static_cast<uint32_t>(Name.size());
  return fmix(Hash);
}

llvm::ArrayRef<SyscallInfo> syscallInfos() {
  static const std::array Table = {
#define SBF_SYSCALL(ID, NAME, ARGUMENT_COUNT, POINTER_ARGUMENTS, RETURN_KIND,  \
                    CATEGORY, EFFECTS, AVAILABILITY, SOURCE)                   \
  SyscallInfo{Syscall::ID,                                                     \
              NAME,                                                            \
              hashSymbolName(NAME),                                            \
              ARGUMENT_COUNT,                                                  \
              POINTER_ARGUMENTS,                                               \
              SyscallReturnKind::RETURN_KIND,                                  \
              SyscallCategory::CATEGORY,                                       \
              EFFECTS,                                                         \
              SyscallAvailability::AVAILABILITY,                               \
              SyscallSource::SOURCE},
#include "neverd/sbf/SBFSyscalls.def"
  };
  return Table;
}

std::optional<uint64_t> SyscallMemoryInfo::fixedBytes() const {
  if (Extent != SyscallExtent::Fixed)
    return std::nullopt;
  return Detail;
}

std::optional<SyscallArgument> SyscallMemoryInfo::lengthArgument() const {
  if (Extent != SyscallExtent::Counted)
    return std::nullopt;
  return static_cast<SyscallArgument>(Detail);
}

llvm::StringRef syscallMemoryAccessName(SyscallMemoryAccess Access) {
  switch (Access) {
  case SyscallMemoryAccess::Read:
    return "read";
  case SyscallMemoryAccess::Write:
    return "write";
  }
  return "unknown";
}

llvm::StringRef syscallExtentName(SyscallExtent Extent) {
  switch (Extent) {
  case SyscallExtent::Fixed:
    return "fixed";
  case SyscallExtent::Counted:
    return "counted";
  case SyscallExtent::Opaque:
    return "opaque";
  }
  return "unknown";
}

llvm::ArrayRef<SyscallMemoryInfo> syscallMemoryInfos() {
  static const std::array Table = {
#define SBF_SYSCALL_MEMORY_FIXED(SYSCALL, ARGUMENT, ACCESS, BYTES)             \
  fixedWindow(Syscall::SYSCALL, SyscallArgument::ARGUMENT,                     \
              SyscallMemoryAccess::ACCESS, BYTES),
#define SBF_SYSCALL_MEMORY_COUNTED(SYSCALL, ARGUMENT, ACCESS, LENGTH_ARGUMENT) \
  countedWindow(Syscall::SYSCALL, SyscallArgument::ARGUMENT,                   \
                SyscallMemoryAccess::ACCESS,                                   \
                SyscallArgument::LENGTH_ARGUMENT),
#define SBF_SYSCALL_MEMORY_OPAQUE(SYSCALL, ARGUMENT, ACCESS)                   \
  opaqueWindow(Syscall::SYSCALL, SyscallArgument::ARGUMENT,                    \
               SyscallMemoryAccess::ACCESS),
#include "neverd/sbf/SBFSyscallMemory.def"
  };
  return Table;
}

llvm::ArrayRef<SyscallMemoryInfo> getSyscallMemory(Syscall ID) {
  const llvm::ArrayRef<SyscallMemoryInfo> Table = syscallMemoryInfos();
  // The table groups every syscall's windows together, which
  // validateSyscallMemoryTable enforces, so one contiguous span is the whole
  // answer.
  const auto First = llvm::find_if(
      Table, [&](const SyscallMemoryInfo &Info) { return Info.ID == ID; });
  if (First == Table.end())
    return {};
  const auto Last = std::find_if(
      First, Table.end(),
      [&](const SyscallMemoryInfo &Info) { return Info.ID != ID; });
  return Table.slice(First - Table.begin(), Last - First);
}

bool preservesCallerMemory(Syscall ID) {
  if (!getSyscallInfo(ID))
    return false;
  return llvm::none_of(getSyscallMemory(ID), [](const SyscallMemoryInfo &Row) {
    return Row.Access == SyscallMemoryAccess::Write;
  });
}

llvm::Error validateSyscallMemoryTable() {
  const llvm::ArrayRef<SyscallMemoryInfo> Table = syscallMemoryInfos();
  llvm::SmallVector<Syscall> Seen;
  llvm::SmallVector<std::pair<Syscall, uint16_t>> Windows;

  const auto Fail = [](llvm::Twine Message) {
    return llvm::make_error<llvm::StringError>(
        ("sbf: syscall memory: " + Message).str(),
        llvm::inconvertibleErrorCode());
  };

  for (auto [Index, Row] : llvm::enumerate(Table)) {
    const SyscallInfo *Info = getSyscallInfo(Row.ID);
    if (!Info)
      return Fail("syscall memory window " + llvm::Twine(Index) +
                  " names a syscall the table does not declare");
    const llvm::StringRef Name = Info->Name;

    // getSyscallMemory answers with one contiguous span, so a syscall whose
    // rows are split by another syscall's would silently lose the later half.
    const bool StartsGroup = Index == 0 || Table[Index - 1].ID != Row.ID;
    if (StartsGroup) {
      if (llvm::is_contained(Seen, Row.ID))
        return Fail(Name + " has windows that are not listed together");
      Seen.push_back(Row.ID);
    }

    const unsigned Ordinal = argumentOrdinal(Row.Argument);
    if (Ordinal >= Info->ArgumentCount)
      return Fail(Name + " opens a window on an argument it does not take");
    if (!isPointerArgument(Info->PointerArguments, Row.Argument))
      return Fail(Name +
                  " opens a window on an argument not declared to hold an "
                  "address");

    const auto Key = std::pair(
        Row.ID, static_cast<uint16_t>(Ordinal << 1 |
                                      static_cast<unsigned>(Row.Access)));
    if (llvm::is_contained(Windows, Key))
      return Fail(Name + " declares the same window twice");
    Windows.push_back(Key);

    if (const std::optional<SyscallArgument> Length = Row.lengthArgument()) {
      const unsigned LengthOrdinal = argumentOrdinal(*Length);
      if (LengthOrdinal >= Info->ArgumentCount)
        return Fail(Name + " counts a window with an argument it does not "
                           "take");
      if (isPointerArgument(Info->PointerArguments, *Length))
        return Fail(Name + " counts a window with an address argument");
    }
    if (Row.Extent == SyscallExtent::Fixed && Row.Detail == 0)
      return Fail(Name + " declares a fixed window of no bytes");
  }

  // The effect summary and the window table describe the same behaviour at two
  // resolutions, so they agree in both directions. Without the second
  // direction a syscall could declare that it writes caller memory and then
  // describe no window, and recovery would keep trusting bytes that call had
  // overwritten.
  for (const SyscallInfo &Info : syscallInfos()) {
    const llvm::ArrayRef<SyscallMemoryInfo> Rows = getSyscallMemory(Info.ID);
    const auto Opens = [&](SyscallMemoryAccess Access) {
      return llvm::any_of(Rows, [&](const SyscallMemoryInfo &Row) {
        return Row.Access == Access;
      });
    };
    for (auto [Access, Effect] :
         {std::pair(SyscallMemoryAccess::Read, SyscallEffect::ReadsMemory),
          std::pair(SyscallMemoryAccess::Write, SyscallEffect::WritesMemory)})
      if (Opens(Access) != hasEffect(Info.Effects, Effect))
        return Fail(Info.Name + " disagrees with its " +
                    syscallMemoryAccessName(Access) +
                    " windows about whether it touches caller memory");
  }
  return llvm::Error::success();
}

llvm::StringRef syscallAvailabilityName(SyscallAvailability Availability) {
  switch (Availability) {
#define SBF_SYSCALL_AVAILABILITY(ID, SPELLING)                                 \
  case SyscallAvailability::ID:                                                \
    return SPELLING;
#include "neverd/sbf/SBFSyscallAvailability.def"
  }
  return "unknown";
}

llvm::ArrayRef<SyscallSourceInfo> syscallSourceInfos() {
  static const std::array Table = {
#define SBF_UPSTREAM_SOURCE(ID, NAME, REVISION)                                \
  SyscallSourceInfo{SyscallSource::ID, NAME, REVISION},
#include "neverd/sbf/SBFUpstreamSources.def"
  };
  return Table;
}

llvm::StringRef syscallSourceName(SyscallSource Source) {
  for (const SyscallSourceInfo &Info : syscallSourceInfos())
    if (Info.ID == Source)
      return Info.Name;
  return "unknown";
}

llvm::StringRef syscallSourceRevision(SyscallSource Source) {
  for (const SyscallSourceInfo &Info : syscallSourceInfos())
    if (Info.ID == Source)
      return Info.Revision;
  return {};
}

const SyscallInfo *getSyscallInfo(uint32_t Hash) {
  for (const SyscallInfo &Info : syscallInfos())
    if (Info.Hash == Hash)
      return &Info;
  return nullptr;
}

const SyscallInfo *getSyscallInfo(Syscall ID) {
  for (const SyscallInfo &Info : syscallInfos())
    if (Info.ID == ID)
      return &Info;
  return nullptr;
}

const SyscallInfo *findSyscallByName(llvm::StringRef Name) {
  for (const SyscallInfo &Info : syscallInfos())
    if (Info.Name == Name)
      return &Info;
  return nullptr;
}

} // namespace neverd::sbf
