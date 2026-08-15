//===- ARMEHABI.cpp - ARM EHABI exception recovery driver -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Walks the sorted `.ARM.exidx` index of an image and turns every entry into
/// a normalized \ref ExceptionFunction: its code range, its unwind opcodes,
/// its personality routine, and the Itanium language data an `.ARM.extab`
/// entry appends after the opcodes.  The index parsing, opcode decoding,
/// entry decoding, and type-table convention proof this drives each live in a
/// file of their own beside this one.
///
//===----------------------------------------------------------------------===//

#include "neverd/loader/ELF/ARMEHABI.h"

#include "ARMEHABIDetail.h"

#include "neverd/loader/DWARF/LSDA.h"
#include "neverd/loader/LanguageRuntime.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/DwarfEH.h"

#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <optional>

#define DEBUG_TYPE "neverd-arm-ehabi"

namespace neverd::arm_ehabi {

using namespace dweh;
using namespace detail;

namespace {

/// A record whose type table was read before the image's convention had been
/// proven.
///
/// The convention holds for the whole image but nothing in the image states
/// it, so it is proven from the first record whose types this decoder can
/// reach -- and the records ahead of that one were already read against the
/// header's bare `absptr`.  A type defined in another shared object reaches
/// nothing from here and settles nothing, which is exactly the case that makes
/// the first record the wrong one to ask.  Reading those records again once
/// the answer exists keeps a frame's types from depending on where in the
/// index it happened to sit.
struct DeferredTypeTable {
  size_t FunctionIndex = 0;
  dwarf_eh::LSDAParseRequest Request;
  dwarf_eh::PointerBases Bases;
};

/// One index entry, resolved but not yet decoded.
struct IndexEntry {
  va_t EntryVA = 0;
  va_t FunctionVA = 0;
  uint32_t Word = 0;
};

} // namespace

void parseARMEHABIExceptions(BinaryImage &Img) {
  // `.ARM.exidx` is a processor-specific section type with a processor-
  // specific meaning.  Reading one out of an image built for another machine
  // would decode its words against the wrong pointer size.
  if (Img.Arch != Arch::ARM)
    return;
  // Every field of an index in an unlinked object is owed by a relocation the
  // link step has not applied, so each entry's `prel31` reads as a
  // displacement of zero and names its own address.  That is not an index of
  // anything, and reading it would put a frame on whatever happens to sit at
  // the bottom of the synthesized layout.
  if (Img.IsRelocatable)
    return;
  std::vector<TableSection> Indexes = findIndexSections(Img);
  if (Indexes.empty())
    return;

  // --- Collect the index -------------------------------------------------
  std::vector<IndexEntry> Entries;
  size_t Unreadable = 0;
  for (const TableSection &Index : Indexes) {
    const size_t Count = std::min<size_t>(Index.Size / kIndexEntrySize,
                                          kMaxIndexEntries - Entries.size());
    for (size_t I = 0; I < Count; ++I) {
      const uint8_t *Bytes = Index.Data + I * kIndexEntrySize;
      const uint32_t Word0 = readLE<uint32_t>(Bytes);
      const uint32_t Word1 = readLE<uint32_t>(Bytes + 4);
      const va_t EntryVA = static_cast<va_t>(Index.VA + I * kIndexEntrySize);
      // The first word is a `prel31`, so its top bit is not part of the
      // displacement.  An entry that sets it is not an index entry.
      if ((Word0 & kCompactBit) != 0) {
        ++Unreadable;
        continue;
      }
      IndexEntry Entry;
      Entry.EntryVA = EntryVA;
      // The linker clears the Thumb bit in the index because the table is
      // searched by program counter, but a hand-written or rewritten one may
      // not have; a function address that names an odd byte names none.
      Entry.FunctionVA =
          static_cast<va_t>(clearThumbBit(resolvePrel31(Word0, EntryVA)));
      Entry.Word = Word1;
      Entries.push_back(Entry);
    }
    if (Entries.size() >= kMaxIndexEntries)
      break;
  }
  if (Entries.empty())
    return;

  ExceptionInfo &Out = Img.ExceptionMetadata;
  Out.addModel(ExceptionModel::ARMEHABI);
  if (Unreadable != 0) {
    Out.ParseStatus = mergeExceptionParseStatus(Out.ParseStatus,
                                                ExceptionParseStatus::Partial);
    Out.Diagnostics.push_back(
        std::to_string(Unreadable) +
        " .ARM.exidx entries do not begin with a prel31 function address");
  }

  // The index is defined to be sorted, and an unwinder binary-searches it on
  // that basis.  Sorting a table that arrived out of order is what lets the
  // extent of each function be taken from the entry after it; saying so keeps
  // a reordered table from silently producing overlapping frames.
  const bool WasSorted =
      std::is_sorted(Entries.begin(), Entries.end(),
                     [](const IndexEntry &A, const IndexEntry &B) {
                       return A.FunctionVA < B.FunctionVA;
                     });
  if (!WasSorted) {
    std::sort(Entries.begin(), Entries.end(),
              [](const IndexEntry &A, const IndexEntry &B) {
                return A.FunctionVA < B.FunctionVA;
              });
    Out.ParseStatus = mergeExceptionParseStatus(Out.ParseStatus,
                                                ExceptionParseStatus::Partial);
    Out.Diagnostics.emplace_back(
        ".ARM.exidx is not sorted by function address, so the extent each "
        "entry covers was recovered by sorting it");
  }

  dwarf_eh::PointerBases Bases;
  for (const Segment &Seg : Img.Segments)
    if (Seg.isExecutable() && (Bases.Text == 0 || Seg.VA < Bases.Text))
      Bases.Text = Seg.VA;
  for (const char *Name : {section_names::elf::GotPlt, section_names::elf::Got})
    if (const Section *Sec = Img.getSectionByName(Name)) {
      Bases.Data = Sec->VA;
      break;
    }

  auto SeenSymbols = Img.getSymbolAddresses();
  ARMTypeTableConvention Convention = ARMTypeTableConvention::Unknown;
  std::vector<DeferredTypeTable> Undecided;
  size_t Added = 0;

  for (size_t I = 0; I < Entries.size(); ++I) {
    const IndexEntry &Entry = Entries[I];
    ExceptionFunction F;
    F.Kind = RuntimeFunctionKind::Primary;
    F.UnwindInfoVA = Entry.EntryVA;

    const va_t End = I + 1 < Entries.size()
                         ? Entries[I + 1].FunctionVA
                         : executableEndFor(Img, Entry.FunctionVA);
    if (End > Entry.FunctionVA) {
      F.CodeRange = {Entry.FunctionVA, End};
    } else {
      F.ParseStatus = ExceptionParseStatus::Malformed;
      F.Diagnostics.emplace_back(
          ".ARM.exidx entry describes an empty or inverted code range");
    }

    const Segment *Seg = Img.getSegmentFor(Entry.FunctionVA);
    if (!Seg || !Seg->isExecutable()) {
      F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus,
                                                ExceptionParseStatus::Partial);
      F.Diagnostics.emplace_back(
          ".ARM.exidx entry names a function outside executable data");
    }

    ARMEHABIInfo EHABI;
    EHABI.IndexEntryVA = Entry.EntryVA;
    EHABI.IndexWord = Entry.Word;

    if (Entry.Word == kCantUnwind) {
      EHABI.Kind = ARMEHABIEntryKind::CantUnwind;
      F.Encoding = ExceptionEncoding::ARMEHABICantUnwind;
    } else if ((Entry.Word & kCompactBit) != 0) {
      EHABI.Kind = ARMEHABIEntryKind::InlineCompact;
      F.Encoding = ExceptionEncoding::ARMEHABIInline;
      if ((Entry.Word & kCompactVendorMask) != 0 ||
          ((Entry.Word >> kCompactIndexShift) & kCompactIndexMask) != 0) {
        // Only routine 0 fits in the index word: the others need a word count
        // this form has no field for.
        F.ParseStatus = mergeExceptionParseStatus(
            F.ParseStatus, ExceptionParseStatus::Malformed);
        F.Diagnostics.emplace_back(
            ".ARM.exidx inline entry names a personality routine that cannot "
            "be encoded inline");
      } else {
        EHABI.PersonalityIndex = 0;
        F.Personality = ExceptionPersonality::AeabiUnwindCppPr0;
        F.PersonalityName = getExceptionPersonalityName(F.Personality);
        std::vector<uint8_t> Opcodes;
        appendWordOpcodes(Opcodes, Entry.Word, 3);
        F.NativeUnwindBytes = Opcodes;
        bool Refuses = false;
        if (!decodeUnwindOpcodes(Opcodes, F.UnwindOperations, Refuses))
          F.Diagnostics.emplace_back(
              "ARM EHABI inline opcodes end without a finish");
        if (Refuses)
          F.Diagnostics.emplace_back(
              "ARM EHABI opcodes refuse to unwind this frame");
      }
    } else {
      const va_t TableVA = resolvePrel31(Entry.Word, Entry.EntryVA + kWordSize);
      EHABI.TableEntryVA = TableVA;
      TableEntry Table;
      std::string Diagnostic;
      if (!decodeTableEntry(Img, TableVA, Table, Diagnostic)) {
        F.ParseStatus = mergeExceptionParseStatus(
            F.ParseStatus, ExceptionParseStatus::Malformed);
        F.Diagnostics.push_back(std::move(Diagnostic));
        F.Encoding = ExceptionEncoding::ARMEHABIGeneric;
      } else {
        EHABI.Kind = Table.Kind;
        EHABI.PersonalityIndex = Table.PersonalityIndex;
        EHABI.ExtraWordCount = Table.ExtraWordCount;
        F.Encoding = Table.Kind == ARMEHABIEntryKind::Compact
                         ? ExceptionEncoding::ARMEHABICompact
                         : ExceptionEncoding::ARMEHABIGeneric;
        F.NativeUnwindBytes = Table.Opcodes;
        bool Refuses = false;
        if (!decodeUnwindOpcodes(Table.Opcodes, F.UnwindOperations, Refuses))
          F.Diagnostics.emplace_back("ARM EHABI opcodes end without a finish");
        if (Refuses)
          F.Diagnostics.emplace_back(
              "ARM EHABI opcodes refuse to unwind this frame");

        if (Table.PersonalityIndex) {
          F.Personality = static_cast<ExceptionPersonality>(
              static_cast<uint8_t>(ExceptionPersonality::AeabiUnwindCppPr0) +
              *Table.PersonalityIndex);
          F.PersonalityName = getExceptionPersonalityName(F.Personality);
        } else {
          F.PersonalityVA = Table.PersonalityVA;
          F.PersonalityName = resolveRoutineName(Img, Table.PersonalityVA, 0);
          F.Personality = classifyPersonalityName(F.PersonalityName);
          if (F.Personality == ExceptionPersonality::Unknown ||
              F.Personality == ExceptionPersonality::None)
            F.Diagnostics.emplace_back("unnamed ARM EHABI personality routine");
        }

        // --- Language data ---------------------------------------------
        // EHABI gives the personality routine everything after the opcodes
        // and says nothing about what is there.  For the three ARM-defined
        // routines that is a scope-descriptor list with no types in it; for
        // every language personality it is an ordinary Itanium LSDA, which is
        // why an ARM image can hold a complete call-site graph and no
        // `.gcc_except_table` at all.
        F.HandlerDataVA = Table.HandlerDataVA;
        if (Img.readVA(Table.HandlerDataVA, 1) &&
            readsAnItaniumLSDA(F.Personality)) {
          dwarf_eh::LSDAParseRequest Req;
          Req.LSDAVA = Table.HandlerDataVA;
          Req.FunctionStart = F.CodeRange.Begin;
          Req.FunctionEnd = F.CodeRange.End;
          Req.Personality = F.Personality;
          dwarf_eh::PointerBases LSDABases = Bases;
          LSDABases.Func = F.CodeRange.Begin;

          // A bare `absptr` type table is the one place EHABI leaves the
          // encoding to the platform.  Whichever reading the image was linked
          // with holds for all of it, so the first record that proves one
          // settles the question for every record after it -- and, below, for
          // the ones before it too.  The override is offered unconditionally
          // because the LSDA reader applies it to the bare form alone.
          if (Convention != ARMTypeTableConvention::Unknown &&
              Convention != ARMTypeTableConvention::Absolute)
            Req.TypeTableEncodingOverride = encodingFor(Convention);
          dwarf_eh::LSDAParseResult LSDA =
              dwarf_eh::parseLSDA(Img, Req, LSDABases);

          if (Convention == ARMTypeTableConvention::Unknown && LSDA.Info &&
              LSDA.Info->TypeTableEncoding == Absptr &&
              !LSDA.Info->TypeTable.empty()) {
            Convention = proveTypeTableConvention(Img, *LSDA.Info);
            if (Convention == ARMTypeTableConvention::Unknown)
              Undecided.push_back({Out.Functions.size(), Req, LSDABases});
            else if (Convention != ARMTypeTableConvention::Absolute) {
              Req.TypeTableEncodingOverride = encodingFor(Convention);
              LSDA = dwarf_eh::parseLSDA(Img, Req, LSDABases);
            }
          }
          EHABI.TypeTableConvention = Convention;

          // Only a named personality promised that a table is there.  An
          // unnamed one -- which is what a static link that kept no symbol
          // for its routine leaves behind -- promised nothing, and the bytes
          // after the opcodes are then as likely to belong to the next entry.
          // They are still worth reading, because a stripped static binary is
          // exactly the image whose handlers nothing else can recover, but a
          // reading taken on no promise has to carry its own evidence: a
          // table that decoded cleanly and that describes this frame.  By the
          // same token, bytes that did not read as a table are not a fault in
          // an image that never claimed they were one.
          const bool Promised =
              F.Personality != ExceptionPersonality::Unknown &&
              F.Personality != ExceptionPersonality::None;
          const bool Usable =
              LSDA.Info &&
              (Promised ? LSDA.ParseStatus != ExceptionParseStatus::Malformed
                        : LSDA.ParseStatus == ExceptionParseStatus::Complete &&
                              !LSDA.Info->CallSites.empty());
          if (Usable)
            F.Itanium = std::move(*LSDA.Info);
          if (Promised) {
            F.ParseStatus =
                mergeExceptionParseStatus(F.ParseStatus, LSDA.ParseStatus);
            for (std::string &Diag : LSDA.Diagnostics)
              F.Diagnostics.push_back(std::move(Diag));
          } else if (!Usable) {
            F.HandlerDataVA = 0;
          }
        }
      }
    }

    F.ARMEHABI = std::move(EHABI);
    Out.ParseStatus = mergeExceptionParseStatus(Out.ParseStatus, F.ParseStatus);

    if (F.CodeRange.isValid()) {
      Img.KnownCodeRanges.emplace_back(F.CodeRange.Begin, F.CodeRange.End);
      // The index is the most complete function table a stripped ARM image
      // has: the linker puts an entry in it for every function it placed,
      // including the ones that unwind through no opcodes at all.
      if (Seg && Seg->isExecutable() &&
          F.ParseStatus != ExceptionParseStatus::Malformed &&
          SeenSymbols.insert(F.CodeRange.Begin).second) {
        Img.Symbols.push_back(
            Symbol::makeFunc(F.CodeRange.Begin, F.CodeRange.size()));
        ++Added;
      }
    }
    Out.Functions.push_back(std::move(F));
  }

  if (Convention != ARMTypeTableConvention::Unknown &&
      Convention != ARMTypeTableConvention::Absolute) {
    for (DeferredTypeTable &Deferred : Undecided) {
      ExceptionFunction &F = Out.Functions[Deferred.FunctionIndex];
      if (F.ARMEHABI)
        F.ARMEHABI->TypeTableConvention = Convention;
      if (!F.Itanium)
        continue;
      Deferred.Request.TypeTableEncodingOverride = encodingFor(Convention);
      dwarf_eh::LSDAParseResult LSDA =
          dwarf_eh::parseLSDA(Img, Deferred.Request, Deferred.Bases);
      if (LSDA.Info && LSDA.ParseStatus != ExceptionParseStatus::Malformed)
        F.Itanium = std::move(*LSDA.Info);
    }
  }

  std::sort(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end());
  Img.KnownCodeRanges.erase(
      std::unique(Img.KnownCodeRanges.begin(), Img.KnownCodeRanges.end()),
      Img.KnownCodeRanges.end());
  Out.rebuildIndex();

  LLVM_DEBUG(llvm::dbgs() << "arm-ehabi: normalized " << Entries.size()
                          << " index entries (" << Added << " new funcs, "
                          << getARMTypeTableConventionName(Convention)
                          << " type table)\n");
}

} // namespace neverd::arm_ehabi
