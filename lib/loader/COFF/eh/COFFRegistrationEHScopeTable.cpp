//===- COFFRegistrationEHScopeTable.cpp - x86-32 SEH scope tables --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

/// \file
/// Recover SEH scope records and revalidate their typed immutable storage.
//===----------------------------------------------------------------------===//

#include "COFFRegistrationEHDetail.h"

#include "neverd/Limits.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFRegistrationEH.h"
#include "neverd/loader/ReadOnlyBytes.h"
#include "neverd/loader/RegistrationScopeGraph.h"
#include "neverd/support/BinaryEncoding.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

namespace neverd::coff_loader::registration_detail {
namespace {

/// One candidate store relative to a checked base. A narrow immediate is an
/// observation of written bits, not a complete runtime state.
struct FrameSlotStore {
  va_t StoreVA = 0;
  va_t EndVA = 0;
  int32_t Displacement = 0;
  int32_t Value = 0;
  uint8_t Width = 4;
};

/// Every such store inside a code range.
///
/// This is a byte scan rather than a decode, so it can also match bytes that
/// are the tail of some other instruction. The CFG owner must authenticate
/// each hit against its decoded instruction boundary and reaching value before
/// using it. Adjacency alone cannot prove a register value across control flow.
std::vector<FrameSlotStore>
findFrameSlotStores(const BinaryImage &Img, const ExceptionAddressRange &Range,
                    uint8_t BaseRegister, int32_t BaseOffset) {
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
  for (uint64_t I = Begin; I + 3 <= End; ++I) {
    uint64_t Opcode = I;
    uint8_t Width = 4;
    const bool RegisterZero = Data[Opcode] == 0x89;
    if (Data[Opcode] == 0x66) {
      ++Opcode;
      Width = 2;
    }
    if (Data[Opcode] == 0xC6 && Width == 4)
      Width = 1;
    else if (Data[Opcode] != 0xC7 && !RegisterZero)
      continue;
    // A memory MOV with the selected unindexed frame base and a displacement.
    const uint8_t ModRM = Data[Opcode + 1];
    const unsigned Mod = ModRM >> 6;
    const uint64_t DispBytes = Mod == 1 ? 1 : Mod == 2 ? 4 : 0;
    const uint64_t ImmediateBytes = RegisterZero ? 0 : Width;
    const uint64_t SIBBytes = BaseRegister == 4 ? 1 : 0;
    if ((!RegisterZero && (ModRM & 0x38)) || (ModRM & 7) != BaseRegister ||
        !DispBytes ||
        Opcode + 2 + SIBBytes + DispBytes + ImmediateBytes > End ||
        (SIBBytes && Data[Opcode + 2] != 0x24))
      continue;
    if (RegisterZero) {
      const unsigned Register = (ModRM >> 3) & 7;
      // Only observe an adjacent full-register self-XOR. Do not infer values
      // through other instructions, and do not let a cleared frame/stack base
      // describe a store into the installed registration frame.
      if (Register == 4 || Register == BaseRegister || I < Begin + 2 ||
          (Data[I - 2] != 0x31 && Data[I - 2] != 0x33) ||
          Data[I - 1] != uint8_t(0xc0 | (Register << 3) | Register) ||
          (I >= Begin + 3 && Data[I - 3] == 0x66))
        continue;
    }
    const uint64_t DisplacementAt = Opcode + 2 + SIBBytes;
    const uint64_t Immediate = DisplacementAt + DispBytes;
    const int64_t Displacement =
        int64_t(BaseOffset) + (DispBytes == 1
                                   ? int8_t(Data[DisplacementAt])
                                   : readLE<int32_t>(Data + DisplacementAt));
    if (Displacement < INT32_MIN || Displacement > INT32_MAX)
      continue;
    const int32_t Value = RegisterZero ? 0
                          : Width == 1 ? Data[Immediate]
                          : Width == 2 ? readLE<uint16_t>(Data + Immediate)
                                       : readLE<int32_t>(Data + Immediate);
    Stores.push_back({static_cast<va_t>(Seg->VA + I),
                      static_cast<va_t>(Seg->VA + Immediate + ImmediateBytes),
                      int32_t(Displacement), Value, Width});
    // Do not publish a second dword candidate inside the operand-size prefix.
    if (Opcode != I)
      I = Opcode;
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
                            std::vector<RegistrationScopeRecord> &Scopes,
                            bool &BudgetExhausted) {
  BudgetExhausted = false;
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
    if (!isValidRegistrationScopeLevel(Level, Index, IsEH4))
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
  if (Scopes.size() == MaxRegistrationRecords) {
    const uint64_t Bytes = uint64_t(MaxRegistrationRecords) * 12;
    BudgetExhausted =
        Bytes > InvalidVA - ArrayVA || Limit == 0 || ArrayVA + Bytes != Limit;
  }
  return static_cast<uint32_t>(Scopes.size());
}

/// Collect candidate stores in the prologue-authenticated state slot. For
/// other prologues retain a unique value-pattern observation for inspection;
/// that heuristic does not establish registration ownership or CFG states.
void recoverTryLevelStores(const BinaryImage &Img,
                           const ExceptionAddressRange &Range, int32_t Seed,
                           size_t ScopeCount, RegistrationChainInfo &Chain) {
  if (ScopeCount == 0 || ScopeCount > MaxRegistrationRecords)
    return;
  const int32_t Highest = static_cast<int32_t>(ScopeCount) - 1;

  std::map<int32_t, std::vector<FrameSlotStore>> BySlot;
  const uint8_t BaseRegister =
      Chain.RealignedFrame ? Chain.RealignedFrame->BaseRegister : 5;
  const int32_t BaseOffset =
      Chain.RealignedFrame ? Chain.RealignedFrame->BaseOffset : 0;
  for (const FrameSlotStore &Store :
       findFrameSlotStores(Img, Range, BaseRegister, BaseOffset)) {
    // The try level lives in the frame the prologue established, which is
    // below the frame pointer.  A positive displacement addresses an incoming
    // argument and cannot be it.
    if (Store.Displacement < 0)
      BySlot[Store.Displacement].push_back(Store);
  }

  const std::vector<FrameSlotStore> *Winner = nullptr;
  int32_t WinningSlot = 0;
  if (Chain.RegistrationOffset && Chain.TryLevelOffset) {
    auto It = BySlot.find(*Chain.TryLevelOffset);
    if (It != BySlot.end()) {
      Winner = &It->second;
      WinningSlot = It->first;
    }
  }
  // Legacy observations without an authenticated layout remain inspectable,
  // but cannot authorize CFG state recovery or native frame ownership.
  for (const auto &[Slot, Stores] : BySlot) {
    if (Chain.RegistrationOffset)
      break;
    bool SawSeed = false;
    bool SawScope = false;
    bool AllInRange = true;
    for (const FrameSlotStore &Store : Stores) {
      if (Store.Width != 4) {
        AllInRange = false;
        continue;
      }
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
    Chain.TryLevelStores.push_back(
        {Store.StoreVA, Store.EndVA, Store.Value, Store.Width});
  std::sort(
      Chain.TryLevelStores.begin(), Chain.TryLevelStores.end(),
      [](const RegistrationTryLevelStore &A,
         const RegistrationTryLevelStore &B) { return A.StoreVA < B.StoreVA; });
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

namespace neverd::coff_loader {
std::optional<ExceptionAddressRange>
getX86RegistrationSEHScopeTableRange(const ExceptionFunction &Function) {
  const bool EH3 =
      Function.Personality == ExceptionPersonality::ExceptHandler3 &&
      Function.Encoding == ExceptionEncoding::X86ScopeTableEH3;
  const bool EH4 =
      Function.Personality == ExceptionPersonality::ExceptHandler4 &&
      Function.Encoding == ExceptionEncoding::X86ScopeTableEH4;
  if ((!EH3 && !EH4) || !Function.Registration)
    return std::nullopt;
  const auto &Chain = *Function.Registration;
  if (!Chain.ScopeTableVA || Chain.ScopeTableVA > UINT32_MAX ||
      Chain.Scopes.empty() ||
      Chain.Scopes.size() > limits::kMaxRegistrationEHRecords)
    return std::nullopt;
  const uint64_t Bytes = uint64_t(Chain.Scopes.size()) * 12 + (EH4 ? 16 : 0);
  if (Bytes > uint64_t(UINT32_MAX) + 1 - Chain.ScopeTableVA)
    return std::nullopt;
  return ExceptionAddressRange{Chain.ScopeTableVA, Chain.ScopeTableVA + Bytes};
}

std::optional<std::vector<uint8_t>> getCheckedX86SEHScopeTableBytes(
    const BinaryImage &Img, const ExceptionFunction &Function, size_t &Work) {
  const auto Range = getX86RegistrationSEHScopeTableRange(Function);
  if (!Range || Img.Arch != Arch::X86 || Img.Bits != Bitness::Bits32 ||
      Img.Format != BinaryFormat::COFF || Img.IsRelocatable)
    return std::nullopt;
  auto Charge = [&](size_t Amount) {
    if (Amount > limits::kMaxRegistrationEHStateWork -
                     std::min(Work, limits::kMaxRegistrationEHStateWork)) {
      Work = limits::kMaxRegistrationEHStateWork;
      return false;
    }
    Work += Amount;
    return true;
  };
  const auto &Chain = *Function.Registration;
  const bool EH4 = Function.Personality == ExceptionPersonality::ExceptHandler4;
  if (!Charge(Chain.Scopes.size() + Range->size()) ||
      !hasValidRegistrationScopeGraph(Chain, EH4) ||
      Chain.SeededTryLevel != registrationOutsideTryLevel(EH4) ||
      Chain.HasSecurityCookies != (EH4 && Chain.GSCookieOffset != -2))
    return std::nullopt;
  std::vector<uint32_t> Words;
  Words.reserve(Range->size() / 4);
  if (EH4)
    Words = {uint32_t(Chain.GSCookieOffset), uint32_t(Chain.GSCookieXOROffset),
             uint32_t(Chain.EHCookieOffset), uint32_t(Chain.EHCookieXOROffset)};
  for (const auto &Scope : Chain.Scopes) {
    Words.push_back(uint32_t(Scope.EnclosingLevel));
    Words.push_back(uint32_t(Scope.FilterVA));
    Words.push_back(uint32_t(Scope.HandlerVA));
  }
  std::vector<uint8_t> Bytes(Range->size());
  for (size_t I = 0; I != Words.size(); ++I) {
    // The strongest read scans exact HIGHLOW multiplicity, source conflicts
    // and target conflicts. Charge all three inventories for every field.
    for (unsigned Scan = 0; Scan != 3; ++Scan) {
      for (size_t Count :
           {Img.BaseRelocations.size(), Img.Relocations.size(),
            Img.RuntimeCallablePointerSlots.size(),
            Img.CodePtrRelocSlots.size(), Img.DataPtrRelocSlots.size(),
            Img.DataPtrRelocTargetOwners.size(), Img.RelCodeRelocSlots.size(),
            Img.RelDataPtrRelocSlots.size(),
            Img.MachOResolvedChainedPointerSlots.size(),
            Img.ConflictingImportStorageSlots.size(), Img.ImportPtrSlots.size(),
            Img.ImportStorageSlots.size(), Img.DyldBindSlots.size(),
            Img.ObjCSourceReferences.size(),
            Img.DataAddressRelocOperands.size(),
            Img.CodeAddressRelocOperands.size()})
        if (!Charge(Count + 1))
          return std::nullopt;
      // Each of the three mapping queries performs nested owner lookups.
      for (unsigned Lookup = 0; Lookup != 8; ++Lookup)
        if (!Charge(Img.Sections.size()) || !Charge(Img.Segments.size()))
          return std::nullopt;
    }
    const va_t Address = Range->Begin + I * 4;
    if (const auto Target = Chain.scopePointerTarget(Address)) {
      if (*Target != Words[I] ||
          readImmutablePE32CodePointerStorage(Img, Address) != *Target)
        return std::nullopt;
    } else {
      const auto Scalar = readImmutableImageBytes(Img, Address, 4);
      if (!Scalar || readLE<uint32_t>(Scalar->data()) != Words[I])
        return std::nullopt;
    }
    writeLE<uint32_t>(Bytes.data() + I * 4, Words[I]);
  }
  return Bytes;
}
} // namespace neverd::coff_loader
