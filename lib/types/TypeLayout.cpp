//===- TypeLayout.cpp - Where a compiler puts a record's members ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/types/TypeLayout.h"

#include "llvm/Support/Error.h"

#include <algorithm>

namespace neverd::types {
namespace {

uint64_t alignBits(uint64_t Bits, uint32_t Alignment) {
  const uint64_t Unit = uint64_t(std::max<uint32_t>(Alignment, 1)) * 8;
  return (Bits + Unit - 1) / Unit * Unit;
}

llvm::Error problem(const llvm::Twine &Message) {
  return llvm::createStringError(llvm::inconvertibleErrorCode(), Message);
}

} // namespace

llvm::Expected<RecordDef> layOutRecord(const TypeLibrary &Library,
                                       const DraftRecord &Draft) {
  const LayoutRules Rules = Library.layoutRules();
  RecordDef Record;
  Record.Union = Draft.Union;
  Record.Pack = Draft.Pack;
  // A member aligns as its type, no further than the pack.
  const auto packed = [&](uint32_t Alignment) {
    return Draft.Pack ? std::min(Alignment, Draft.Pack) : Alignment;
  };
  uint64_t End = 0;    // In bits: where the next member may begin.
  uint64_t Widest = 0; // In bits: a union's widest alternative.
  uint32_t Alignment = 1;
  bool RequiredAlignment = Draft.Alignment != 0;
  // Microsoft's run of bitfields in one storage unit.
  struct Run {
    bool Active = false;
    uint64_t UnitBits = 0;
    uint64_t Start = 0;
    uint64_t Used = 0;
  } Bits;

  for (const DraftMember &Field : Draft.Members) {
    if (!Field.Type)
      continue;
    const std::string Name =
        Field.Name.empty() ? "an unnamed member" : Field.Name;
    std::optional<uint64_t> Size = Library.sizeOf(*Field.Type);
    // A flexible array holds no bytes of its own.
    if (!Size && Field.Type->Kind == TypeExprKind::Array && !Field.Type->Count)
      Size = 0;
    if (!Size)
      return problem("the size of " + Name + " is unknown");
    const uint32_t Natural = Library.alignOf(*Field.Type);
    // A member's declared alignment raises its packed one.  GNU's packed
    // member would lower the record's alignment below what its members'
    // types give, which a record keeps only as a pack of its own.
    if (Field.Packed && Draft.Pack != 1)
      return problem(Name + " is packed alone, which local types do not lay "
                            "out; pack the record");
    if (Field.Bitfield && Field.Alignment)
      return problem("bitfield " + Name +
                     " is aligned, which local types do not lay out");
    // A type's declared alignment survives the pack too, as Microsoft's
    // compiler lays it out.
    const uint32_t Required =
        Rules == LayoutRules::MSVC ? Library.requiredAlignOf(*Field.Type) : 0u;
    const uint32_t Aligned =
        std::max({packed(Natural), Field.Alignment, Required});
    // Microsoft propagates required alignment through containing records,
    // even when their natural alignment already equals it. A later packed
    // parent must not lower that inherited requirement.
    if (Rules == LayoutRules::MSVC && (Field.Alignment || Required))
      RequiredAlignment = true;

    if (Field.Bitfield) {
      const uint64_t Unit = *Size * 8;
      if (!Unit || Field.Width > Unit)
        return problem("bitfield " + Name + " is wider than its type");
      uint64_t Bit = 0, Storage = 0;
      if (Draft.Union) {
        if (!Field.Width)
          continue;
        Widest = std::max(Widest, Unit);
        Alignment = std::max(Alignment, Aligned);
      } else if (Rules == LayoutRules::MSVC) {
        if (!Field.Width) {
          Bits.Active = false;
          continue;
        }
        if (Bits.Active && Bits.UnitBits == Unit &&
            Bits.Used + Field.Width <= Unit) {
          Bit = Bits.Start + Bits.Used;
          Bits.Used += Field.Width;
        } else {
          Bits.Active = true;
          Bits.UnitBits = Unit;
          Bits.Start = alignBits(End, Aligned);
          Bits.Used = Field.Width;
          Bit = Bits.Start;
          End = Bits.Start + Unit;
        }
        Storage = Bits.Start;
        Alignment = std::max(Alignment, Aligned);
      } else {
        if (!Field.Width) {
          End = alignBits(End, Aligned);
          continue;
        }
        Bit = End;
        // Unless packed, a field spans no more units of its type's
        // alignment than its type has, as GCC places it: one unit of its
        // size where it aligns to that, two of four bytes for the i386
        // System V ABI's long long.
        const uint64_t Span = uint64_t(Natural) * 8;
        if (!Draft.Pack &&
            (Bit % Span + Field.Width + Span - 1) / Span > Unit / Span)
          Bit = alignBits(Bit, Natural);
        Storage = Bit / Span * Span;
        End = Bit + Field.Width;
        if (!Field.Name.empty())
          Alignment = std::max(Alignment, Aligned);
      }
      if (Field.Name.empty())
        continue;
      Member Laid;
      Laid.Name = Field.Name;
      Laid.Type = Field.Type;
      Laid.BitOffset = Bit;
      Laid.BitWidth = Field.Width;
      Laid.StorageBitOffset = Storage;
      Laid.Comment = Field.Comment;
      Record.Members.push_back(std::move(Laid));
      continue;
    }

    Bits.Active = false;
    Member Laid;
    Laid.Name = Field.Name;
    Laid.Type = Field.Type;
    Laid.Comment = Field.Comment;
    if (Draft.Union) {
      Widest = std::max(Widest, *Size * 8);
    } else {
      Laid.BitOffset = alignBits(End, Aligned);
      End = Laid.BitOffset + *Size * 8;
    }
    Alignment = std::max(Alignment, Aligned);
    Record.Members.push_back(std::move(Laid));
  }

  // A declared alignment may only raise the record's.
  Alignment = std::max(Alignment, Draft.Alignment);
  Record.Size = alignBits(Draft.Union ? Widest : End, Alignment) / 8;
  // A record with no bytes, such as a union of empty arrays, still takes
  // some: a byte in C++, and in Microsoft's C four or its declared
  // alignment.
  if (!Record.Size && (Draft.Cpp || Rules == LayoutRules::MSVC)) {
    const uint32_t Least = Draft.Cpp ? 1 : 4;
    Record.Size = Draft.Alignment >= Least ? Alignment : Least;
  }
  // Keep a required alignment even when the members already imply it: a
  // containing record's pack must distinguish it from natural alignment.
  if (RequiredAlignment || Alignment > Library.alignOf(Record))
    Record.Alignment = Alignment;
  return Record;
}

} // namespace neverd::types
