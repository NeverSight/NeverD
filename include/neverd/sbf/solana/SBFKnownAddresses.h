//===- SBFKnownAddresses.h - Well-known Solana addresses --------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Recognizes protocol and canonical-program addresses embedded in a program's
/// read-only data. A match annotates a constant with the name a developer would
/// recognize; it never changes decoded semantics.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_SBF_SOLANA_SBFKNOWNADDRESSES_H
#define NEVERD_SBF_SOLANA_SBFKNOWNADDRESSES_H

#include "neverd/sbf/solana/SBFPubkey.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>

namespace neverd::sbf {

enum class KnownAddressCategory : uint8_t {
#define SBF_KNOWN_ADDRESS_CATEGORY(ID, SPELLING) ID,
#include "neverd/sbf/solana/SBFKnownAddressCategories.def"
};

enum class KnownAddress : uint8_t {
#define SBF_KNOWN_ADDRESS(ID, NAME, ADDRESS, CATEGORY) ID,
#include "neverd/sbf/solana/SBFKnownAddresses.def"
  Unknown,
};

struct KnownAddressInfo {
  KnownAddress ID;
  llvm::StringLiteral Name;
  /// The base58 spelling exactly as the table records it.
  llvm::StringLiteral Text;
  KnownAddressCategory Category;
  Pubkey Key;
  /// False when the table's spelling is not a valid 32-byte address. Such an
  /// entry can never match; validateKnownAddressTable reports it.
  bool Decoded;
};

llvm::ArrayRef<KnownAddressInfo> knownAddressInfos();
const KnownAddressInfo *findKnownAddress(const Pubkey &Key);
const KnownAddressInfo *getKnownAddressInfo(KnownAddress ID);
llvm::StringRef knownAddressCategoryName(KnownAddressCategory Category);

/// Report every table entry whose base58 spelling does not decode to exactly
/// kPubkeyByteCount bytes, and every duplicated name or address.
llvm::Error validateKnownAddressTable();

} // namespace neverd::sbf

#endif // NEVERD_SBF_SOLANA_SBFKNOWNADDRESSES_H
