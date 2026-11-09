//===- NeverDCAPISwitches.cpp - C API: switch jump tables -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// The jump tables whole-program analysis recovered, as a listing names and
/// lays them out: the dispatch, the table, how each slot stores its target
/// and the case each target serves.
///
//===----------------------------------------------------------------------===//

#include "JSONText.h"
#include "SessionImpl.h"

#include "neverd/ir/low/LowIR.h"
#include "neverd/sdk/Limits.h"

#include "llvm/Support/JSON.h"
#include "llvm/Support/MathExtras.h"

#include <algorithm>
#include <optional>

using namespace neverd;
using namespace neverd::sdk;

namespace {

/// The most functions one page covers.

/// How every slot of a table stores its target.
enum class EntryForm { Absolute, TableRelative, ImageRelative };

const char *entryFormName(EntryForm Form) {
  switch (Form) {
  case EntryForm::Absolute:
    return "absolute";
  case EntryForm::TableRelative:
    return "table_relative";
  case EntryForm::ImageRelative:
    return "image_relative";
  }
  return "";
}

/// The \p Size bytes at \p Address as an unsigned little-endian value, when
/// the image stores them.
std::optional<uint64_t> storedValue(const BinaryImage &Img, va_t Address,
                                    unsigned Size) {
  const Segment *Seg = Img.getSegmentFor(Address);
  if (!Seg || Size == 0 || Size > 8 || Address - Seg->VA > Seg->Data.size() ||
      Seg->Data.size() - (Address - Seg->VA) < Size)
    return std::nullopt;
  uint64_t Value = 0;
  for (unsigned I = 0; I < Size; ++I)
    Value |= uint64_t(Seg->Data[Address - Seg->VA + I]) << (8 * I);
  return Value;
}

/// The value physical slot \p Slot of the table stores, as the dispatch reads
/// it: at the table's address plus the slot times its stride, in storage the
/// table owns other than \p Except.
std::optional<uint64_t>
slotValue(const BinaryImage &Img, const JumpTable &Table, uint64_t Slot,
          const JumpTableStorageRange *Except = nullptr) {
  if (!Table.HasBaseAddr || Table.EntrySize == 0 || Table.EntrySize > 8 ||
      Table.EntryStride < Table.EntrySize ||
      Slot > (InvalidVA - Table.BaseAddr) / Table.EntryStride)
    return std::nullopt;
  const va_t Address = Table.BaseAddr + Slot * Table.EntryStride;
  if (!std::any_of(Table.StorageRanges.begin(), Table.StorageRanges.end(),
                   [&](const JumpTableStorageRange &Range) {
                     return (!Except || !(Range == *Except)) &&
                            Range.ownsStorageAddress(Address);
                   }))
    return std::nullopt;
  auto Value = storedValue(Img, Address, Table.EntrySize);
  if (Value && Table.IsSigned && Table.EntrySize < 8)
    Value =
        static_cast<uint64_t>(llvm::SignExtend64(*Value, Table.EntrySize * 8));
  return Value;
}

/// The form the table's encoding names, and the address its entries add to.
std::optional<std::pair<EntryForm, uint64_t>>
encodingForm(const BinaryImage &Img, const JumpTable &Table) {
  if (Table.IsPEImageRelativeRVA)
    return std::pair(EntryForm::ImageRelative, Img.Base);
  // A compact table scales its entries from another anchor.
  if (Table.HasTargetBase)
    return std::nullopt;
  if (Table.IsRelative)
    return std::pair(EntryForm::TableRelative, Table.BaseAddr);
  return std::pair(EntryForm::Absolute, uint64_t{0});
}

/// The form the table's encoding names, when every slot that dispatches to
/// a target stores exactly that target in it.
std::optional<EntryForm> entryForm(const BinaryImage &Img,
                                   const JumpTable &Table) {
  if (!Table.HasDispatchSlotMap ||
      Table.SlotIndices.size() != Table.Targets.size() ||
      Table.Targets.empty() || Table.TwoTableSelect || Table.TwoLevelIndex ||
      Table.MutatedUnsafe || Table.PreScaledIndex)
    return std::nullopt;
  const auto Encoding = encodingForm(Img, Table);
  if (!Encoding)
    return std::nullopt;
  for (size_t K = 0; K < Table.Targets.size(); ++K) {
    const auto Value = slotValue(Img, Table, Table.SlotIndices[K]);
    if (!Value || Encoding->second + *Value != Table.Targets[K])
      return std::nullopt;
  }
  return Encoding->first;
}

/// A two-level table as the dispatch reads it: the index run's entry K holds
/// the slot of the address table that stores Targets[K] in Form.
struct TwoLevelLayout {
  EntryForm Form;
  JumpTableStorageRange Index;
  std::vector<uint64_t> Slots;
};

/// The layout of a two-level table, when every index entry selects an address
/// slot, outside the index run, that stores its case's target.  A clamped
/// dispatch, which replaces some entries, has none.
std::optional<TwoLevelLayout> twoLevelLayout(const BinaryImage &Img,
                                             const JumpTable &Table) {
  if (!Table.TwoLevelIndex || !Table.TwoLevelIndexRange ||
      Table.Targets.empty() || Table.TwoTableSelect || Table.MutatedUnsafe ||
      Table.PreScaledIndex)
    return std::nullopt;
  const JumpTableStorageRange &Index = *Table.TwoLevelIndexRange;
  if (Index.PhysicalSlotCount != Table.Targets.size() || Index.EntrySize == 0 ||
      Index.EntrySize > 4 || Index.EntryStride != Index.EntrySize ||
      !Index.storageEnd() ||
      std::find(Table.StorageRanges.begin(), Table.StorageRanges.end(),
                Index) == Table.StorageRanges.end())
    return std::nullopt;
  const auto Encoding = encodingForm(Img, Table);
  if (!Encoding)
    return std::nullopt;
  TwoLevelLayout Layout{Encoding->first, Index, {}};
  Layout.Slots.reserve(Table.Targets.size());
  for (size_t K = 0; K < Table.Targets.size(); ++K) {
    const auto Slot = storedValue(Img, Index.BaseAddr + K * Index.EntryStride,
                                  Index.EntrySize);
    const auto Value =
        Slot ? slotValue(Img, Table, *Slot, &Index) : std::optional<uint64_t>();
    if (!Value || Encoding->second + *Value != Table.Targets[K])
      return std::nullopt;
    Layout.Slots.push_back(*Slot);
  }
  return Layout;
}

llvm::json::Value switchJson(const BinaryImage &Img, va_t Function,
                             const JumpTable &Table) {
  const auto address = [](bool Known, va_t Address) -> llvm::json::Value {
    return Known ? llvm::json::Value(vaHex(Address)) : nullptr;
  };
  llvm::json::Object Object;
  Object["function"] = vaHex(Function);
  Object["jump"] = vaHex(Table.InsnAddr);
  Object["load"] =
      address(Table.TableLoadAddr != InvalidVA, Table.TableLoadAddr);
  Object["table"] = address(Table.HasBaseAddr, Table.BaseAddr);
  Object["entry_size"] = static_cast<int64_t>(Table.EntrySize);
  Object["stride"] = static_cast<int64_t>(Table.EntryStride);
  llvm::json::Array Storage;
  for (const JumpTableStorageRange &Run : Table.StorageRanges)
    Storage.push_back(llvm::json::Array{
        vaHex(Run.BaseAddr), static_cast<int64_t>(Run.EntrySize),
        static_cast<int64_t>(Run.EntryStride),
        static_cast<int64_t>(Run.PhysicalSlotCount)});
  Object["storage"] = std::move(Storage);
  const auto TwoLevel = twoLevelLayout(Img, Table);
  const auto Form =
      TwoLevel ? std::optional(TwoLevel->Form) : entryForm(Img, Table);
  Object["form"] = Form ? llvm::json::Value(entryFormName(*Form)) : nullptr;
  Object["index"] =
      TwoLevel
          ? llvm::json::Value(llvm::json::Object{
                {"table", vaHex(TwoLevel->Index.BaseAddr)},
                {"entry_size", static_cast<int64_t>(TwoLevel->Index.EntrySize)},
                {"stride", static_cast<int64_t>(TwoLevel->Index.EntryStride)},
                {"entries",
                 static_cast<int64_t>(TwoLevel->Index.PhysicalSlotCount)}})
          : llvm::json::Value(nullptr);
  const bool Slots = Table.HasDispatchSlotMap &&
                     Table.SlotIndices.size() == Table.Targets.size();
  llvm::json::Array Targets;
  for (size_t K = 0; K < Table.Targets.size(); ++K)
    Targets.push_back(llvm::json::Array{
        vaHex(Table.Targets[K]), Table.caseLabel(K),
        TwoLevel ? llvm::json::Value(static_cast<int64_t>(TwoLevel->Slots[K]))
        : Slots  ? llvm::json::Value(static_cast<int64_t>(Table.SlotIndices[K]))
                 : nullptr});
  Object["targets"] = std::move(Targets);
  return llvm::json::Value(std::move(Object));
}

} // namespace

const char *neverd_switches_json(neverd_session_t Sess, neverd_va_t FirstEntry,
                                 int MaxFunctions) {
  auto *S = toSession(Sess);
  if (!S)
    return nullptr;
  S->clearError();
  if (!S->Loaded) {
    S->setError("no binary loaded");
    return nullptr;
  }
  // A pipeline restricted to some functions arbitrated their tables alone.
  if (!S->PipeRan || !S->PipeResult.Success ||
      !S->OnlyFunctionEntries.empty()) {
    S->setError("switch tables are published after whole-program analysis");
    return nullptr;
  }
  const size_t Limit = static_cast<size_t>(
      std::clamp(MaxFunctions, 1, kMaxSwitchFunctionsPerPage));
  std::vector<const LowFunc *> Functions;
  Functions.reserve(S->PipeResult.LowFuncs.size());
  for (const LowFunc &F : S->PipeResult.LowFuncs)
    if (F.Entry >= FirstEntry)
      Functions.push_back(&F);
  std::sort(
      Functions.begin(), Functions.end(),
      [](const LowFunc *A, const LowFunc *B) { return A->Entry < B->Entry; });
  llvm::json::Array Switches;
  for (size_t I = 0; I < std::min(Limit, Functions.size()); ++I)
    for (const JumpTable &Table : Functions[I]->JumpTables)
      Switches.push_back(switchJson(S->Img, Functions[I]->Entry, Table));
  llvm::json::Object Result;
  Result["switches"] = std::move(Switches);
  Result["next_entry"] = Functions.size() > Limit
                             ? llvm::json::Value(vaHex(Functions[Limit]->Entry))
                             : nullptr;
  return dupStr(jsonToString(llvm::json::Value(std::move(Result))));
}
