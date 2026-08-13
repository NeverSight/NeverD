//===- SBFKnownAddresses.cpp - Well-known Solana addresses ----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/sbf/solana/SBFKnownAddresses.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/Twine.h"

#include <array>
#include <vector>

namespace neverd::sbf {
namespace {

KnownAddressInfo makeInfo(KnownAddress ID, llvm::StringLiteral Name,
                          llvm::StringLiteral Text,
                          KnownAddressCategory Category) {
  KnownAddressInfo Info{ID, Name, Text, Category, Pubkey{}, false};
  if (llvm::Expected<Pubkey> Key = parsePubkey(Text)) {
    Info.Key = *Key;
    Info.Decoded = true;
  } else {
    llvm::consumeError(Key.takeError());
  }
  return Info;
}

} // namespace

llvm::ArrayRef<KnownAddressInfo> knownAddressInfos() {
  static const std::vector<KnownAddressInfo> Table = {
#define SBF_KNOWN_ADDRESS(ID, NAME, ADDRESS, CATEGORY)                         \
  makeInfo(KnownAddress::ID, NAME, ADDRESS, KnownAddressCategory::CATEGORY),
#include "neverd/sbf/solana/SBFKnownAddresses.def"
  };
  return Table;
}

const KnownAddressInfo *findKnownAddress(const Pubkey &Key) {
  for (const KnownAddressInfo &Info : knownAddressInfos())
    if (Info.Decoded && Info.Key == Key)
      return &Info;
  return nullptr;
}

const KnownAddressInfo *getKnownAddressInfo(KnownAddress ID) {
  for (const KnownAddressInfo &Info : knownAddressInfos())
    if (Info.ID == ID)
      return &Info;
  return nullptr;
}

llvm::StringRef knownAddressCategoryName(KnownAddressCategory Category) {
  switch (Category) {
#define SBF_KNOWN_ADDRESS_CATEGORY(ID, SPELLING)                               \
  case KnownAddressCategory::ID:                                               \
    return SPELLING;
#include "neverd/sbf/solana/SBFKnownAddressCategories.def"
  }
  return "unknown";
}

llvm::Error validateKnownAddressTable() {
  llvm::StringSet<> Names;
  std::vector<Pubkey> Seen;
  for (const KnownAddressInfo &Info : knownAddressInfos()) {
    if (!Info.Decoded)
      return llvm::make_error<llvm::StringError>(
          ("sbf: known address '" + Info.Name + "' has spelling '" + Info.Text +
           "', which is not a " + llvm::Twine(kPubkeyByteCount) +
           "-byte base58 address")
              .str(),
          llvm::inconvertibleErrorCode());
    if (!Names.insert(Info.Name).second)
      return llvm::make_error<llvm::StringError>(
          ("sbf: duplicate known address name '" + Info.Name + "'").str(),
          llvm::inconvertibleErrorCode());
    if (llvm::is_contained(Seen, Info.Key))
      return llvm::make_error<llvm::StringError>(
          ("sbf: duplicate known address '" + Info.Text + "'").str(),
          llvm::inconvertibleErrorCode());
    Seen.push_back(Info.Key);
  }
  return llvm::Error::success();
}

} // namespace neverd::sbf
