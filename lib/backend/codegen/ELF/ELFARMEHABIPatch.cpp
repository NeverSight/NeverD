//===- ELFARMEHABIPatch.cpp - ARM EHABI unwind-table rewrite -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Locates an image's `.ARM.exidx` and `.ARM.extab`, merges entries for
/// rewritten and added functions into the index, and appends the descriptors
/// those entries name.
///
/// The index is a binary-search structure keyed on function address, so an
/// entry cannot simply be appended: one whose function sorts before an
/// existing entry has to be inserted, and every entry it displaces moves.
/// Each entry is built from `prel31` displacements measured against its own
/// address, so a moved entry's words no longer mean what they meant.  The
/// index is therefore rewritten whole, out of displacements resolved back into
/// the addresses they named.
///
//===----------------------------------------------------------------------===//

#include "ELFARMEHABIPatchDetail.h"

#include "neverd/backend/ExceptionRewriteContract.h"
#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/ELF/ELFExceptionPatch.h"
#include "neverd/object/ELFLayout.h"
#include "neverd/object/SectionNames.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#define DEBUG_TYPE "neverd-elf-patch"

namespace neverd {

using namespace elf_arm_ehabi_detail;

namespace {

/// EHABI is defined for 32-bit ARM alone, so every header this rewrite reads
/// and writes is an ELF32 one.
constexpr bool kIs64 = false;
constexpr uint64_t kTargetAddressLimit = uint64_t(1) << 32;

/// An index entry with its `prel31` fields resolved back into addresses, which
/// is what lets it be moved: re-encoding an entry at a new address needs the
/// address it named, not the displacement that used to reach it.
struct IndexEntry {
  uint64_t FunctionVA = 0;
  /// The description the entry carries itself -- the reserved "cannot unwind"
  /// value, or an inline compact word -- used when \ref DescriptorVA is unset.
  uint32_t Word = 0;
  /// Where the entry's `.ARM.extab` descriptor is.
  std::optional<uint64_t> DescriptorVA;
};

bool byFunction(const IndexEntry &A, const IndexEntry &B) {
  return A.FunctionVA < B.FunctionVA;
}

bool checkedAdd(uint64_t Left, uint64_t Right, uint64_t &Result) {
  if (Right > std::numeric_limits<uint64_t>::max() - Left)
    return false;
  Result = Left + Right;
  return true;
}

bool checkedAlignUp(uint64_t Value, uint64_t Alignment, uint64_t &Result) {
  if (Alignment == 0 || (Alignment & (Alignment - 1)) != 0)
    return false;
  uint64_t Rounded = 0;
  if (!checkedAdd(Value, Alignment - 1, Rounded))
    return false;
  Result = Rounded & ~(Alignment - 1);
  return true;
}

/// How far a section may grow: to the next section laid out after it, or to
/// the end of the loadable segment that maps it, whichever comes first.
/// Returns false when no segment maps it, which leaves no room at all.
bool growthLimit(const uint8_t *Data, size_t Size,
                 const std::vector<uint64_t> &SectionOffsets, uint64_t FileOff,
                 uint64_t VA, uint64_t SectionSize, uint64_t &Limit) {
  Limit = Size;
  unsigned SegmentCount = 0;
  forEachELFPhdr(Data, Size,
                 [&](const ELFPhdrFields &P, const uint8_t *, bool) {
                   if (P.Type != llvm::ELF::PT_LOAD)
                     return;
                   if (!rangeInBounds(P.Offset, P.FileSz, Size) ||
                       P.Offset > FileOff || P.MemSz < P.FileSz)
                     return;
                   const uint64_t Delta = FileOff - P.Offset;
                   if (Delta > P.FileSz || SectionSize > P.FileSz - Delta ||
                       Delta > std::numeric_limits<uint64_t>::max() - P.VAddr ||
                       P.VAddr + Delta != VA)
                     return;
                   Limit = std::min(Limit, P.Offset + P.FileSz);
                   ++SegmentCount;
                 });
  if (SegmentCount != 1)
    return false;
  for (uint64_t Offset : SectionOffsets)
    if (Offset > FileOff)
      Limit = std::min(Limit, Offset);
  return Limit >= FileOff && Limit <= Size;
}

void growELFSection(std::vector<uint8_t> &Binary, uint64_t HeaderOff,
                    uint64_t NewSize) {
  if (!rangeInBounds(HeaderOff, getELFShdrSize(kIs64), Binary.size()))
    return;
  ELFShdrFields F = readELFShdr(Binary.data() + HeaderOff, kIs64);
  F.Size = NewSize;
  writeELFShdr(Binary.data() + HeaderOff, kIs64, F);
}

void appendOpcodeBytes(std::vector<uint8_t> &Opcodes, uint32_t Word,
                       unsigned Count) {
  for (unsigned I = Count; I-- > 0;)
    Opcodes.push_back(static_cast<uint8_t>(Word >> (I * 8)));
}

bool hasTerminatedOpcodeProgram(llvm::ArrayRef<uint8_t> Opcodes) {
  size_t Cursor = 0;
  while (Cursor < Opcodes.size()) {
    const uint8_t First = Opcodes[Cursor++];
    if (First == kFinishOpcode)
      return true;
    const bool HasSecond = (First & 0xF0) == 0x80 || First == 0xB1 ||
                           First == 0xB3 || First == 0xC6 || First == 0xC7 ||
                           First == 0xC8 || First == 0xC9;
    if (HasSecond) {
      if (Cursor >= Opcodes.size())
        return false;
      ++Cursor;
      continue;
    }
    if (First != 0xB2)
      continue;

    bool Complete = false;
    for (unsigned I = 0; I < 10 && Cursor < Opcodes.size(); ++I) {
      const uint8_t Byte = Opcodes[Cursor++];
      if (I == 9 && ((Byte & 0x80) != 0 || (Byte & 0x7F) > 1))
        return false;
      if ((Byte & 0x80) == 0) {
        Complete = true;
        break;
      }
    }
    if (!Complete)
      return false;
  }
  return false;
}

bool validateInlineOpcodeWord(uint32_t Word) {
  std::vector<uint8_t> Opcodes;
  appendOpcodeBytes(Opcodes, Word, kInlineOpcodeBytes);
  return hasTerminatedOpcodeProgram(Opcodes);
}

llvm::Expected<ELFARMEHABIModel>
validateDescriptorBytes(llvm::ArrayRef<uint8_t> Bytes, uint64_t Delta,
                        uint64_t DescriptorVA, llvm::StringRef Subject) {
  if ((DescriptorVA % kWordSize) != 0)
    return patchError(llvm::Twine(Subject) +
                      " .ARM.extab descriptor is not aligned");
  if (!rangeInBounds(Delta, kWordSize, Bytes.size()))
    return patchError(llvm::Twine(Subject) +
                      " .ARM.extab descriptor is truncated");

  const uint32_t First =
      readLE<uint32_t>(Bytes.data() + static_cast<size_t>(Delta));
  uint64_t RequiredSize = kWordSize;
  ELFARMEHABIModel Model = ELFARMEHABIModel::Compact;
  std::vector<uint8_t> Opcodes;
  if ((First & kCompactBit) != 0) {
    if ((First & kCompactVendorMask) != 0)
      return patchError(llvm::Twine(Subject) +
                        " .ARM.extab descriptor has an unknown vendor");
    const uint8_t Personality =
        static_cast<uint8_t>((First >> kCompactIndexShift) & 0xF);
    if (Personality > kMaxPersonalityIndex)
      return patchError(llvm::Twine(Subject) +
                        " .ARM.extab descriptor has an unknown personality");
    if (Personality != 0) {
      const uint64_t ExtraWords =
          (First >> kCompactExtraWordShift) & kMaxExtraWords;
      RequiredSize += ExtraWords * kWordSize;
    }
    appendOpcodeBytes(Opcodes, First,
                      Personality == 0 ? kInlineOpcodeBytes
                                       : kCompactOpcodeBytes);
  } else {
    Model = ELFARMEHABIModel::Generic;
    // A zero displacement is the value an unapplied R_ARM_PREL31 relocation
    // leaves behind.  It points at the descriptor itself, not a routine.
    if (First == 0)
      return patchError(llvm::Twine(Subject) +
                        " generic .ARM.extab has an unresolved personality");
    if (!rangeInBounds(Delta, 2 * kWordSize, Bytes.size()))
      return patchError(llvm::Twine(Subject) +
                        " generic .ARM.extab is truncated");
    const uint32_t Header =
        readLE<uint32_t>(Bytes.data() + static_cast<size_t>(Delta + kWordSize));
    const uint64_t ExtraWords = Header >> kGenericExtraWordShift;
    RequiredSize = 2 * kWordSize + ExtraWords * kWordSize;
    appendOpcodeBytes(Opcodes, Header, kGenericOpcodeBytes);
  }
  if (!rangeInBounds(Delta, RequiredSize, Bytes.size()))
    return patchError(llvm::Twine(Subject) +
                      " .ARM.extab opcode program is truncated");
  const uint64_t ExtraBegin =
      Delta + (Model == ELFARMEHABIModel::Generic ? 2 : 1) * kWordSize;
  for (uint64_t Off = ExtraBegin; Off < Delta + RequiredSize; Off += kWordSize)
    appendOpcodeBytes(
        Opcodes, readLE<uint32_t>(Bytes.data() + static_cast<size_t>(Off)), 4);
  if (!hasTerminatedOpcodeProgram(Opcodes))
    return patchError(llvm::Twine(Subject) +
                      " .ARM.extab opcode program has no finish");
  return Model;
}

/// Read the index at \p Region into entries whose targets are addresses.
llvm::Error readIndex(llvm::ArrayRef<uint8_t> Binary,
                      const ELFARMEHABIRegion &Region,
                      std::vector<IndexEntry> &Out) {
  if (!rangeInBounds(Region.IndexFileOff, Region.IndexSize, Binary.size()) ||
      (Region.IndexSize % kIndexEntrySize) != 0)
    return patchError("the .ARM.exidx at 0x" + llvm::utohexstr(Region.IndexVA) +
                      " is not a whole number of index entries");

  const uint64_t Count = Region.IndexSize / kIndexEntrySize;
  Out.reserve(static_cast<size_t>(Count));
  std::optional<uint64_t> PreviousFunction;
  for (uint64_t I = 0; I < Count; ++I) {
    if (I > (std::numeric_limits<uint64_t>::max() - Region.IndexVA) /
                kIndexEntrySize)
      return patchError("the .ARM.exidx entry address overflows");
    const uint8_t *Bytes =
        Binary.data() + Region.IndexFileOff + I * kIndexEntrySize;
    const uint64_t EntryVA = Region.IndexVA + I * kIndexEntrySize;
    const uint32_t Word0 = readLE<uint32_t>(Bytes);
    const uint32_t Word1 = readLE<uint32_t>(Bytes + kWordSize);
    // The first word is a `prel31`, so its top bit is not part of the
    // displacement.  An entry that sets it is not an index entry.
    if ((Word0 & kCompactBit) != 0)
      return patchError("the .ARM.exidx entry at 0x" +
                        llvm::utohexstr(EntryVA) +
                        " does not begin with a prel31 function address");

    IndexEntry Entry;
    // The linker clears the Thumb bit in the index because the table is
    // searched by program counter, which never has it set.
    Entry.FunctionVA = clearThumbBit(decodePrel31(Word0, EntryVA));
    if (PreviousFunction && Entry.FunctionVA <= *PreviousFunction)
      return patchError("the .ARM.exidx function addresses are not strictly "
                        "increasing");
    PreviousFunction = Entry.FunctionVA;
    if (Word1 == kCantUnwind) {
      Entry.Word = Word1;
    } else if ((Word1 & kCompactBit) != 0) {
      if ((Word1 & kCompactVendorMask) != 0 ||
          ((Word1 >> kCompactIndexShift) & 0xF) != 0 ||
          !validateInlineOpcodeWord(Word1))
        return patchError("the .ARM.exidx entry at 0x" +
                          llvm::utohexstr(EntryVA) +
                          " has an invalid inline personality");
      Entry.Word = Word1;
    } else {
      if (EntryVA > std::numeric_limits<uint64_t>::max() - kWordSize)
        return patchError("the .ARM.exidx descriptor field address overflows");
      Entry.DescriptorVA = decodePrel31(Word1, EntryVA + kWordSize);
      uint64_t TableEndVA = 0;
      if (!Region.HasTable ||
          !checkedAdd(Region.TableVA, Region.TableSize, TableEndVA) ||
          *Entry.DescriptorVA < Region.TableVA ||
          *Entry.DescriptorVA >= TableEndVA ||
          !rangeInBounds(Region.TableFileOff, Region.TableSize, Binary.size()))
        return patchError("the .ARM.exidx entry at 0x" +
                          llvm::utohexstr(EntryVA) +
                          " points outside its unique .ARM.extab");
      const llvm::ArrayRef<uint8_t> Table(
          Binary.data() + Region.TableFileOff,
          static_cast<size_t>(Region.TableSize));
      auto Model =
          validateDescriptorBytes(Table, *Entry.DescriptorVA - Region.TableVA,
                                  *Entry.DescriptorVA, "the input");
      if (!Model)
        return Model.takeError();
    }
    Out.push_back(Entry);
  }
  return llvm::Error::success();
}

/// Turn \p Records into entries, encoding a descriptor for each record that
/// does not already have one placed and collecting the bytes to append at
/// \p AppendVA.
llvm::Error planRecords(const ELFARMEHABIRegion &Region,
                        llvm::ArrayRef<ELFARMEHABIRecord> Records,
                        uint64_t AppendVA, std::vector<IndexEntry> &Out,
                        std::vector<uint8_t> &Appended) {
  Out.reserve(Records.size());
  for (const ELFARMEHABIRecord &Record : Records) {
    IndexEntry Entry;
    Entry.FunctionVA = clearThumbBit(Record.FunctionVA);

    const bool OutOfLine = Record.Model == ELFARMEHABIModel::Compact ||
                           Record.Model == ELFARMEHABIModel::Generic;
    if (Record.PlacedDescriptorVA) {
      if (!OutOfLine)
        return patchError("the record for the function at 0x" +
                          llvm::utohexstr(Entry.FunctionVA) +
                          " names a placed descriptor its model cannot reach");
      Entry.DescriptorVA = *Record.PlacedDescriptorVA;
    } else if (OutOfLine) {
      if (!Region.HasTable)
        return patchError("the image declares no .ARM.extab to append the "
                          "descriptor of the function at 0x" +
                          llvm::utohexstr(Entry.FunctionVA) + " to");
      uint64_t DescriptorVA = 0;
      if (!checkedAdd(AppendVA, Appended.size(), DescriptorVA))
        return patchError("the appended .ARM.extab address overflows");
      Entry.DescriptorVA = DescriptorVA;
      if (llvm::Error Err =
              encodeARMEHABIDescriptor(Record, *Entry.DescriptorVA, Appended))
        return Err;
    } else if (llvm::Error Err = encodeARMEHABIIndexWord(Record, Entry.Word)) {
      return Err;
    }
    Out.push_back(std::move(Entry));
  }
  return llvm::Error::success();
}

/// Merge \p New into \p Entries, keeping the index sorted by function address.
///
/// A record whose function already has an entry replaces it, which is what a
/// function rewritten where it stands needs; every other one is inserted.
llvm::Error mergeEntries(std::vector<IndexEntry> &Entries,
                         std::vector<IndexEntry> New) {
  std::stable_sort(New.begin(), New.end(), byFunction);
  auto Duplicate = std::adjacent_find(
      New.begin(), New.end(), [](const IndexEntry &A, const IndexEntry &B) {
        return A.FunctionVA == B.FunctionVA;
      });
  if (Duplicate != New.end())
    return patchError("two records describe the function at 0x" +
                      llvm::utohexstr(Duplicate->FunctionVA));
  if (New.size() > std::numeric_limits<size_t>::max() - Entries.size())
    return patchError("the rewritten .ARM.exidx entry count overflows");

  std::vector<IndexEntry> Merged;
  if (Entries.size() + New.size() > Merged.max_size())
    return patchError("the rewritten .ARM.exidx cannot be represented");
  Merged.reserve(Entries.size() + New.size());
  auto Old = Entries.begin();
  auto Add = New.begin();
  while (Old != Entries.end() || Add != New.end()) {
    if (Add == New.end() ||
        (Old != Entries.end() && Old->FunctionVA < Add->FunctionVA)) {
      Merged.push_back(std::move(*Old++));
      continue;
    }
    if (Old != Entries.end() && Old->FunctionVA == Add->FunctionVA)
      ++Old;
    Merged.push_back(std::move(*Add++));
  }
  Entries = std::move(Merged);
  return llvm::Error::success();
}

/// Encode \p Entries as the index that will sit at \p IndexVA.
llvm::Error encodeIndex(const std::vector<IndexEntry> &Entries,
                        uint64_t IndexVA, std::vector<uint8_t> &Out) {
  if (Entries.size() > Out.max_size() / kIndexEntrySize ||
      Entries.size() > std::numeric_limits<uint32_t>::max() / kIndexEntrySize)
    return patchError("the rewritten .ARM.exidx byte size cannot be encoded");
  Out.assign(Entries.size() * kIndexEntrySize, 0);
  for (size_t I = 0; I < Entries.size(); ++I) {
    const IndexEntry &Entry = Entries[I];
    if (I > (std::numeric_limits<uint64_t>::max() - IndexVA) / kIndexEntrySize)
      return patchError("the rewritten .ARM.exidx entry address overflows");
    const uint64_t EntryVA = IndexVA + I * kIndexEntrySize;
    uint32_t Word0 = 0;
    if (llvm::Error Err = encodePrel31(EntryVA, Entry.FunctionVA, Word0))
      return Err;
    uint32_t Word1 = Entry.Word;
    if (Entry.DescriptorVA) {
      if (EntryVA > std::numeric_limits<uint64_t>::max() - kWordSize)
        return patchError(
            "the rewritten .ARM.exidx descriptor field address overflows");
      if (llvm::Error Err =
              encodePrel31(EntryVA + kWordSize, *Entry.DescriptorVA, Word1))
        return Err;
    }

    uint8_t *Bytes = Out.data() + I * kIndexEntrySize;
    writeLE<uint32_t>(Bytes, Word0);
    writeLE<uint32_t>(Bytes + kWordSize, Word1);
  }
  return llvm::Error::success();
}

/// The bytes of \p Section, wherever the compile placed them.
llvm::ArrayRef<uint8_t> sectionBytes(const CompiledImage &Compiled,
                                     const CompiledSection &Section) {
  if (!Section.IsInImage)
    return Section.ExternalBytes;
  if (!rangeInBounds(Section.Offset, Section.Size, Compiled.Bytes.size()))
    return {};
  return llvm::ArrayRef<uint8_t>(Compiled.Bytes.data() + Section.Offset,
                                 static_cast<size_t>(Section.Size));
}

bool isGeneratedDescriptorSection(const CompiledSection &Section) {
  const llvm::StringRef Name(Section.Name);
  return Section.IsAllocated &&
         (Name == section_names::elf::ArmExTab ||
          Name.starts_with(section_names::elf::ArmExTabPrefix));
}

llvm::Expected<ELFARMEHABIModel>
validateGeneratedDescriptor(const CompiledImage &Compiled,
                            uint64_t DescriptorVA) {
  if ((DescriptorVA % kWordSize) != 0)
    return patchError("a regenerated .ARM.extab descriptor is not aligned");

  const CompiledSection *Owner = nullptr;
  for (const CompiledSection &Section : Compiled.Sections) {
    if (!isGeneratedDescriptorSection(Section))
      continue;
    uint64_t EndVA = 0;
    if (!checkedAdd(Section.VA, Section.Size, EndVA))
      return patchError("a regenerated .ARM.extab range overflows");
    if (DescriptorVA < Section.VA || DescriptorVA >= EndVA)
      continue;
    if (Owner)
      return patchError(
          "a regenerated descriptor belongs to multiple .ARM.extab sections");
    Owner = &Section;
  }
  if (!Owner)
    return patchError(
        "a regenerated .ARM.exidx entry points outside .ARM.extab");

  const llvm::ArrayRef<uint8_t> Bytes = sectionBytes(Compiled, *Owner);
  if (Bytes.size() != Owner->Size)
    return patchError("a regenerated .ARM.extab has unavailable bytes");
  const uint64_t Delta = DescriptorVA - Owner->VA;
  return validateDescriptorBytes(Bytes, Delta, DescriptorVA, "a regenerated");
}

bool isGeneratedIndexSection(const CompiledSection &Section) {
  llvm::StringRef Name(Section.Name);
  return Section.IsAllocated &&
         (Name == section_names::elf::ArmExIdx ||
          Name.starts_with(section_names::elf::ArmExIdxPrefix));
}

/// Read the index fragment codegen produced into records that name the
/// descriptors codegen placed beside it.
llvm::Error collectGeneratedRecords(const CompiledImage &Compiled,
                                    std::vector<ELFARMEHABIRecord> &Out,
                                    std::vector<uint64_t> &RangeTerminators) {
  const size_t FirstGeneratedRecord = Out.size();
  for (const CompiledSection &Section : Compiled.Sections) {
    if (!isGeneratedIndexSection(Section))
      continue;
    llvm::ArrayRef<uint8_t> Bytes = sectionBytes(Compiled, Section);
    if (Bytes.size() != Section.Size || (Bytes.size() % kIndexEntrySize) != 0)
      return patchError("the regenerated .ARM.exidx is not a whole number of "
                        "index entries");
    if ((Section.VA % kWordSize) != 0)
      return patchError("the regenerated .ARM.exidx is not word aligned");

    for (size_t Off = 0; Off < Bytes.size(); Off += kIndexEntrySize) {
      if (Off > std::numeric_limits<uint64_t>::max() - Section.VA)
        return patchError("a regenerated .ARM.exidx entry address overflows");
      const uint64_t EntryVA = Section.VA + Off;
      if (EntryVA >= kTargetAddressLimit ||
          kIndexEntrySize > kTargetAddressLimit - EntryVA)
        return patchError("a regenerated .ARM.exidx entry range exceeds the "
                          "target address width");
      const uint32_t Word0 = readLE<uint32_t>(Bytes.data() + Off);
      const uint32_t Word1 = readLE<uint32_t>(Bytes.data() + Off + kWordSize);
      if ((Word0 & kCompactBit) != 0)
        return patchError("a regenerated .ARM.exidx entry does not begin with "
                          "a prel31 function address");

      ELFARMEHABIRecord Record;
      const uint64_t DecodedFunctionVA = decodePrel31(Word0, EntryVA);
      if (DecodedFunctionVA != clearThumbBit(DecodedFunctionVA))
        return patchError("a regenerated .ARM.exidx function address carries "
                          "a Thumb bit");
      Record.FunctionVA = DecodedFunctionVA;
      // A displacement of zero is what an unapplied `R_ARM_PREL31` leaves
      // behind, and an entry that names its own address names no function.
      if (Record.FunctionVA == EntryVA)
        return patchError("the regenerated .ARM.exidx entry at 0x" +
                          llvm::utohexstr(EntryVA) +
                          " still holds an unrelocated function address");
      uint32_t RoundTrip = 0;
      if (llvm::Error Err = encodePrel31(EntryVA, Record.FunctionVA, RoundTrip))
        return Err;
      if (RoundTrip != Word0)
        return patchError("a regenerated .ARM.exidx function displacement "
                          "does not round-trip exactly");

      if (Word1 == kCantUnwind) {
        Record.Model = ELFARMEHABIModel::CantUnwind;
      } else if ((Word1 & kCompactBit) != 0) {
        if ((Word1 & kCompactVendorMask) != 0 ||
            ((Word1 >> kCompactIndexShift) & 0xF) != 0 ||
            !validateInlineOpcodeWord(Word1)) {
          return patchError("a regenerated .ARM.exidx entry names a "
                            "personality routine or opcode program that "
                            "cannot be encoded inline");
        }
        Record.Model = ELFARMEHABIModel::Inline;
        for (size_t Byte = kInlineOpcodeBytes; Byte-- > 0;)
          Record.Opcodes.push_back(static_cast<uint8_t>(Word1 >> (Byte * 8)));
      } else {
        // The descriptor is already in the appended segment at a settled
        // address; which of the two out-of-line models it uses is a property
        // of those bytes rather than of the entry that reaches them.
        if (EntryVA > std::numeric_limits<uint64_t>::max() - kWordSize)
          return patchError(
              "a regenerated .ARM.exidx descriptor field overflows");
        const uint64_t DescriptorVA = decodePrel31(Word1, EntryVA + kWordSize);
        auto Model = validateGeneratedDescriptor(Compiled, DescriptorVA);
        if (!Model)
          return Model.takeError();
        Record.Model = *Model;
        Record.PlacedDescriptorVA = DescriptorVA;
      }
      Out.push_back(std::move(Record));
    }
  }

  if (Out.size() == FirstGeneratedRecord)
    return llvm::Error::success();

  std::vector<uint64_t> FunctionStarts;
  FunctionStarts.reserve(Out.size() - FirstGeneratedRecord);
  for (size_t I = FirstGeneratedRecord; I < Out.size(); ++I)
    FunctionStarts.push_back(Out[I].FunctionVA);
  std::sort(FunctionStarts.begin(), FunctionStarts.end());
  if (std::adjacent_find(FunctionStarts.begin(), FunctionStarts.end()) !=
      FunctionStarts.end())
    return patchError(
        "the regenerated .ARM.exidx contains duplicate function starts");

  std::vector<std::pair<uint64_t, uint64_t>> CodeRanges;
  for (const CompiledSection &Section : Compiled.Sections) {
    if (!Section.IsAllocated || Section.Size == 0 ||
        Section.Kind != llvm::mc_rewrite::RewriteSectionKind::Code)
      continue;
    if ((Section.VA & 1) != 0 || (Section.Size & 1) != 0)
      return patchError("a generated executable range is not halfword aligned");
    uint64_t EndVA = 0;
    if (!checkedAdd(Section.VA, Section.Size, EndVA))
      return patchError("a generated executable section range overflows");
    if (Section.VA >= kTargetAddressLimit || EndVA > kTargetAddressLimit)
      return patchError("a generated executable section range exceeds the "
                        "target address width");
    CodeRanges.emplace_back(clearThumbBit(Section.VA), clearThumbBit(EndVA));
  }
  if (CodeRanges.empty())
    return patchError("the regenerated .ARM.exidx has no executable range");
  std::sort(CodeRanges.begin(), CodeRanges.end());
  std::vector<std::pair<uint64_t, uint64_t>> Merged;
  for (const auto &Range : CodeRanges) {
    if (!Merged.empty() && Range.first <= Merged.back().second) {
      Merged.back().second = std::max(Merged.back().second, Range.second);
      continue;
    }
    Merged.push_back(Range);
  }

  for (size_t I = FirstGeneratedRecord; I < Out.size(); ++I) {
    const uint64_t FunctionVA = Out[I].FunctionVA;
    const bool InGeneratedCode =
        std::any_of(Merged.begin(), Merged.end(), [&](const auto &Range) {
          return FunctionVA >= Range.first && FunctionVA < Range.second;
        });
    if (!InGeneratedCode)
      return patchError("a regenerated .ARM.exidx function start lies "
                        "outside every generated executable range");
  }

  // The final index rule extends until the next entry.  Terminate every
  // generated executable run explicitly so it can never claim unrelated
  // addresses that happen to follow the appended code.
  for (const auto &[Begin, End] : Merged) {
    (void)Begin;
    auto Existing = std::find_if(Out.begin(), Out.end(), [&](const auto &R) {
      return clearThumbBit(R.FunctionVA) == End;
    });
    if (Existing != Out.end()) {
      if (Existing->Model != ELFARMEHABIModel::CantUnwind)
        return patchError("a generated executable-range end is also a live "
                          ".ARM.exidx function entry");
      continue;
    }
    ELFARMEHABIRecord Sentinel;
    Sentinel.FunctionVA = End;
    Sentinel.Model = ELFARMEHABIModel::CantUnwind;
    Out.push_back(std::move(Sentinel));
    RangeTerminators.push_back(End);
  }
  return llvm::Error::success();
}

/// An existing entry at a generated range's end already terminates the
/// preceding range.  Keep that entry -- it may describe a real function that
/// starts at the boundary -- instead of replacing it with the synthetic
/// `cantunwind` record.
llvm::Error
validateGeneratedKeysAgainstInput(llvm::ArrayRef<uint8_t> Binary,
                                  const ELFARMEHABIRegion &Region,
                                  llvm::ArrayRef<uint64_t> RangeTerminators,
                                  std::vector<ELFARMEHABIRecord> &Records) {
  std::vector<IndexEntry> Existing;
  if (llvm::Error Err = readIndex(Binary, Region, Existing))
    return Err;
  for (uint64_t Terminator : RangeTerminators) {
    const bool HasBoundary =
        std::any_of(Existing.begin(), Existing.end(), [&](const IndexEntry &E) {
          return E.FunctionVA == Terminator;
        });
    if (!HasBoundary)
      continue;
    auto It = std::find_if(
        Records.begin(), Records.end(), [&](const ELFARMEHABIRecord &Record) {
          return Record.FunctionVA == Terminator &&
                 Record.Model == ELFARMEHABIModel::CantUnwind;
        });
    if (It == Records.end())
      return patchError("a generated range terminator was lost");
    Records.erase(It);
  }

  for (const ELFARMEHABIRecord &Record : Records) {
    const uint64_t FunctionVA = clearThumbBit(Record.FunctionVA);
    const auto It =
        std::lower_bound(Existing.begin(), Existing.end(), FunctionVA,
                         [](const IndexEntry &Entry, uint64_t Address) {
                           return Entry.FunctionVA < Address;
                         });
    if (It != Existing.end() && It->FunctionVA == FunctionVA)
      return patchError("a regenerated .ARM.exidx entry would overwrite the "
                        "input entry at 0x" +
                        llvm::utohexstr(FunctionVA));
  }
  return llvm::Error::success();
}

} // namespace

std::optional<ELFARMEHABIRegion>
findELFARMEHABIRegion(llvm::ArrayRef<uint8_t> Binary) {
  const uint8_t *Data = Binary.data();
  const size_t Size = Binary.size();
  ELFHeaderInfo Hdr;
  if (!validateELFHeaderTables(Data, Size, Hdr) || Hdr.Is64)
    return std::nullopt;
  // The index is a processor-specific section with a processor-specific
  // meaning; under another machine or byte order its words are not an index.
  const auto *EHdr =
      reinterpret_cast<const llvm::object::ELF32LE::Ehdr *>(Data);
  if (EHdr->e_machine != llvm::ELF::EM_ARM ||
      Data[llvm::ELF::EI_DATA] != llvm::ELF::ELFDATA2LSB)
    return std::nullopt;

  uint64_t ShStrOff =
      Hdr.ShOff + static_cast<uint64_t>(Hdr.ShStrNdx) * Hdr.ShEntSize;
  if (!rangeInBounds(ShStrOff, Hdr.ShEntSize, Size))
    return std::nullopt;
  auto ShStr = readELFShdr(Data + ShStrOff, kIs64);
  if (ShStr.Type != llvm::ELF::SHT_STRTAB ||
      !rangeInBounds(ShStr.Offset, ShStr.Size, Size))
    return std::nullopt;

  struct Section {
    ELFShdrFields F;
    uint64_t HeaderOff = 0;
  };
  std::optional<Section> Index, Table;
  unsigned IndexCount = 0;
  unsigned TableCount = 0;
  bool InvalidIndexIdentity = false;
  bool InvalidSectionName = false;
  std::vector<uint64_t> SectionOffsets;
  forEachELFShdr(Data, Size, [&](const ELFShdrFields &F, uint16_t I) {
    uint64_t HeaderOff = Hdr.ShOff + static_cast<uint64_t>(I) * Hdr.ShEntSize;
    if (F.Type != llvm::ELF::SHT_NOBITS && F.Offset != 0)
      SectionOffsets.push_back(F.Offset);
    auto Name = readELFSectionName(Data, Size, ShStr, F.Name);
    if (!Name) {
      InvalidSectionName = true;
      return;
    }
    if (F.Type == llvm::ELF::SHT_ARM_EXIDX ||
        *Name == section_names::elf::ArmExIdx) {
      ++IndexCount;
      if (F.Type != llvm::ELF::SHT_ARM_EXIDX ||
          *Name != section_names::elf::ArmExIdx) {
        InvalidIndexIdentity = true;
      } else if (!Index) {
        Index = Section{F, HeaderOff};
      }
    } else if (*Name == section_names::elf::ArmExTab) {
      ++TableCount;
      if (!Table)
        Table = Section{F, HeaderOff};
    }
  });
  // A link told to keep per-function sections leaves a run of indexes, and the
  // table they form is their concatenation.  Rewriting one of them would leave
  // the others describing the frames that fall between its entries.
  if (InvalidSectionName || !Index || IndexCount != 1 || TableCount > 1 ||
      InvalidIndexIdentity)
    return std::nullopt;

  const ELFShdrFields &IF = Index->F;
  if (IF.Size == 0 || IF.Offset == 0 ||
      (IF.Flags & llvm::ELF::SHF_ALLOC) == 0 ||
      (IF.Size % kIndexEntrySize) != 0 ||
      !rangeInBounds(IF.Offset, IF.Size, Size))
    return std::nullopt;

  ELFARMEHABIRegion Region;
  Region.IndexVA = IF.Addr;
  Region.IndexFileOff = IF.Offset;
  Region.IndexSize = IF.Size;
  Region.IndexSectionHeaderOff = Index->HeaderOff;
  if (!growthLimit(Data, Size, SectionOffsets, IF.Offset, IF.Addr, IF.Size,
                   Region.IndexLimitFileOff) ||
      Region.IndexLimitFileOff < IF.Offset + IF.Size)
    return std::nullopt;

  // Without the program header that publishes it, the index is one the
  // unwinder has no way to find.  It has to publish this index and not merely
  // one at the same place: every displacement written below is measured from
  // the section's address, and an unwinder that reads the table at another
  // would resolve all of them somewhere else.
  uint64_t ExidxPhdrOff = 0;
  unsigned ExidxHeaderCount = 0;
  unsigned ExidxPhdrCount = 0;
  forEachELFPhdr(Data, Size,
                 [&](const ELFPhdrFields &P, const uint8_t *Ptr, bool) {
                   if (P.Type != llvm::ELF::PT_ARM_EXIDX)
                     return;
                   ++ExidxHeaderCount;
                   if (P.Offset == IF.Offset && P.VAddr == IF.Addr &&
                       P.FileSz == IF.Size && P.MemSz == IF.Size) {
                     ++ExidxPhdrCount;
                     ExidxPhdrOff = static_cast<uint64_t>(Ptr - Data);
                   }
                 });
  if (ExidxHeaderCount != 1 || ExidxPhdrCount != 1 || ExidxPhdrOff == 0)
    return std::nullopt;
  Region.ExidxPhdrOff = ExidxPhdrOff;

  if (Table) {
    const ELFShdrFields &TF = Table->F;
    uint64_t TableLimit = 0;
    // A descriptor appended past the table is placed by stepping the same
    // distance through both its address and its file offset, so a table that
    // was never given an address of its own would put every descriptor at an
    // address nothing maps.  Leaving the table out is what makes the models
    // that need one fail closed instead.
    if (TF.Type != llvm::ELF::SHT_PROGBITS || TF.Addr == 0 || TF.Offset == 0 ||
        (TF.Flags & llvm::ELF::SHF_ALLOC) == 0 ||
        !rangeInBounds(TF.Offset, TF.Size, Size) ||
        !growthLimit(Data, Size, SectionOffsets, TF.Offset, TF.Addr, TF.Size,
                     TableLimit) ||
        TableLimit < TF.Offset + TF.Size)
      return std::nullopt;
    Region.HasTable = true;
    Region.TableVA = TF.Addr;
    Region.TableFileOff = TF.Offset;
    Region.TableSize = TF.Size;
    Region.TableLimitFileOff = TableLimit;
    Region.TableSectionHeaderOff = Table->HeaderOff;
  }
  return Region;
}

llvm::Error
installELFARMEHABIRecords(std::vector<uint8_t> &Binary,
                          const ELFARMEHABIRegion &Region,
                          llvm::ArrayRef<ELFARMEHABIRecord> Records) {
  if (Records.empty())
    return llvm::Error::success();

  const std::optional<ELFARMEHABIRegion> Current =
      findELFARMEHABIRegion(Binary);
  if (!Current || Current->IndexVA != Region.IndexVA ||
      Current->IndexFileOff != Region.IndexFileOff ||
      Current->IndexSize != Region.IndexSize ||
      Current->IndexLimitFileOff != Region.IndexLimitFileOff ||
      Current->IndexSectionHeaderOff != Region.IndexSectionHeaderOff ||
      Current->ExidxPhdrOff != Region.ExidxPhdrOff ||
      Current->HasTable != Region.HasTable ||
      Current->TableVA != Region.TableVA ||
      Current->TableFileOff != Region.TableFileOff ||
      Current->TableSize != Region.TableSize ||
      Current->TableLimitFileOff != Region.TableLimitFileOff ||
      Current->TableSectionHeaderOff != Region.TableSectionHeaderOff)
    return patchError("the public ARM EHABI region is not the image's exact "
                      "current layout");

  if (Region.IndexLimitFileOff < Region.IndexFileOff ||
      !rangeInBounds(Region.IndexSectionHeaderOff, getELFShdrSize(kIs64),
                     Binary.size()) ||
      !rangeInBounds(Region.ExidxPhdrOff, getELFPhdrSize(kIs64), Binary.size()))
    return patchError("the public .ARM.exidx region is invalid");
  const ELFShdrFields IndexSection =
      readELFShdr(Binary.data() + Region.IndexSectionHeaderOff, kIs64);
  const ELFPhdrFields ExidxHeader =
      readELFPhdr(Binary.data() + Region.ExidxPhdrOff, kIs64);
  if (IndexSection.Type != llvm::ELF::SHT_ARM_EXIDX ||
      (IndexSection.Flags & llvm::ELF::SHF_ALLOC) == 0 ||
      IndexSection.Offset != Region.IndexFileOff ||
      IndexSection.Addr != Region.IndexVA ||
      IndexSection.Size != Region.IndexSize ||
      ExidxHeader.Type != llvm::ELF::PT_ARM_EXIDX ||
      ExidxHeader.Offset != Region.IndexFileOff ||
      ExidxHeader.VAddr != Region.IndexVA ||
      ExidxHeader.FileSz != Region.IndexSize ||
      ExidxHeader.MemSz != Region.IndexSize)
    return patchError("the public .ARM.exidx region does not match the image");

  // Descriptors are settled first: an entry cannot be written before the
  // address of the record it points at is known.
  uint64_t AppendVA = 0;
  uint64_t AppendFileOff = 0;
  if (Region.HasTable) {
    if (Region.TableLimitFileOff < Region.TableFileOff ||
        !rangeInBounds(Region.TableSectionHeaderOff, getELFShdrSize(kIs64),
                       Binary.size()))
      return patchError("the public .ARM.extab region is invalid");
    const ELFShdrFields TableSection =
        readELFShdr(Binary.data() + Region.TableSectionHeaderOff, kIs64);
    if (TableSection.Type != llvm::ELF::SHT_PROGBITS ||
        (TableSection.Flags & llvm::ELF::SHF_ALLOC) == 0 ||
        TableSection.Offset != Region.TableFileOff ||
        TableSection.Addr != Region.TableVA ||
        TableSection.Size != Region.TableSize)
      return patchError(
          "the public .ARM.extab region does not match the image");

    uint64_t TableEndVA = 0;
    if (!checkedAdd(Region.TableVA, Region.TableSize, TableEndVA) ||
        !checkedAlignUp(TableEndVA, kWordSize, AppendVA) ||
        !checkedAdd(Region.TableFileOff, AppendVA - Region.TableVA,
                    AppendFileOff) ||
        AppendFileOff > Region.TableLimitFileOff)
      return patchError("the appended .ARM.extab location overflows");
  }

  std::vector<IndexEntry> Entries;
  if (llvm::Error Err = readIndex(Binary, Region, Entries))
    return Err;

  std::vector<IndexEntry> New;
  std::vector<uint8_t> Appended;
  if (llvm::Error Err = planRecords(Region, Records, AppendVA, New, Appended))
    return Err;
  if (llvm::Error Err = mergeEntries(Entries, std::move(New)))
    return Err;

  std::vector<uint8_t> Index;
  if (llvm::Error Err = encodeIndex(Entries, Region.IndexVA, Index))
    return Err;

  if (Region.IndexLimitFileOff < Region.IndexFileOff ||
      Index.size() > Region.IndexLimitFileOff - Region.IndexFileOff ||
      !rangeInBounds(Region.IndexFileOff, Index.size(), Binary.size()))
    return patchError("no slack after .ARM.exidx for " +
                      llvm::Twine(Entries.size()) + " index entries");
  if (!Appended.empty() && !Region.HasTable)
    return patchError("the image has no .ARM.extab for appended descriptors");
  uint64_t AppendFileEnd = AppendFileOff;
  uint64_t AppendVAEnd = AppendVA;
  if (!Appended.empty() &&
      (!checkedAdd(AppendFileOff, Appended.size(), AppendFileEnd) ||
       !checkedAdd(AppendVA, Appended.size(), AppendVAEnd) ||
       AppendFileEnd > Region.TableLimitFileOff ||
       !rangeInBounds(AppendFileOff, Appended.size(), Binary.size())))
    return patchError("no slack after .ARM.extab for " +
                      llvm::Twine(Appended.size()) +
                      " bytes of unwind descriptors");

  uint64_t IndexFileEnd = 0;
  uint64_t IndexVAEnd = 0;
  if (!checkedAdd(Region.IndexFileOff, Index.size(), IndexFileEnd) ||
      !checkedAdd(Region.IndexVA, Index.size(), IndexVAEnd))
    return patchError("the rewritten .ARM.exidx range overflows");
  if (Region.HasTable) {
    uint64_t CurrentTableFileEnd = 0;
    uint64_t CurrentTableVAEnd = 0;
    if (!checkedAdd(Region.TableFileOff, Region.TableSize,
                    CurrentTableFileEnd) ||
        !checkedAdd(Region.TableVA, Region.TableSize, CurrentTableVAEnd))
      return patchError("the input .ARM.extab range overflows");
    const uint64_t FinalTableFileEnd =
        Appended.empty() ? CurrentTableFileEnd : AppendFileEnd;
    const uint64_t FinalTableVAEnd =
        Appended.empty() ? CurrentTableVAEnd : AppendVAEnd;
    if (FinalTableFileEnd < Region.TableFileOff ||
        FinalTableVAEnd < Region.TableVA ||
        FinalTableVAEnd - Region.TableVA >
            std::numeric_limits<uint32_t>::max() ||
        FinalTableFileEnd - Region.TableFileOff !=
            FinalTableVAEnd - Region.TableVA)
      return patchError("the rewritten .ARM.extab size cannot be encoded");
    const bool FileRangesOverlap = Region.IndexFileOff < FinalTableFileEnd &&
                                   Region.TableFileOff < IndexFileEnd;
    const bool VARangesOverlap =
        Region.IndexVA < FinalTableVAEnd && Region.TableVA < IndexVAEnd;
    if (FileRangesOverlap || VARangesOverlap)
      return patchError(
          "the rewritten .ARM.exidx and .ARM.extab ranges overlap");
  }

  // Everything fits; commit.
  std::memcpy(Binary.data() + Region.IndexFileOff, Index.data(), Index.size());
  growELFSection(Binary, Region.IndexSectionHeaderOff, Index.size());
  if (!Appended.empty()) {
    std::memcpy(Binary.data() + AppendFileOff, Appended.data(),
                Appended.size());
    growELFSection(Binary, Region.TableSectionHeaderOff,
                   AppendVAEnd - Region.TableVA);
  }

  ELFPhdrFields PH = readELFPhdr(Binary.data() + Region.ExidxPhdrOff, kIs64);
  PH.FileSz = Index.size();
  PH.MemSz = Index.size();
  writeELFPhdr(Binary.data() + Region.ExidxPhdrOff, kIs64, PH);
  return llvm::Error::success();
}

bool hasGeneratedELFARMEHABI(const CompiledImage &Compiled) {
  return std::any_of(Compiled.Sections.begin(), Compiled.Sections.end(),
                     isGeneratedIndexSection);
}

llvm::Error installELFARMEHABI(std::vector<uint8_t> &Binary,
                               const std::optional<ELFARMEHABIRegion> &Region,
                               const CompiledImage &Compiled,
                               const llvm::Module &Mod) {
  auto Requirements = exception_rewrite::validateExceptionRewriteContracts(Mod);
  if (!Requirements)
    return Requirements.takeError();
  const bool Required = Requirements->RequiresRegisteredUnwind;
  if (Required && !Compiled.Unresolved.empty())
    return patchError("required unwind output has unresolved symbols: " +
                      llvm::join(Compiled.Unresolved, ", "));
  auto RequiredFunctions = exception_rewrite::resolveRequiredFunctionAddresses(
      *Requirements, Compiled);
  if (!RequiredFunctions)
    return RequiredFunctions.takeError();
  auto reportOrDrop = [&](llvm::Error Err) -> llvm::Error {
    if (!Err || Required)
      return Err;
    const std::string Reason = llvm::toString(std::move(Err));
    LLVM_DEBUG(llvm::dbgs() << "elf arm ehabi patch: " << Reason
                            << "; leaving the index as it was\n");
    return llvm::Error::success();
  };

  std::vector<ELFARMEHABIRecord> Records;
  std::vector<uint64_t> RangeTerminators;
  if (llvm::Error Err =
          collectGeneratedRecords(Compiled, Records, RangeTerminators))
    return reportOrDrop(std::move(Err));
  for (uint64_t Address : *RequiredFunctions) {
    const uint64_t FunctionVA = clearThumbBit(Address);
    if (std::none_of(Records.begin(), Records.end(),
                     [&](const ELFARMEHABIRecord &Record) {
                       return clearThumbBit(Record.FunctionVA) == FunctionVA &&
                              Record.Model != ELFARMEHABIModel::CantUnwind;
                     }))
      return reportOrDrop(patchError(
          "regenerated .ARM.exidx does not cover a required function"));
  }
  if (Records.empty())
    return reportOrDrop(
        patchError("no registrable .ARM.exidx entries produced"));
  if (!Region)
    return reportOrDrop(
        patchError("the image declares no .ARM.exidx to register in"));
  if (llvm::Error Err = validateGeneratedKeysAgainstInput(
          Binary, *Region, RangeTerminators, Records))
    return reportOrDrop(std::move(Err));
  return reportOrDrop(installELFARMEHABIRecords(Binary, *Region, Records));
}

} // namespace neverd
