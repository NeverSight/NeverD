//===- StorageSlots.h - The storage slots a specification fixes -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines the dictionary of storage slots whose numbers a specification fixes
/// rather than a compiler allocates.
///
/// A compiler numbers a contract's own variables from zero, so a low slot means
/// nothing on its own. The slots here are the opposite: each is the hash of a
/// published string, so a program that touches one is saying which
/// specification it is speaking. That is what makes a proxy's implementation
/// address findable in a stripped binary.
///
/// Slots are derived from their preimages on first use, never transcribed, so a
/// match exhibits a preimage exactly the way a selector match does.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_EVM_STORAGESLOTS_H
#define NEVERD_EVM_STORAGESLOTS_H

#include "neverd/evm/ABI.h"

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

#include <cstdint>

namespace neverd::evm {

/// How a published preimage becomes a slot number.
enum class SlotDerivation : uint8_t {
#define EVM_SLOT_DERIVATION(ID, NAME, SUMMARY) ID,
#include "neverd/evm/EVMKnownSlots.def"
};

struct SlotDerivationInfo {
  SlotDerivation ID;
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
};

llvm::ArrayRef<SlotDerivationInfo> slotDerivationInfos();
llvm::StringRef slotDerivationName(SlotDerivation Derivation);

/// The slot \p Derivation produces from \p Preimage.
llvm::APInt deriveSlot(SlotDerivation Derivation, llvm::StringRef Preimage);

enum class KnownSlot : uint8_t {
#define EVM_KNOWN_SLOT(ID, PREIMAGE, DERIVATION, STANDARD, NAME, SUMMARY) ID,
#include "neverd/evm/EVMKnownSlots.def"
};

/// One tabulated slot together with the number its preimage derives to.
struct KnownSlotInfo {
  KnownSlot ID = KnownSlot::ERC1967Implementation;
  KnownStandard Standard = KnownStandard::Common;
  SlotDerivation Derivation = SlotDerivation::Keccak;
  /// The string the specification publishes.
  llvm::StringLiteral Preimage;
  /// A stable, dotted name for the slot, used wherever a recovered access is
  /// reported.
  llvm::StringLiteral Name;
  llvm::StringLiteral Summary;
  /// What \c Derivation makes of \c Preimage.
  llvm::APInt Slot;
};

llvm::ArrayRef<KnownSlotInfo> knownSlotInfos();
const KnownSlotInfo &getKnownSlotInfo(KnownSlot ID);

/// The tabulated slot equal to \p Slot, or null when no specification fixes
/// that number.
const KnownSlotInfo *findKnownSlot(const llvm::APInt &Slot);

/// Report two entries that derive to one slot, which would make the surviving
/// name depend on table order.
llvm::Error validateKnownSlotTable();

} // namespace neverd::evm

#endif // NEVERD_EVM_STORAGESLOTS_H
