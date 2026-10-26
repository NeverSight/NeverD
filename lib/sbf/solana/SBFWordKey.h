//===- SBFWordKey.h - Full-domain keys for Solana byte evidence -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Represents arbitrary binary words without colliding with DenseMap's empty
/// and tombstone keys. Both all-ones words are valid discriminator/key bytes.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SOLANA_SBFWORDKEY_H
#define NEVERD_SBF_SOLANA_SBFWORDKEY_H

#include "llvm/ADT/DenseMapInfo.h"

#include <cstdint>
#include <utility>

namespace neverd::sbf {

/// The second lane distinguishes every real word from the pair sentinels,
/// whose two lanes are both reserved. No binary word is excluded or rewritten.
using WordKey = std::pair<uint64_t, uint8_t>;

inline WordKey wordKey(uint64_t Word) { return {Word, 0}; }

} // namespace neverd::sbf

#endif // NEVERD_SBF_SOLANA_SBFWORDKEY_H
