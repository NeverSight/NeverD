//===- Syscalls.cpp - Solana runtime syscall metadata -------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/Syscalls.h"

#include "llvm/Support/Endian.h"

#include <array>
#include <limits>

namespace neverd::sbf {
namespace {

constexpr SyscallEffect None = SyscallEffect::None;
constexpr SyscallEffect ReadsMemory = SyscallEffect::ReadsMemory;
constexpr SyscallEffect WritesMemory = SyscallEffect::WritesMemory;
constexpr SyscallEffect Terminal = SyscallEffect::Terminal;
constexpr SyscallEffect CPI = SyscallEffect::CPI;

constexpr unsigned kMurmurWordBits = std::numeric_limits<uint32_t>::digits;
constexpr unsigned kMurmurBlockRotation = 15;
constexpr unsigned kMurmurHashRotation = 13;
constexpr unsigned kMurmurFinalShift1 = 16;
constexpr unsigned kMurmurFinalShift2 = 13;
constexpr unsigned kBitsPerByte = 8;
constexpr uint32_t kMurmurBlockMultiplier1 = 0xcc9e2d51u;
constexpr uint32_t kMurmurBlockMultiplier2 = 0x1b873593u;
constexpr uint32_t kMurmurFinalMultiplier1 = 0x85ebca6bu;
constexpr uint32_t kMurmurFinalMultiplier2 = 0xc2b2ae35u;
constexpr uint32_t kMurmurHashMultiplier = 5u;
constexpr uint32_t kMurmurHashIncrement = 0xe6546b64u;

constexpr uint32_t rotateLeft(uint32_t Value, unsigned Amount) {
  return (Value << Amount) | (Value >> (kMurmurWordBits - Amount));
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
#define SBF_SYSCALL(ID, NAME, ARGUMENT_COUNT, CATEGORY, EFFECTS)               \
  SyscallInfo{Syscall::ID,                                                     \
              NAME,                                                            \
              hashSymbolName(NAME),                                            \
              ARGUMENT_COUNT,                                                  \
              SyscallCategory::CATEGORY,                                       \
              EFFECTS},
#include "neverd/sbf/SBFSyscalls.def"
  };
  return Table;
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
