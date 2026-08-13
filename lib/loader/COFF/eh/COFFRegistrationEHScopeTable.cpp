//===- COFFRegistrationEHScopeTable.cpp - x86-32 SEH scope tables --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace neverd::coff_loader::registration_detail {
namespace {

/// True when a scope-table entry names a level that already exists.  Levels
/// are indices into the same table, so a forward or out-of-range reference
/// would make the nesting graph cyclic or dangling.
bool isValidEnclosingLevel(int32_t Level, uint32_t Index, bool IsEH4) {
  if (Level == -1)
    return true;
  if (IsEH4 && Level == -2)
    return true;
  return Level >= 0 && static_cast<uint32_t>(Level) < Index;
}

/// One `mov dword ptr [ebp+disp], imm32`, the only shape the compiler uses to
/// set the current try level.
struct FrameSlotStore {
  va_t StoreVA = 0;
  va_t EndVA = 0;
  int32_t Displacement = 0;
  int32_t Value = 0;
};

/// Every such store inside a code range.
///
/// This is a byte scan rather than a decode, so it can also match bytes that
/// are the tail of some other instruction.  Nothing downstream trusts a hit on
/// its own: a slot is only believed once the whole set of stores into it reads
/// as a try-level sequence, which arbitrary bytes do not.
std::vector<FrameSlotStore>
findFrameSlotStores(const BinaryImage &Img,
                    const ExceptionAddressRange &Range) {
  std::vector<FrameSlotStore> Stores;
  const Segment *Seg = Img.getSegmentFor(Range.Begin);
  if (!Seg || !Seg->isExecutable() || Range.Begin < Seg->VA ||
      Range.End <= Range.Begin)
    return Stores;
  const uint64_t Begin = Range.Begin - Seg->VA;
  const uint64_t End =
      std::min<uint64_t>(Range.End - Seg->VA, Seg->Data.size());
  if (Begin >= End)
    return Stores;

  const uint8_t *Data = Seg->Data.data();
  for (uint64_t I = Begin; I + 7 <= End; ++I) {
    if (Data[I] != 0xC7)
      continue;
    // ModRM /0 with a base of EBP: mod=01 is the byte displacement and mod=10
    // the dword one.  Both take a trailing imm32.
    if (Data[I + 1] == 0x45) {
      Stores.push_back(
          {static_cast<va_t>(Seg->VA + I), static_cast<va_t>(Seg->VA + I + 7),
           static_cast<int8_t>(Data[I + 2]),
           static_cast<int32_t>(readLE<uint32_t>(Data + I + 3))});
    } else if (Data[I + 1] == 0x85 && I + 10 <= End) {
      Stores.push_back(
          {static_cast<va_t>(Seg->VA + I), static_cast<va_t>(Seg->VA + I + 10),
           static_cast<int32_t>(readLE<uint32_t>(Data + I + 2)),
           static_cast<int32_t>(readLE<uint32_t>(Data + I + 6))});
    }
  }
  return Stores;
}

} // namespace

/// Decode the scope-table entry array.
///
/// The array is unsized: nothing in the image records how many entries a table
/// has, because the runtime only ever indexes it by the try level held in the
/// frame.  Validating entries until one fails is therefore not enough on its
/// own — the compiler emits these tables back to back, so the first entry of
/// the *next* function's table is a perfectly well-formed entry and a walk
/// that only checks well-formedness runs straight into it, attributing another
/// function's handlers to this one.
///
/// \p Limit is the address the next table begins at, which caps the walk at
/// the one boundary the image does establish.  It is zero for the last table
/// in the image, where validation is all there is.
uint32_t decodeScopeRecords(const BinaryImage &Img, va_t ArrayVA, va_t Limit,
                            bool IsEH4,
                            std::vector<RegistrationScopeRecord> &Scopes) {
  for (uint32_t Index = 0; Index < MaxRegistrationRecords; ++Index) {
    uint64_t Offset = uint64_t(Index) * 12;
    if (Offset > InvalidVA - ArrayVA)
      break;
    if (Limit != 0 && ArrayVA + Offset + 12 > Limit)
      break;
    const uint8_t *Entry = Img.readVA(ArrayVA + Offset, 12);
    if (!Entry)
      break;
    int32_t Level = readLE<int32_t>(Entry);
    uint32_t Filter = readLE<uint32_t>(Entry + 4);
    uint32_t Handler = readLE<uint32_t>(Entry + 8);
    if (!isValidEnclosingLevel(Level, Index, IsEH4))
      break;
    // A `__finally` has no filter; an `__except` has both.  An entry with no
    // handler at all describes nothing and marks the end of the array.
    if (Handler == 0 || !isExecutableAddress(Img, Handler))
      break;
    if (Filter != 0 && !isExecutableAddress(Img, Filter))
      break;

    RegistrationScopeRecord Scope;
    Scope.EnclosingLevel = Level;
    Scope.FilterVA = Filter;
    Scope.HandlerVA = Handler;
    Scope.IsFinally = Filter == 0;
    Scopes.push_back(Scope);
  }
  return static_cast<uint32_t>(Scopes.size());
}

/// Prove which frame slot holds the current try level, and keep the stores
/// into it.
///
/// The scope table is indexed by a level the runtime reads out of the frame,
/// so the table alone never says which code each scope guards — only the
/// stores do.  Which slot holds the level is not recorded anywhere either, so
/// it has to be proven, and the table itself supplies the vocabulary to prove
/// it with: a try-level slot only ever receives the seed the prologue pushed
/// or the index of a scope the table declares.  A frame slot qualifies when
/// every store into it is one of those values, the seed is among them, and so
/// is at least one real scope index.  Ordinary locals fail on the first count
/// by holding something outside the range and on the second by never being set
/// to the seed.  When more than one slot survives, nothing was proven and no
/// ranges are published.
void recoverTryLevelStores(const BinaryImage &Img,
                           const ExceptionAddressRange &Range, int32_t Seed,
                           size_t ScopeCount, RegistrationChainInfo &Chain) {
  if (ScopeCount == 0 || ScopeCount > MaxRegistrationRecords)
    return;
  const int32_t Highest = static_cast<int32_t>(ScopeCount) - 1;

  std::map<int32_t, std::vector<FrameSlotStore>> BySlot;
  for (const FrameSlotStore &Store : findFrameSlotStores(Img, Range)) {
    // The try level lives in the frame the prologue established, which is
    // below the frame pointer.  A positive displacement addresses an incoming
    // argument and cannot be it.
    if (Store.Displacement < 0)
      BySlot[Store.Displacement].push_back(Store);
  }

  const std::vector<FrameSlotStore> *Winner = nullptr;
  int32_t WinningSlot = 0;
  for (const auto &[Slot, Stores] : BySlot) {
    bool SawSeed = false;
    bool SawScope = false;
    bool AllInRange = true;
    for (const FrameSlotStore &Store : Stores) {
      if (Store.Value == Seed)
        SawSeed = true;
      else if (Store.Value >= 0 && Store.Value <= Highest)
        SawScope = true;
      else
        AllInRange = false;
    }
    if (!AllInRange || !SawSeed || !SawScope)
      continue;
    if (Winner)
      return;
    Winner = &Stores;
    WinningSlot = Slot;
  }
  if (!Winner)
    return;

  Chain.TryLevelOffset = WinningSlot;
  Chain.TryLevelStores.reserve(Winner->size());
  for (const FrameSlotStore &Store : *Winner)
    Chain.TryLevelStores.push_back({Store.StoreVA, Store.EndVA, Store.Value});
  std::sort(Chain.TryLevelStores.begin(), Chain.TryLevelStores.end(),
            [](const RegistrationTryLevelStore &A,
               const RegistrationTryLevelStore &B) {
              return A.StoreVA < B.StoreVA;
            });
}

/// `_except_handler4` prefixes the entry array with the frame displacements of
/// the security cookies it verifies before trusting the table.  A `-2` cookie
/// offset is the sentinel for "this frame has no cookie of that kind".
bool decodeEH4Header(const BinaryImage &Img, va_t TableVA,
                     RegistrationChainInfo &Chain) {
  const uint8_t *Header = Img.readVA(TableVA, 16);
  if (!Header)
    return false;
  Chain.GSCookieOffset = readLE<int32_t>(Header);
  Chain.GSCookieXOROffset = readLE<int32_t>(Header + 4);
  Chain.EHCookieOffset = readLE<int32_t>(Header + 8);
  Chain.EHCookieXOROffset = readLE<int32_t>(Header + 12);
  Chain.HasSecurityCookies = Chain.GSCookieOffset != -2;
  return true;
}

} // namespace neverd::coff_loader::registration_detail
