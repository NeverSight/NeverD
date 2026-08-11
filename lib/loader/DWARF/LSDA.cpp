//===- LSDA.cpp - Itanium language-specific data area decoding ------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/DWARF/LSDA.h"

#include "neverd/Support/BinaryEncoding.h"
#include "neverd/Support/DwarfEH.h"
#include "neverd/loader/LanguageRuntime.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <limits>
#include <set>

#define DEBUG_TYPE "neverd-dwarf-eh"

namespace neverd::dwarf_eh {
namespace {

using namespace dweh;

/// Largest LSDA record read from the image in one go.  A record describes one
/// function; a megabyte is already far beyond any real translation unit and
/// bounds the work a crafted header can request.
constexpr size_t kMaxLSDABytes = 1u << 20;

/// Largest mangled RTTI name followed out of a `std::type_info`.
constexpr size_t kMaxTypeNameBytes = 4096;

/// Largest run starting at \p Address, capped at \p Want, that a single
/// \ref BinaryImage::readVA can hand back.  The decoder works on a bounded
/// local buffer instead of chasing addresses field by field, so the length must
/// stay inside one segment: adjacent segments are contiguous in VA space but
/// live in separate buffers, and asking for a run that crosses the boundary
/// fails outright rather than returning the short prefix.
size_t mappedBytesAt(const BinaryImage &Img, va_t Address, size_t Want) {
  if (Want == 0 || !Img.readVA(Address, 1))
    return 0;
  size_t Lo = 1, Hi = Want;
  while (Lo < Hi) {
    const size_t Mid = Lo + (Hi - Lo + 1) / 2;
    if (Img.readVA(Address, Mid))
      Lo = Mid;
    else
      Hi = Mid - 1;
  }
  return Lo;
}

} // namespace

std::string readItaniumTypeName(const BinaryImage &Img, va_t TypeInfoVA) {
  if (TypeInfoVA == 0)
    return {};
  const unsigned PtrSize = Img.getPointerSize();
  if (PtrSize != 4 && PtrSize != 8)
    return {};
  if (TypeInfoVA > InvalidVA - PtrSize)
    return {};

  const uint8_t *NameSlot = Img.readVA(TypeInfoVA + PtrSize, PtrSize);
  if (!NameSlot)
    return {};
  const va_t NameVA = PtrSize == 4 ? va_t(readLE<uint32_t>(NameSlot))
                                   : va_t(readLE<uint64_t>(NameSlot));
  if (NameVA == 0)
    return {};

  const size_t Available = mappedBytesAt(Img, NameVA, kMaxTypeNameBytes);
  if (Available == 0)
    return {};
  const uint8_t *Bytes = Img.readVA(NameVA, Available);
  if (!Bytes)
    return {};
  size_t Length = 0;
  while (Length < Available && Bytes[Length] != 0)
    ++Length;
  // An unterminated run is not a C string; refusing it keeps a pointer into
  // arbitrary data from being reported as a type name.
  if (Length == 0 || Length == Available)
    return {};
  return std::string(reinterpret_cast<const char *>(Bytes), Length);
}

LSDAParseResult parseLSDA(const BinaryImage &Img, const LSDAParseRequest &Req,
                          const PointerBases &Bases) {
  LSDAParseResult Result;
  auto malformed = [&](const char *Message) {
    Result.ParseStatus = mergeExceptionParseStatus(
        Result.ParseStatus, ExceptionParseStatus::Malformed);
    Result.Diagnostics.emplace_back(Message);
    return Result;
  };
  auto partial = [&](const char *Message) {
    Result.ParseStatus = mergeExceptionParseStatus(
        Result.ParseStatus, ExceptionParseStatus::Partial);
    Result.Diagnostics.emplace_back(Message);
  };

  if (Req.LSDAVA == 0)
    return Result;

  const unsigned PtrSize = Img.getPointerSize();
  const size_t Available = mappedBytesAt(Img, Req.LSDAVA, kMaxLSDABytes);
  if (Available < 2)
    return malformed("Itanium LSDA is not mapped readable data");
  const uint8_t *Buf = Img.readVA(Req.LSDAVA, Available);
  if (!Buf)
    return malformed("Itanium LSDA is not mapped readable data");

  ItaniumEHInfo Info;
  Info.LSDAVA = Req.LSDAVA;
  Info.IsCallSiteAddressForm = !Req.IsSJLJ;

  size_t Cursor = 0;

  // --- Header: landing pad base -------------------------------------------
  Info.LandingPadBaseEncoding = Buf[Cursor++];
  if (Info.LandingPadBaseEncoding != Omit) {
    va_t Base = 0;
    if (!readEncodedPointer(Buf, Available, Cursor, Req.LSDAVA,
                            Info.LandingPadBaseEncoding, Bases, PtrSize, &Img,
                            Base))
      return malformed("unreadable Itanium LSDA landing-pad base");
    Info.LandingPadBase = Base;
  }
  // The ABI default is the start of the function the record describes.
  if (Info.LandingPadBase == 0)
    Info.LandingPadBase = Req.FunctionStart;

  // --- Header: type table -------------------------------------------------
  if (Cursor >= Available)
    return malformed("truncated Itanium LSDA header");
  Info.TypeTableEncoding = Buf[Cursor++];
  if (Info.TypeTableEncoding != Omit) {
    uint64_t TypeTableOffset = 0;
    if (!readULEB128(Buf, Available, Cursor, TypeTableOffset))
      return malformed("truncated Itanium LSDA type-table offset");
    // The offset is measured from the byte after the offset field itself.
    if (TypeTableOffset > InvalidVA - (Req.LSDAVA + Cursor))
      return malformed("Itanium LSDA type-table base overflows");
    Info.TypeTableVA = Req.LSDAVA + Cursor + TypeTableOffset;
  }

  // --- Header: call-site table --------------------------------------------
  if (Cursor >= Available)
    return malformed("truncated Itanium LSDA header");
  Info.CallSiteEncoding = Buf[Cursor++];
  uint64_t CallSiteTableLength = 0;
  if (!readULEB128(Buf, Available, Cursor, CallSiteTableLength))
    return malformed("truncated Itanium LSDA call-site table length");
  if (!rangeInBounds(Cursor, CallSiteTableLength, Available))
    return malformed("Itanium LSDA call-site table leaves mapped data");
  Info.CallSiteTableLength = CallSiteTableLength;

  const size_t CallSiteEnd = Cursor + static_cast<size_t>(CallSiteTableLength);
  const size_t ActionTableStart = CallSiteEnd;
  std::vector<uint64_t> ActionRoots;

  // The call-site fields are displacements from the landing-pad base, not
  // addresses, so only the value format applies.  Resolving the application
  // nibble here would add a base twice.
  const uint8_t CallSiteFormat = Info.CallSiteEncoding == Omit
                                     ? uint8_t(Omit)
                                     : getFormat(Info.CallSiteEncoding);
  PointerBases Raw;

  if (Req.IsSJLJ) {
    // The SJLJ form's "ranges" are call-site indices assigned by the
    // front end, not addresses.  They cannot be mapped onto code without the
    // dispatch switch, so record the record's shape and stop.
    partial("setjmp/longjmp Itanium call-site table is not address indexed");
    Cursor = CallSiteEnd;
  } else if (Info.CallSiteEncoding == Omit) {
    partial("Itanium LSDA omits its call-site encoding");
    Cursor = CallSiteEnd;
  } else {
    while (Cursor < CallSiteEnd) {
      if (Info.CallSites.size() >= Req.MaxRecords) {
        partial("Itanium LSDA call-site table exceeds decode budget");
        break;
      }
      va_t Start = 0, Length = 0, LandingPad = 0;
      uint64_t ActionRecord = 0;
      if (!readEncodedPointer(Buf, CallSiteEnd, Cursor, Req.LSDAVA,
                              CallSiteFormat, Raw, PtrSize, &Img, Start) ||
          !readEncodedPointer(Buf, CallSiteEnd, Cursor, Req.LSDAVA,
                              CallSiteFormat, Raw, PtrSize, &Img, Length) ||
          !readEncodedPointer(Buf, CallSiteEnd, Cursor, Req.LSDAVA,
                              CallSiteFormat, Raw, PtrSize, &Img, LandingPad) ||
          !readULEB128(Buf, CallSiteEnd, Cursor, ActionRecord))
        return malformed("truncated Itanium LSDA call-site entry");

      ItaniumCallSite Site;
      Site.NativeStart = Start;
      Site.NativeLength = Length;
      Site.NativeLandingPad = LandingPad;
      Site.NativeActionRecord = ActionRecord;

      if (Info.LandingPadBase > InvalidVA - Start ||
          Length > InvalidVA - (Info.LandingPadBase + Start))
        return malformed("Itanium LSDA call-site range overflows");
      const va_t RangeBegin = Info.LandingPadBase + Start;
      // A zero-length region is legal in the encoding but describes nothing;
      // keeping it would create an empty protected range in the IR.
      if (Length != 0) {
        Site.GuardedRange = {RangeBegin, RangeBegin + Length};
        if (Req.FunctionEnd != 0 && (RangeBegin < Req.FunctionStart ||
                                     RangeBegin + Length > Req.FunctionEnd))
          partial("Itanium LSDA call-site range leaves its function");
      }

      if (LandingPad != 0) {
        if (Info.LandingPadBase > InvalidVA - LandingPad)
          return malformed("Itanium LSDA landing-pad address overflows");
        Site.LandingPadVA = Info.LandingPadBase + LandingPad;
        if (Req.FunctionEnd != 0 && (Site.LandingPadVA < Req.FunctionStart ||
                                     Site.LandingPadVA >= Req.FunctionEnd))
          partial("Itanium LSDA landing pad leaves its function");
      }

      if (ActionRecord != 0) {
        // Action records are named by a 1-based byte offset into the action
        // table, so the stored table offset is one less.
        Site.FirstActionOffset = ActionRecord - 1;
        ActionRoots.push_back(ActionRecord - 1);
      }
      Info.CallSites.push_back(std::move(Site));
    }
    Cursor = CallSiteEnd;
  }

  // --- Action table -------------------------------------------------------
  // The action table carries no length, and its end cannot be computed from
  // the header: the type table that follows grows *downward* from a base past
  // its own last entry, so the gap between the two tables is only known once
  // the number of type entries is known — which the actions themselves
  // determine.  Walking the chains the call sites actually name resolves the
  // circularity exactly, and matches how a personality routine reads them.
  size_t ActionTableEnd = Available;
  if (Info.TypeTableVA != 0 && Info.TypeTableVA > Req.LSDAVA) {
    uint64_t Limit = Info.TypeTableVA - Req.LSDAVA;
    if (Limit < ActionTableEnd)
      ActionTableEnd = static_cast<size_t>(Limit);
  }
  if (ActionTableEnd < ActionTableStart)
    return malformed("Itanium LSDA type table precedes its action table");

  std::vector<uint64_t> Pending = ActionRoots;
  std::set<uint64_t> Decoded;
  while (!Pending.empty()) {
    const uint64_t TableOffset = Pending.back();
    Pending.pop_back();
    if (!Decoded.insert(TableOffset).second)
      continue;
    if (Info.Actions.size() >= Req.MaxRecords) {
      partial("Itanium LSDA action table exceeds decode budget");
      break;
    }
    if (TableOffset >= ActionTableEnd - ActionTableStart) {
      partial("Itanium LSDA names an action outside its table");
      continue;
    }

    size_t ActionCursor = ActionTableStart + static_cast<size_t>(TableOffset);
    int64_t Filter = 0;
    if (!readSLEB128(Buf, ActionTableEnd, ActionCursor, Filter)) {
      partial("truncated Itanium LSDA action record");
      continue;
    }
    const size_t LinkFieldOffset = ActionCursor - ActionTableStart;
    int64_t NextOffset = 0;
    if (!readSLEB128(Buf, ActionTableEnd, ActionCursor, NextOffset)) {
      partial("truncated Itanium LSDA action record");
      continue;
    }

    ItaniumAction Action;
    Action.TableOffset = TableOffset;
    Action.TypeFilter = Filter;
    if (NextOffset != 0) {
      // The chain link is self-relative: measured from the position of the
      // link field itself.
      const int64_t Target = static_cast<int64_t>(LinkFieldOffset) + NextOffset;
      if (Target < 0 ||
          static_cast<uint64_t>(Target) >= ActionTableEnd - ActionTableStart) {
        partial("Itanium LSDA action chain leaves its table");
      } else {
        Action.NextActionOffset = static_cast<uint64_t>(Target);
        Pending.push_back(static_cast<uint64_t>(Target));
      }
    }
    Info.Actions.push_back(std::move(Action));
  }
  std::sort(Info.Actions.begin(), Info.Actions.end(),
            [](const ItaniumAction &A, const ItaniumAction &B) {
              return A.TableOffset < B.TableOffset;
            });

  // --- Type table ---------------------------------------------------------
  // Entries grow downward from the base: entry N is at base - N * size.
  if (Info.TypeTableEncoding != Omit && Info.TypeTableVA != 0) {
    uint64_t HighestIndex = 0;
    for (const ItaniumAction &A : Info.Actions)
      if (A.isCatch())
        HighestIndex =
            std::max(HighestIndex, static_cast<uint64_t>(A.TypeFilter));

    const size_t EntrySize = getFormat(Info.TypeTableEncoding) == Absptr
                                 ? PtrSize
                                 : getEncodedSize(Info.TypeTableEncoding);
    if (HighestIndex != 0 && EntrySize == 0) {
      partial("Itanium LSDA type table uses a variable-length encoding");
    } else {
      for (uint64_t Index = 1; Index <= HighestIndex; ++Index) {
        if (Info.TypeTable.size() >= Req.MaxRecords) {
          partial("Itanium LSDA type table exceeds decode budget");
          break;
        }
        if (Index > Info.TypeTableVA / EntrySize) {
          partial("Itanium LSDA type-table entry underflows its base");
          break;
        }
        const va_t EntryVA = Info.TypeTableVA - Index * EntrySize;
        const uint8_t *Slot = Img.readVA(EntryVA, EntrySize);
        if (!Slot) {
          partial("Itanium LSDA type-table entry is not mapped");
          break;
        }
        size_t SlotCursor = 0;
        va_t TypeInfo = 0;
        va_t IndirectSlot = 0;
        if (!readEncodedPointer(Slot, EntrySize, SlotCursor, EntryVA,
                                Info.TypeTableEncoding, Bases, PtrSize, &Img,
                                TypeInfo, &IndirectSlot)) {
          partial("unresolved Itanium LSDA type-table entry");
          break;
        }
        // A slot the loader binds holds no usable pointer in the file image:
        // Mach-O chained fixups leave an encoded ordinal there and ELF leaves
        // zero or a link-time placeholder.  Reporting that word as an address
        // would hand a consumer a number that names nothing, so the address is
        // dropped and the slot — which the binding does name — is what carries
        // the identity.
        if (TypeInfo != 0 && IndirectSlot != 0 && !Img.readVA(TypeInfo, 1))
          TypeInfo = 0;

        ItaniumTypeEntry Entry;
        Entry.Index = Index;
        Entry.TypeInfoVA = TypeInfo;
        Entry.TypeInfoSlotVA = IndirectSlot;
        // A catch-all is a null slot in the image.  An indirect slot bound at
        // load time is also null there, but it is a real type: the binding
        // names it, so only an unbound null is the ABI's `catch (...)`.
        Entry.IsCatchAll = TypeInfo == 0 && IndirectSlot == 0;
        if (TypeInfo != 0)
          Entry.TypeName = readItaniumTypeName(Img, TypeInfo);
        if (Entry.TypeName.empty()) {
          Entry.TypeName = resolveRoutineName(Img, TypeInfo, IndirectSlot);
          Entry.IsCatchAll = Entry.IsCatchAll && Entry.TypeName.empty();
        }
        Info.TypeTable.push_back(std::move(Entry));
      }
    }

    // Exception-specification lists grow upward from the same base.
    for (const ItaniumAction &A : Info.Actions) {
      if (!A.isExceptionSpecification())
        continue;
      const uint64_t Index = static_cast<uint64_t>(-A.TypeFilter);
      bool Known = false;
      for (const ItaniumExceptionSpec &S : Info.ExceptionSpecs)
        if (S.Index == Index)
          Known = true;
      if (Known)
        continue;
      if (Index == 0 || Index - 1 > InvalidVA - Info.TypeTableVA) {
        partial("Itanium LSDA exception specification index is invalid");
        continue;
      }
      ItaniumExceptionSpec Spec;
      Spec.Index = Index;
      const va_t ListVA = Info.TypeTableVA + (Index - 1);
      const size_t ListAvailable = mappedBytesAt(Img, ListVA, 4096);
      const uint8_t *List =
          ListAvailable ? Img.readVA(ListVA, ListAvailable) : nullptr;
      if (!List) {
        partial("Itanium LSDA exception specification list is not mapped");
        continue;
      }
      size_t ListCursor = 0;
      while (ListCursor < ListAvailable) {
        uint64_t TypeIndex = 0;
        if (!readULEB128(List, ListAvailable, ListCursor, TypeIndex)) {
          partial("truncated Itanium LSDA exception specification list");
          break;
        }
        if (TypeIndex == 0)
          break;
        if (Spec.TypeIndices.size() >= Req.MaxRecords) {
          partial("Itanium LSDA exception specification exceeds budget");
          break;
        }
        Spec.TypeIndices.push_back(TypeIndex);
      }
      Info.ExceptionSpecs.push_back(std::move(Spec));
    }
  }

  Result.Info = std::move(Info);
  LLVM_DEBUG(llvm::dbgs() << "dwarf-eh: LSDA at 0x"
                          << llvm::utohexstr(Req.LSDAVA) << " has "
                          << Result.Info->CallSites.size() << " call sites, "
                          << Result.Info->Actions.size() << " actions, "
                          << Result.Info->TypeTable.size() << " types\n");
  return Result;
}

} // namespace neverd::dwarf_eh
