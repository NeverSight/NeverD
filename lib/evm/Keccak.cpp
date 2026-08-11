//===- Keccak.cpp - Ethereum's Keccak-256 permutation --------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "Keccak.h"

#include "neverd/evm/EVMConstants.h"

#include "llvm/ADT/bit.h"

#include <algorithm>

namespace neverd::evm {
namespace {

inline constexpr unsigned kKeccakDimension = 5;
inline constexpr unsigned kKeccakLaneCount =
    kKeccakDimension * kKeccakDimension;
inline constexpr unsigned kKeccakRoundCount = 24;
inline constexpr size_t kKeccak256RateBytes = 136;
inline constexpr uint8_t kKeccakDomainSeparator = 0x01;
inline constexpr uint8_t kKeccakFinalBit = 0x80;

static_assert(kKeccak256RateBytes % sizeof(uint64_t) == 0,
              "Keccak rate must contain complete lanes");
static_assert(kKeccak256DigestBytes <= kKeccak256RateBytes,
              "one Keccak squeeze must contain a whole digest");
static_assert(kKeccak256DigestBytes == kWordBytes,
              "the hashing opcode pushes one whole digest");
static_assert(kSelectorBytes <= kKeccak256DigestBytes,
              "a selector is a prefix of one digest");

void keccakF1600(std::array<uint64_t, kKeccakLaneCount> &State) {
  static constexpr uint64_t RoundConstants[] = {
      0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
      0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
      0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
      0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
      0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
      0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
      0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
      0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL};
  static constexpr unsigned Rotation[] = {0,  1, 62, 28, 27, 36, 44, 6,  55,
                                          20, 3, 10, 43, 25, 39, 41, 45, 15,
                                          21, 8, 18, 2,  61, 56, 14};

  static_assert(std::size(RoundConstants) == kKeccakRoundCount);
  static_assert(std::size(Rotation) == kKeccakLaneCount);

  for (uint64_t RC : RoundConstants) {
    uint64_t C[kKeccakDimension], D[kKeccakDimension], B[kKeccakLaneCount];
    for (unsigned X = 0; X < kKeccakDimension; ++X)
      C[X] = State[X] ^ State[X + kKeccakDimension] ^
             State[X + 2 * kKeccakDimension] ^ State[X + 3 * kKeccakDimension] ^
             State[X + 4 * kKeccakDimension];
    for (unsigned X = 0; X < kKeccakDimension; ++X)
      D[X] = C[(X + kKeccakDimension - 1) % kKeccakDimension] ^
             llvm::rotl(C[(X + 1) % kKeccakDimension], 1);
    for (unsigned Y = 0; Y < kKeccakDimension; ++Y)
      for (unsigned X = 0; X < kKeccakDimension; ++X)
        State[X + kKeccakDimension * Y] ^= D[X];
    for (unsigned Y = 0; Y < kKeccakDimension; ++Y)
      for (unsigned X = 0; X < kKeccakDimension; ++X)
        B[Y + kKeccakDimension * ((2 * X + 3 * Y) % kKeccakDimension)] =
            llvm::rotl(State[X + kKeccakDimension * Y],
                       static_cast<int>(Rotation[X + kKeccakDimension * Y]));
    for (unsigned Y = 0; Y < kKeccakDimension; ++Y)
      for (unsigned X = 0; X < kKeccakDimension; ++X)
        State[X + kKeccakDimension * Y] =
            B[X + kKeccakDimension * Y] ^
            ((~B[(X + 1) % kKeccakDimension + kKeccakDimension * Y]) &
             B[(X + 2) % kKeccakDimension + kKeccakDimension * Y]);
    State[0] ^= RC;
  }
}

} // namespace

Keccak256Digest keccak256(llvm::ArrayRef<uint8_t> Data) {
  std::array<uint64_t, kKeccakLaneCount> State{};
  while (Data.size() >= kKeccak256RateBytes) {
    for (size_t I = 0; I < kKeccak256RateBytes; ++I)
      State[I / sizeof(uint64_t)] ^= static_cast<uint64_t>(Data[I])
                                     << ((I % sizeof(uint64_t)) * kBitsPerByte);
    keccakF1600(State);
    Data = Data.drop_front(kKeccak256RateBytes);
  }

  std::array<uint8_t, kKeccak256RateBytes> Last{};
  llvm::copy(Data, Last.begin());
  Last[Data.size()] = kKeccakDomainSeparator;
  Last[kKeccak256RateBytes - 1] |= kKeccakFinalBit;
  for (size_t I = 0; I < kKeccak256RateBytes; ++I)
    State[I / sizeof(uint64_t)] ^= static_cast<uint64_t>(Last[I])
                                   << ((I % sizeof(uint64_t)) * kBitsPerByte);
  keccakF1600(State);

  Keccak256Digest Digest{};
  for (size_t I = 0; I < kKeccak256DigestBytes; ++I)
    Digest[I] = static_cast<uint8_t>(State[I / sizeof(uint64_t)] >>
                                     ((I % sizeof(uint64_t)) * kBitsPerByte));
  return Digest;
}

Keccak256Digest keccak256(llvm::StringRef Text) {
  return keccak256(llvm::ArrayRef(
      reinterpret_cast<const uint8_t *>(Text.data()), Text.size()));
}

llvm::APInt keccak256Word(llvm::ArrayRef<uint8_t> Data) {
  const Keccak256Digest Digest = keccak256(Data);
  llvm::APInt Result(kWordBits, 0);
  for (uint8_t Byte : Digest) {
    Result <<= kBitsPerByte;
    Result |= Byte;
  }
  return Result;
}

uint32_t keccak256Selector(llvm::StringRef Text) {
  const Keccak256Digest Digest = keccak256(Text);
  uint32_t Selector = 0;
  for (unsigned I = 0; I < kSelectorBytes; ++I)
    Selector = (Selector << kBitsPerByte) | Digest[I];
  return Selector;
}

llvm::APInt keccak256Topic(llvm::StringRef Text) {
  const Keccak256Digest Digest = keccak256(Text);
  llvm::APInt Result(kWordBits, 0);
  for (uint8_t Byte : Digest) {
    Result <<= kBitsPerByte;
    Result |= Byte;
  }
  return Result;
}

} // namespace neverd::evm
