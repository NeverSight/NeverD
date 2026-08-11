//===- StorageSlots.cpp - The storage slots a specification fixes -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/evm/StorageSlots.h"

#include "Keccak.h"

#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>
#include <array>
#include <vector>

namespace neverd::evm {
namespace {

llvm::Error slotError(llvm::Twine Message) {
  return llvm::make_error<llvm::StringError>(
      ("evm: known slot: " + Message).str(), llvm::inconvertibleErrorCode());
}

/// \p Value as the thirty-two big-endian bytes `abi.encode(uint256)` produces.
std::array<uint8_t, kWordBytes> encodeWord(const llvm::APInt &Value) {
  std::array<uint8_t, kWordBytes> Encoded{};
  for (unsigned I = 0; I < kWordBytes; ++I)
    Encoded[kWordBytes - 1 - I] = static_cast<uint8_t>(
        Value.extractBitsAsZExtValue(kBitsPerByte, I * kBitsPerByte));
  return Encoded;
}

} // namespace

llvm::ArrayRef<SlotDerivationInfo> slotDerivationInfos() {
  static const std::array Table = {
#define EVM_SLOT_DERIVATION(ID, NAME, SUMMARY)                                 \
  SlotDerivationInfo{SlotDerivation::ID, NAME, SUMMARY},
#include "neverd/evm/EVMKnownSlots.def"
  };
  return Table;
}

llvm::StringRef slotDerivationName(SlotDerivation Derivation) {
  return slotDerivationInfos()[static_cast<size_t>(Derivation)].Name;
}

llvm::APInt deriveSlot(SlotDerivation Derivation, llvm::StringRef Preimage) {
  const llvm::APInt Digest = keccak256Topic(Preimage);
  switch (Derivation) {
  case SlotDerivation::Keccak:
    return Digest;
  case SlotDerivation::KeccakMinusOne:
    return Digest - 1;
  case SlotDerivation::ERC7201:
    // The namespace formula hashes the encoding of the decremented digest and
    // then clears the low byte, so a namespace owns an aligned run of slots
    // and its base can never collide with a compiler-allocated one.
    return keccak256Word(encodeWord(Digest - 1)) &
           ~llvm::APInt::getLowBitsSet(kWordBits, kBitsPerByte);
  }
  llvm_unreachable("invalid slot derivation");
}

llvm::ArrayRef<KnownSlotInfo> knownSlotInfos() {
  // Deriving each slot here rather than writing it down is what makes a match
  // meaningful: the entry cannot claim a number its preimage does not hash to.
  static const std::vector<KnownSlotInfo> Table = [] {
    std::vector<KnownSlotInfo> Entries;
    const auto Add = [&](KnownSlot ID, KnownStandard Standard,
                         SlotDerivation Derivation, llvm::StringLiteral Preimage,
                         llvm::StringLiteral Name, llvm::StringLiteral Summary) {
      Entries.push_back(KnownSlotInfo{ID, Standard, Derivation, Preimage, Name,
                                      Summary,
                                      deriveSlot(Derivation, Preimage)});
    };
#define EVM_KNOWN_SLOT(ID, PREIMAGE, DERIVATION, STANDARD, NAME, SUMMARY)      \
  Add(KnownSlot::ID, KnownStandard::STANDARD, SlotDerivation::DERIVATION,      \
      PREIMAGE, NAME, SUMMARY);
#include "neverd/evm/EVMKnownSlots.def"
    return Entries;
  }();
  return Table;
}

const KnownSlotInfo &getKnownSlotInfo(KnownSlot ID) {
  return knownSlotInfos()[static_cast<size_t>(ID)];
}

const KnownSlotInfo *findKnownSlot(const llvm::APInt &Slot) {
  if (Slot.getBitWidth() != kWordBits)
    return nullptr;
  static const std::vector<const KnownSlotInfo *> Sorted = [] {
    std::vector<const KnownSlotInfo *> Order;
    for (const KnownSlotInfo &Info : knownSlotInfos())
      Order.push_back(&Info);
    llvm::sort(Order, [](const KnownSlotInfo *LHS, const KnownSlotInfo *RHS) {
      return LHS->Slot.ult(RHS->Slot);
    });
    return Order;
  }();

  const auto It =
      std::lower_bound(Sorted.begin(), Sorted.end(), Slot,
                       [](const KnownSlotInfo *Info, const llvm::APInt &Value) {
                         return Info->Slot.ult(Value);
                       });
  return It != Sorted.end() && (*It)->Slot == Slot ? *It : nullptr;
}

llvm::Error validateKnownSlotTable() {
  llvm::DenseSet<llvm::StringRef> Names;
  for (const KnownSlotInfo &Info : knownSlotInfos()) {
    if (Info.Preimage.empty() || Info.Name.empty())
      return slotError("'" + Info.Name + "' is incompletely described");
    if (!Names.insert(Info.Name).second)
      return slotError("'" + Info.Name + "' is listed twice");
    // Two entries deriving to one number would make the surviving name depend
    // on table order.
    if (findKnownSlot(Info.Slot) != &Info)
      return slotError("'" + Info.Preimage +
                       "' is not the single slot its preimage derives to");
  }
  return llvm::Error::success();
}

} // namespace neverd::evm
