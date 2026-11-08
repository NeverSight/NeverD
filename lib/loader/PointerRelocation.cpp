//===- PointerRelocation.cpp - Absolute pointer relocations in bulk -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/PointerRelocation.h"

#include "llvm/ADT/STLExtras.h"

#include <algorithm>
#include <iterator>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

using Effect = AbsolutePointerRelocationEffect;
using Kind = Effect::Kind;

/// Insert \p Values, sorted and unique, into \p Set, each next to the one
/// before it.
template <typename SetType>
void insertInOrder(SetType &Set, llvm::ArrayRef<va_t> Values) {
  auto Hint = Set.end();
  for (va_t Value : Values)
    Hint = std::next(Set.insert(Hint, Value));
}

/// Record \p Field for \p Slot in \p Operands, next to \p Hint, as
/// applyAbsolutePointerRelocation does: an operand that already names the
/// same address is kept as it is.  Returns the hint for the next slot.
template <typename OperandMap>
typename OperandMap::iterator
recordOperand(OperandMap &Operands, typename OperandMap::iterator Hint,
              va_t Slot, const RelocatedAddressField &Field) {
  const auto It = Operands.try_emplace(Hint, Slot, Field);
  const RelocatedAddressField &Kept = It->second;
  if (Kept.EncodedValue != Field.EncodedValue ||
      Kept.TargetVA != Field.TargetVA || Kept.Width != Field.Width ||
      Kept.TargetOwnerVA != Field.TargetOwnerVA)
    It->second = Field;
  return std::next(It);
}

/// Sort \p Values and drop the duplicates.
void sortUnique(std::vector<va_t> &Values) {
  llvm::sort(Values);
  Values.erase(std::unique(Values.begin(), Values.end()), Values.end());
}

/// classifyPointerRelocationAddress, remembered for the ranges of addresses
/// over which it cannot change.
class AddressClassMemo {
public:
  explicit AddressClassMemo(const BinaryImage &Img)
      : Img(Img), Enabled(canRemember(Img)) {}

  PointerRelocationAddressClass operator()(va_t Addr) {
    for (const Remembered &R : Ranges)
      if (Addr >= R.Begin && Addr < R.End)
        return R.Class;
    const PointerRelocationAddressClass Class =
        classifyPointerRelocationAddress(Img, Addr);
    if (Enabled && Class.Mapped)
      remember(Addr, Class);
    return Class;
  }

private:
  struct Remembered {
    va_t Begin = 0;
    va_t End = 0;
    PointerRelocationAddressClass Class;
  };

  /// Whether an address's class is its section's (or, outside every
  /// section, its segment's): no ARM mapping symbol makes one address of a
  /// section data, and no two segments, nor two readable sections, overlap,
  /// so an address of one lies in no other.
  static bool canRemember(const BinaryImage &Img) {
    if (Img.Arch == Arch::ARM)
      return false;
    std::vector<std::pair<va_t, va_t>> Segments, Sections;
    for (const Segment &Seg : Img.Segments) {
      if (Seg.Size > InvalidVA - Seg.VA)
        return false;
      Segments.emplace_back(Seg.VA, Seg.VA + Seg.Size);
    }
    for (const Section &Sec : Img.Sections) {
      if (!Sec.isReadable())
        continue;
      if (Sec.Size > InvalidVA - Sec.VA)
        return false;
      Sections.emplace_back(Sec.VA, Sec.VA + Sec.Size);
    }
    return disjoint(Segments) && disjoint(Sections);
  }

  static bool disjoint(std::vector<std::pair<va_t, va_t>> &Ranges) {
    llvm::sort(Ranges);
    for (size_t I = 1; I < Ranges.size(); ++I)
      if (Ranges[I].first < Ranges[I - 1].second)
        return false;
    return true;
  }

  /// Remember \p Class for every address that shares \p Addr's section and
  /// segment, or, outside every section, its gap between sections.  In an
  /// executable segment an address that is not code in its own right is
  /// code only when something names it, one address at a time: it is not
  /// remembered.
  void remember(va_t Addr, const PointerRelocationAddressClass &Class) {
    const Segment *Seg = Img.getSegmentFor(Addr);
    if (!Seg || (Seg->isExecutable() && !Img.isCodeAddress(Addr)))
      return;
    va_t Begin = Seg->VA, End = Seg->VA + Seg->Size;
    if (const Section *Sec = Img.getSectionFor(Addr)) {
      Begin = std::max(Begin, Sec->VA);
      End = std::min(End, Sec->VA + Sec->Size);
    } else {
      for (const Section &Other : Img.Sections) {
        if (!Other.isReadable())
          continue;
        if (Other.VA + Other.Size <= Addr)
          Begin = std::max(Begin, Other.VA + Other.Size);
        else if (Other.VA > Addr)
          End = std::min(End, Other.VA);
      }
    }
    if (Begin <= Addr && Addr < End)
      Ranges.push_back({Begin, End, Class});
  }

  const BinaryImage &Img;
  const bool Enabled;
  std::vector<Remembered> Ranges;
};

/// The addresses the image's own metadata gives to code: function symbols and
/// their extents, runtime function records, known code ranges and the entry
/// point.
class CodeMetadata {
public:
  explicit CodeMetadata(const BinaryImage &Img) : Img(Img) {
    for (const Symbol &Sym : Img.Symbols) {
      if (!Sym.IsFunc)
        continue;
      const va_t Start = normalize(Sym.Addr);
      Starts.push_back(Start);
      if (Sym.Size != 0 && Sym.Size <= InvalidVA - Start)
        Ranges.emplace_back(Start, Start + Sym.Size);
    }
    for (va_t Start : Img.RuntimeFunctionAddrs)
      Starts.push_back(normalize(Start));
    for (va_t Start : Img.VerifiedFunctionEntries)
      Starts.push_back(normalize(Start));
    if (Img.Entry != 0)
      Starts.push_back(normalize(Img.Entry));
    for (const auto &[Begin, End] : Img.KnownCodeRanges)
      Ranges.emplace_back(Begin, End);
    for (const ExceptionFunction &Function : Img.ExceptionMetadata.Functions)
      Ranges.emplace_back(Function.CodeRange.Begin, Function.CodeRange.End);
    // A load limited to some functions keeps the rest of .pdata compact.
    for (const BinaryImage::COFFPDataRecord &Record : Img.COFFPDataRecords)
      if (Record.EndRVA <= InvalidVA - Img.Base)
        Ranges.emplace_back(Img.Base + Record.BeginRVA,
                            Img.Base + Record.EndRVA);
    sortUnique(Starts);
    // Overlapping and nested ranges merge into the code they cover together.
    llvm::erase_if(
        Ranges, [](const auto &Range) { return Range.first >= Range.second; });
    llvm::sort(Ranges);
    size_t Count = 0;
    for (const auto &Range : Ranges) {
      if (Count != 0 && Range.first <= Ranges[Count - 1].second)
        Ranges[Count - 1].second =
            std::max(Ranges[Count - 1].second, Range.second);
      else
        Ranges[Count++] = Range;
    }
    Ranges.resize(Count);
  }

  /// Whether the metadata gives \p Addr to code.
  bool claims(va_t Addr) const {
    const va_t Normalized = normalize(Addr);
    if (std::binary_search(Starts.begin(), Starts.end(), Normalized))
      return true;
    const auto It = std::upper_bound(
        Ranges.begin(), Ranges.end(), Normalized,
        [](va_t Address, const auto &Range) { return Address < Range.first; });
    return It != Ranges.begin() && Normalized < std::prev(It)->second;
  }

  /// Whether code the metadata gives ends at \p Addr.
  bool endsAt(va_t Addr) const {
    const va_t Normalized = normalize(Addr);
    const auto It = std::lower_bound(
        Ranges.begin(), Ranges.end(), Normalized,
        [](const auto &Range, va_t Address) { return Range.second < Address; });
    return It != Ranges.end() && It->second == Normalized;
  }

private:
  va_t normalize(va_t Addr) const {
    return normalizeCodeAddress(Addr, Img.Arch, Img.Mode);
  }

  const BinaryImage &Img;
  std::vector<va_t> Starts;
  std::vector<std::pair<va_t, va_t>> Ranges;
};

} // namespace

void neverd::recordAbsolutePointerRelocations(
    BinaryImage &Img, llvm::ArrayRef<AbsolutePointerRelocation> Relocations) {
  // What a relocation does depends only on where its slot and target lie,
  // never on what the relocations before it recorded, so every effect is
  // decided before any is applied.
  AddressClassMemo Classify(Img);
  std::vector<Effect> Slots;
  std::vector<va_t> CodeTargets, DataTargets, WritableDataTargets;
  for (const AbsolutePointerRelocation &R : Relocations) {
    const Effect E = decideAbsolutePointerRelocation(Img, R.SlotVA, R.TargetVA,
                                                     R.TargetOwnerVA, Classify);
    switch (E.What) {
    case Kind::None:
      continue;
    case Kind::CodeOperandToCode:
      CodeTargets.push_back(E.CodeTargetVA);
      break;
    case Kind::DataSlotToCode:
      break;
    case Kind::CodeOperandToData:
    case Kind::DataSlotToData:
    case Kind::DataTargetOnly:
      (E.TargetWritable ? WritableDataTargets : DataTargets)
          .push_back(E.TargetVA);
      break;
    }
    if (E.What != Kind::DataTargetOnly)
      Slots.push_back(E);
  }

  // Every relocation adds its target.  A slot relocated once ends as that
  // relocation leaves it, however the other slots end, so those are applied
  // in bulk below; a slot relocated more than once -- which a PE's base
  // relocations never are -- is applied relocation by relocation, in order.
  auto BySlot = [](const Effect &A, const Effect &B) {
    return A.SlotVA < B.SlotVA;
  };
  // A PE's base relocations come in address order already.
  if (!std::is_sorted(Slots.begin(), Slots.end(), BySlot))
    std::stable_sort(Slots.begin(), Slots.end(), BySlot);
  auto Single = Slots.begin();
  for (auto Begin = Slots.begin(); Begin != Slots.end();) {
    auto End = std::next(Begin);
    while (End != Slots.end() && End->SlotVA == Begin->SlotVA)
      ++End;
    if (std::next(Begin) == End)
      *Single++ = *Begin;
    else
      for (auto It = Begin; It != End; ++It)
        applyAbsolutePointerRelocation(Img, *It);
    Begin = End;
  }
  Slots.erase(Single, Slots.end());

  // A slot leaves the maps its relocation does not put it in before any slot
  // joins one.
  for (const Effect &E : Slots) {
    switch (E.What) {
    case Kind::CodeOperandToCode:
      Img.DataAddressRelocOperands.erase(E.SlotVA);
      break;
    case Kind::DataSlotToCode:
      Img.DataPtrRelocSlots.erase(E.SlotVA);
      Img.DataPtrRelocTargetOwners.erase(E.SlotVA);
      break;
    case Kind::CodeOperandToData:
      Img.CodeAddressRelocOperands.erase(E.SlotVA);
      break;
    case Kind::DataSlotToData:
      Img.CodePtrRelocSlots.erase(E.SlotVA);
      break;
    case Kind::None:
    case Kind::DataTargetOnly:
      break;
    }
  }

  sortUnique(CodeTargets);
  sortUnique(DataTargets);
  sortUnique(WritableDataTargets);
  insertInOrder(Img.CodeRefTargets, CodeTargets);
  insertInOrder(Img.RelocDataAddrs, DataTargets);
  insertInOrder(Img.WritableRelocDataAddrs, WritableDataTargets);

  auto CodeOperand = Img.CodeAddressRelocOperands.end();
  auto DataOperand = Img.DataAddressRelocOperands.end();
  auto CodeSlot = Img.CodePtrRelocSlots.end();
  auto DataSlot = Img.DataPtrRelocSlots.end();
  auto DataOwner = Img.DataPtrRelocTargetOwners.end();
  for (const Effect &E : Slots) {
    switch (E.What) {
    case Kind::CodeOperandToCode:
      CodeOperand = recordOperand(Img.CodeAddressRelocOperands, CodeOperand,
                                  E.SlotVA, E.field(Img));
      break;
    case Kind::DataSlotToCode:
      CodeSlot = std::next(Img.CodePtrRelocSlots.insert(CodeSlot, E.SlotVA));
      break;
    case Kind::CodeOperandToData:
      DataOperand = recordOperand(Img.DataAddressRelocOperands, DataOperand,
                                  E.SlotVA, E.field(Img));
      break;
    case Kind::DataSlotToData:
      DataSlot = std::next(Img.DataPtrRelocSlots.insert(DataSlot, E.SlotVA));
      DataOwner = std::next(Img.DataPtrRelocTargetOwners.insert_or_assign(
          DataOwner, E.SlotVA, E.TargetOwnerBegin));
      break;
    case Kind::None:
    case Kind::DataTargetOnly:
      break;
    }
  }
}

unsigned neverd::recordDataInCode(BinaryImage &Img,
                                  llvm::ArrayRef<va_t> Objects) {
  const CodeMetadata Code(Img);
  unsigned Recorded = 0;
  for (const va_t Addr : Objects)
    if (Img.isCodeAddress(Addr) && !Code.claims(Addr) && !Code.endsAt(Addr))
      Recorded += Img.DataInCode.insert(Addr).second;
  return Recorded;
}

unsigned
neverd::recordDataPointerTargets(BinaryImage &Img,
                                 llvm::function_ref<bool(va_t)> InData) {
  const uint32_t PointerSize = Img.getPointerSize();
  if (PointerSize == 0 || Img.CodePtrRelocSlots.empty())
    return 0;
  const CodeMetadata Code(Img);
  std::vector<Effect> Effects;
  for (va_t Slot : Img.CodePtrRelocSlots) {
    const uint8_t *Bytes = Img.readVA(Slot, PointerSize);
    if (!Bytes)
      continue;
    const va_t Target = static_cast<va_t>(
        readPtr(Bytes, Img.is64Bit(), archLittleEndian(Img.Arch)));
    if (Code.claims(Target) || !InData(Target))
      continue;
    // The relocation as it would have been recorded had the target's section
    // kept data only.  The slot stays the data it was recorded as, though
    // its own section may hold code: a kernel keeps tables of pointers to
    // its literals among its code in PAGE (recordRelocatedPointerRunsInCode).
    Effects.push_back(decideAbsolutePointerRelocation(
        Img, Slot, Target, InvalidVA, [&](va_t Addr) {
          PointerRelocationAddressClass Class =
              classifyPointerRelocationAddress(Img, Addr);
          if (Addr == Target || Addr == Slot)
            Class.Executable = false;
          return Class;
        }));
  }
  unsigned Changed = 0;
  for (const Effect &E : Effects)
    Changed += applyAbsolutePointerRelocation(Img, E);
  return Changed;
}
