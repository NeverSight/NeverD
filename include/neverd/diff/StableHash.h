//===- StableHash.h - Hashes that do not depend on the host -----*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The 64-bit hashes the comparison keys features by.  They depend only on
/// the values hashed, never on the platform, the process or the order
/// threads run in, so two runs of a comparison, or a comparison saved on
/// one machine and checked on another, agree.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_DIFF_STABLEHASH_H
#define NEVERD_DIFF_STABLEHASH_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"

#include <cstdint>

namespace neverd::diff {

/// The splitmix64 finalizer: a bijection that spreads every input bit.
inline uint64_t mix64(uint64_t X) {
  X ^= X >> 30;
  X *= 0xbf58476d1ce4e5b9ULL;
  X ^= X >> 27;
  X *= 0x94d049bb133111ebULL;
  X ^= X >> 31;
  return X;
}

/// Accumulates an ordered sequence of values.
class StableHash {
public:
  explicit StableHash(uint64_t Seed = 0x6a09e667f3bcc909ULL) : State(Seed) {}

  StableHash &add(uint64_t Value) {
    State = mix64(
        State ^ (Value + 0x9e3779b97f4a7c15ULL + (State << 6) + (State >> 2)));
    ++Count;
    return *this;
  }
  StableHash &add(llvm::StringRef Text) {
    add(Text.size());
    uint64_t Word = 0;
    unsigned Bytes = 0;
    for (unsigned char C : Text) {
      Word |= static_cast<uint64_t>(C) << (8 * Bytes);
      if (++Bytes == 8) {
        add(Word);
        Word = 0;
        Bytes = 0;
      }
    }
    if (Bytes)
      add(Word);
    return *this;
  }
  StableHash &add(llvm::ArrayRef<uint8_t> Bytes) {
    return add(llvm::StringRef(reinterpret_cast<const char *>(Bytes.data()),
                               Bytes.size()));
  }

  /// The hash, never zero: zero means "no feature" to the comparison.
  uint64_t value() const {
    const uint64_t V = mix64(State ^ Count);
    return V ? V : 1;
  }

private:
  uint64_t State;
  uint64_t Count = 0;
};

/// The hash of one string.
inline uint64_t hashText(llvm::StringRef Text) {
  return StableHash().add(Text).value();
}

/// A nonzero odd multiplier standing for \p Value in a product that is the
/// same whatever order its factors come in: the odd residues modulo 2^64
/// form a group, so a product of them is never zero and rarely collides.
inline uint64_t oddFactor(uint64_t Value) {
  return mix64(Value ^ 0x3c6ef372fe94f82bULL) | 1;
}

} // namespace neverd::diff

#endif // NEVERD_DIFF_STABLEHASH_H
