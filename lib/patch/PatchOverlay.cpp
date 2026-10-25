//===- PatchOverlay.cpp - Interactive byte patches over an image ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Admission, application, reversal and replay of the interactive byte
/// overlay.  Runs are kept canonical: maximal, of one kind, in one segment,
/// every byte different from the input file's, so one set of changes has one
/// set of runs whichever order made it.
///
//===----------------------------------------------------------------------===//

#include "neverd/patch/PatchOverlay.h"

#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/patch/Limits.h"
#include "neverd/support/FilePath.h"
#include "neverd/support/SHA256.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/COFF.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/MachOUniversal.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/MemoryBuffer.h"

#include <algorithm>

using namespace neverd;
using namespace neverd::patch;
namespace patchLimits = ::neverd::limits::patch;

namespace {

llvm::Error patchError(const llvm::Twine &Message) {
  return llvm::make_error<llvm::StringError>(Message,
                                             llvm::inconvertibleErrorCode());
}

std::string hexAddress(uint64_t Address) {
  return "0x" + llvm::utohexstr(Address, /*LowerCase=*/true);
}

/// The bytes of \p Seg that come from the file: its file size, as far as the
/// loader materialized it and the segment maps it.
uint64_t fileBackedSize(const Segment &Seg) {
  return std::min(
      {Seg.FileSz, static_cast<uint64_t>(Seg.Data.size()), Seg.Size});
}

std::optional<size_t> segmentIndex(const BinaryImage &Img, va_t Address) {
  const Segment *Seg = Img.getSegmentFor(Address);
  if (!Seg)
    return std::nullopt;
  return static_cast<size_t>(Seg - Img.Segments.data());
}

/// The width of a PE base relocation's field, zero for padding entries.
unsigned baseRelocationWidth(uint8_t Type, unsigned Pointer) {
  switch (Type) {
  case llvm::COFF::IMAGE_REL_BASED_ABSOLUTE:
    return 0;
  case llvm::COFF::IMAGE_REL_BASED_HIGH:
  case llvm::COFF::IMAGE_REL_BASED_LOW:
    return 2;
  case llvm::COFF::IMAGE_REL_BASED_HIGHLOW:
  case llvm::COFF::IMAGE_REL_BASED_HIGHADJ:
    return 4;
  case llvm::COFF::IMAGE_REL_BASED_DIR64:
    return 8;
  // MOVW/MOVT pairs: two four-byte instructions.
  case llvm::COFF::IMAGE_REL_BASED_ARM_MOV32A:
  case llvm::COFF::IMAGE_REL_BASED_ARM_MOV32T:
    return 8;
  default:
    return Pointer;
  }
}

/// A canonical run builder over one window of file offsets.
std::vector<PatchRow> canonicalRuns(const BinaryImage &Img, size_t Segment,
                                    uint64_t InputBase, uint64_t First,
                                    llvm::ArrayRef<uint8_t> Original,
                                    llvm::ArrayRef<uint8_t> Current,
                                    llvm::ArrayRef<PatchKind> Kinds) {
  const auto &Seg = Img.Segments[Segment];
  std::vector<PatchRow> Runs;
  for (size_t I = 0; I < Current.size();) {
    if (Current[I] == Original[I]) {
      ++I;
      continue;
    }
    size_t J = I + 1;
    while (J < Current.size() && Current[J] != Original[J] &&
           Kinds[J] == Kinds[I])
      ++J;
    PatchRow Row;
    Row.FileOffset = First + I;
    Row.Address = Seg.VA + (Row.FileOffset - InputBase - Seg.FileOff);
    Row.Original.assign(Original.begin() + I, Original.begin() + J);
    Row.Current.assign(Current.begin() + I, Current.begin() + J);
    Row.Kind = Kinds[I];
    Runs.push_back(std::move(Row));
    I = J;
  }
  return Runs;
}

void publishRanges(BinaryImage &Img, const PatchOverlay &Overlay) {
  Img.BytePatchRanges.clear();
  for (const auto &[Offset, Row] : Overlay.rows())
    Img.BytePatchRanges.emplace_back(Row.Address, Row.size());
  if (Overlay.empty())
    Img.BytePatchOriginal.reset();
}

} // namespace

llvm::StringRef patch::patchKindName(PatchKind Kind) {
  switch (Kind) {
#define NEVERD_PATCH_KIND(Id, Spelling)                                        \
  case PatchKind::Id:                                                          \
    return Spelling;
#include "neverd/patch/PatchKinds.def"
  }
  return {};
}

std::optional<PatchKind> patch::parsePatchKind(llvm::StringRef Spelling) {
#define NEVERD_PATCH_KIND(Id, Spelling_)                                       \
  if (Spelling == Spelling_)                                                   \
    return PatchKind::Id;
#include "neverd/patch/PatchKinds.def"
  return std::nullopt;
}

// ===--------------------------------------------------------------------===//
// The input file
// ===--------------------------------------------------------------------===//

llvm::Expected<InputFile> InputFile::open(const std::filesystem::path &Path,
                                          const BinaryImage &Img) {
  if (Img.MachOSharedCache)
    return patchError("a dyld shared cache image spans several files; its "
                      "bytes cannot be patched in place");
  std::error_code EC;
  const auto Size = std::filesystem::file_size(Path, EC);
  if (EC)
    return patchError("cannot read the input file " + pathToUTF8(Path) + ": " +
                      EC.message());
  InputFile File;
  File.Path = Path;
  File.Size = Size;
  File.Permissions = std::filesystem::status(Path, EC).permissions();
  if (EC)
    return llvm::errorCodeToError(EC);
  if (Size > patchLimits::kMaxInputFileBytes)
    return patchError("input exceeds the interactive patch snapshot limit");
  // A universal file holds the image the loader read at its slice's offset.
  auto Buffer = llvm::MemoryBuffer::getFile(pathToUTF8(Path), /*IsText=*/false,
                                            /*RequiresNullTerminator=*/false,
                                            /*IsVolatile=*/true);
  if (!Buffer)
    return patchError("cannot read the input file " + pathToUTF8(Path) + ": " +
                      Buffer.getError().message());
  if ((*Buffer)->getBufferSize() != Size)
    return patchError("input changed while taking its patch snapshot");
  if (Img.InputFileSHA256 &&
      sha256(llvm::arrayRefFromStringRef((*Buffer)->getBuffer())) !=
          *Img.InputFileSHA256)
    return patchError("input changed since it was loaded");
  File.Snapshot = std::move(*Buffer);
  if (Img.Format != BinaryFormat::MachO && Img.Format != BinaryFormat::COFF)
    return File;
  auto Binary = llvm::object::createBinary(File.Snapshot->getMemBufferRef());
  if (!Binary)
    return Binary.takeError();
  const auto SetMachOHeaders =
      [&](const llvm::object::MachOObjectFile &Object) {
        File.HeaderSize = Object.is64Bit()
                              ? uint64_t(sizeof(llvm::MachO::mach_header_64)) +
                                    Object.getHeader64().sizeofcmds
                              : uint64_t(sizeof(llvm::MachO::mach_header)) +
                                    Object.getHeader().sizeofcmds;
      };
  if (const auto *PE =
          llvm::dyn_cast<llvm::object::COFFObjectFile>(Binary->get())) {
    if (const auto *Header = PE->getPE32PlusHeader())
      File.HeaderSize = Header->SizeOfHeaders;
    else if (const auto *Header = PE->getPE32Header())
      File.HeaderSize = Header->SizeOfHeaders;
    return File;
  }
  if (const auto *Object =
          llvm::dyn_cast<llvm::object::MachOObjectFile>(Binary->get())) {
    SetMachOHeaders(*Object);
    return File;
  }
  auto *Universal =
      llvm::dyn_cast<llvm::object::MachOUniversalBinary>(Binary->get());
  if (!Universal)
    return patchError("input no longer has the loaded binary format");
  const auto Chosen = macho_loader::chooseUniversalSlice(*Universal);
  if (!Chosen)
    return patchError("the universal input holds no slice its loader reads");
  size_t Index = 0;
  for (const auto &Slice : Universal->objects())
    if (Index++ == *Chosen) {
      File.Base = Slice.getOffset();
      auto Object = Slice.getAsObjectFile();
      if (!Object)
        return Object.takeError();
      SetMachOHeaders(**Object);
      return File;
    }
  return patchError("the universal input lost the slice its loader read");
}

llvm::Expected<std::vector<uint8_t>> InputFile::read(uint64_t Offset,
                                                     uint64_t Count) const {
  if (Offset > Size || Count > Size - Offset)
    return patchError(llvm::formatv(
        "the input file holds {0} bytes; it has no bytes {1}..+{2}", Size,
        hexAddress(Offset), Count));
  if (Count == 0)
    return std::vector<uint8_t>();
  const auto *Data =
      reinterpret_cast<const uint8_t *>(Snapshot->getBufferStart()) + Offset;
  return std::vector<uint8_t>(Data, Data + Count);
}

// ===--------------------------------------------------------------------===//
// Image bytes and relocated fields
// ===--------------------------------------------------------------------===//

void patch::writeImageBytes(BinaryImage &Img, va_t Address,
                            llvm::ArrayRef<uint8_t> Bytes) {
  const va_t End = Address + Bytes.size();
  const auto copyInto = [&](va_t VA, std::vector<uint8_t> &Data) {
    const va_t First = std::max(Address, VA);
    const va_t Last = VA > InvalidVA - Data.size()
                          ? End
                          : std::min<va_t>(End, VA + Data.size());
    if (First >= Last)
      return;
    std::copy(Bytes.begin() + (First - Address),
              Bytes.begin() + (Last - Address), Data.begin() + (First - VA));
  };
  for (auto &Seg : Img.Segments)
    if (Seg.contains(Address))
      copyInto(Seg.VA, Seg.Data);
  for (auto &Sec : Img.Sections)
    if (!Sec.Data.empty() && Sec.VA < End &&
        (Sec.VA >= Address || Address - Sec.VA < Sec.Data.size()))
      copyInto(Sec.VA, Sec.Data);
}

std::optional<std::string>
patch::relocatedFieldIn(const BinaryImage &Img, va_t Address, uint64_t Size) {
  const va_t End = Address + Size;
  const unsigned Pointer = Img.getPointerSize()
                               ? Img.getPointerSize()
                               : patchLimits::kMaxRelocationFieldBytes;
  const va_t First = Address >= patchLimits::kMaxRelocationFieldBytes - 1
                         ? Address - (patchLimits::kMaxRelocationFieldBytes - 1)
                         : 0;
  const auto reaches = [&](va_t Slot, unsigned Width) {
    return Width && Slot < End && (Slot >= Address || Address - Slot < Width);
  };
  const auto field = [](llvm::StringRef What, va_t Slot) {
    return (What + " at " + hexAddress(Slot)).str();
  };
  const std::pair<const std::set<uint64_t> *, std::pair<unsigned, const char *>>
      Slots[] = {
          {&Img.CodePtrRelocSlots, {Pointer, "a code pointer relocation"}},
          {&Img.DataPtrRelocSlots, {Pointer, "a data pointer relocation"}},
          {&Img.RelCodeRelocSlots, {4, "a relative code relocation"}},
          {&Img.RelDataPtrRelocSlots, {4, "a relative data relocation"}}};
  for (const auto &[Set, Kind] : Slots)
    for (auto It = Set->lower_bound(First); It != Set->end() && *It < End; ++It)
      if (reaches(*It, Kind.first))
        return field(Kind.second, *It);
  for (auto It = Img.MachOAuthenticatedPointers.lower_bound(First);
       It != Img.MachOAuthenticatedPointers.end() && It->first < End; ++It)
    if (reaches(It->first, 8))
      return field("an authenticated pointer fixup", It->first);
  for (auto It = Img.ImportStorageSlots.lower_bound(First);
       It != Img.ImportStorageSlots.end() && It->first < End; ++It)
    if (reaches(It->first, Pointer) &&
        Img.isValidImportStorageSlot(It->first, It->second.Name))
      return field("the import slot of " + It->second.Name, It->first);
  for (auto It = Img.ImportPtrSlots.lower_bound(First);
       It != Img.ImportPtrSlots.end() && It->first < End; ++It)
    if (reaches(It->first, Pointer) &&
        Img.isValidImportStorageSlot(It->first, It->second))
      return field("the import slot of " + It->second, It->first);
  for (auto It = Img.DyldBindSlots.lower_bound(First);
       It != Img.DyldBindSlots.end() && It->first < End; ++It)
    if (reaches(It->first, Pointer) &&
        Img.isValidImportStorageSlot(It->first, It->second.Name))
      return field("the bind slot of " + It->second.Name, It->first);
  for (const auto &Reloc : Img.BaseRelocations)
    if (reaches(Reloc.Address, baseRelocationWidth(Reloc.Type, Pointer)))
      return field("a base relocation", Reloc.Address);
  for (const auto &Reloc : Img.Relocations)
    if (reaches(Reloc.Address, Pointer))
      return field(Reloc.SymbolName.empty()
                       ? std::string("a relocation")
                       : "a relocation against " + Reloc.SymbolName,
                   Reloc.Address);
  return std::nullopt;
}

// ===--------------------------------------------------------------------===//
// The overlay
// ===--------------------------------------------------------------------===//

llvm::Expected<WriteTarget> PatchOverlay::admit(const BinaryImage &Img,
                                                const InputFile &Input,
                                                va_t Address,
                                                uint64_t Size) const {
  if (Size == 0)
    return patchError("a patch changes at least one byte");
  if (Size > patchLimits::kMaxPatchedBytes)
    return patchError(llvm::formatv("a patch changes at most {0} bytes at once",
                                    patchLimits::kMaxPatchedBytes));
  if (Address > InvalidVA - Size)
    return patchError("the patch passes the end of the address space");
  const auto Index = segmentIndex(Img, Address);
  if (!Index)
    return patchError(hexAddress(Address) + " is in no segment of the image");
  const Segment &Seg = Img.Segments[*Index];
  const uint64_t Delta = Address - Seg.VA;
  const uint64_t Backed = fileBackedSize(Seg);
  if (Delta >= Backed || Size > Backed - Delta)
    return patchError(llvm::formatv(
        "{0}..+{1} has no file bytes: {2} holds {3} bytes of the file from "
        "{4}, and memory the loader zero-fills has no input byte to patch",
        hexAddress(Address), Size,
        Seg.Name.empty() ? std::string("its segment") : Seg.Name, Backed,
        hexAddress(Seg.VA)));
  if (auto Field = relocatedFieldIn(Img, Address, Size))
    return patchError(*Field +
                      " reaches into the patch; the loader writes that field "
                      "when it loads the image, over any patch");
  if (Seg.FileOff > UINT64_MAX - Input.base() ||
      Delta > UINT64_MAX - Input.base() - Seg.FileOff)
    return patchError("the patch's file offset overflows");
  const uint64_t FileOffset = Input.base() + Seg.FileOff + Delta;
  if (Input.overlapsLoadHeaders(FileOffset, Size))
    return patchError("patch overlaps binary load headers");
  if (Img.ELFMetadata) {
    const auto Hits = [&](uint64_t At, uint64_t Length) {
      const auto Offset = FileOffset - Input.base();
      return Length &&
             (Offset <= At ? At - Offset < Size : Offset - At < Length);
    };
    const auto &M = *Img.ELFMetadata;
    if (Hits(0, M.HeaderSize) ||
        Hits(M.ProgramHeaderFileOffset,
             uint64_t(M.ProgramHeaderEntrySize) * M.ProgramHeaders.size()))
      return patchError("patch overlaps ELF load headers");
    for (const auto &P : M.ProgramHeaders)
      if ((P.Type == llvm::ELF::PT_INTERP || P.Type == llvm::ELF::PT_DYNAMIC) &&
          Hits(P.FileOffset, P.FileSize))
        return patchError(
            "patch overlaps ELF interpreter or dynamic loader metadata");
  }
  // An offset or VA with another mapping has no unique replay target. Refuse
  // it even when the two copies currently happen to contain equal bytes.
  for (size_t I = 0; I < Img.Segments.size(); ++I) {
    if (I == *Index)
      continue;
    const auto &Other = Img.Segments[I];
    const uint64_t OtherSize = fileBackedSize(Other);
    const auto Overlaps = [](uint64_t A, uint64_t N, uint64_t B, uint64_t M) {
      return N && M && (A <= B ? B - A < N : A - B < M);
    };
    if (Overlaps(Address, Size, Other.VA, OtherSize) ||
        Overlaps(FileOffset - Input.base(), Size, Other.FileOff, OtherSize))
      return patchError("patch bytes have ambiguous segment mappings");
  }
  auto FileBytes = Input.read(FileOffset, Size);
  if (!FileBytes)
    return FileBytes.takeError();
  std::vector<uint8_t> Unpatched(Seg.Data.begin() + Delta,
                                 Seg.Data.begin() + Delta + Size);
  restoreOriginal(Address, Unpatched);
  if (Unpatched != *FileBytes) {
    const auto Mismatch =
        std::mismatch(Unpatched.begin(), Unpatched.end(), FileBytes->begin());
    const uint64_t At = Mismatch.first - Unpatched.begin();
    return patchError(llvm::formatv(
        "the image's byte at {0} ({1:x2}) is not the input file's ({2:x2} at "
        "file offset {3}): the loader rewrote it, as relocations do, so no "
        "input byte remains to restore",
        hexAddress(Address + At), *Mismatch.first, *Mismatch.second,
        hexAddress(FileOffset + At)));
  }
  return WriteTarget{*Index, FileOffset};
}

llvm::Error PatchOverlay::apply(BinaryImage &Img, const InputFile &Input,
                                va_t Address, llvm::ArrayRef<uint8_t> NewBytes,
                                PatchKind Kind) {
  if (NewBytes.size() > patchLimits::kMaxApplyBytes)
    return patchError("patch request exceeds the per-apply byte limit");
  auto Target = admit(Img, Input, Address, NewBytes.size());
  if (!Target)
    return Target.takeError();
  const Segment &Seg = Img.Segments[Target->Segment];
  const uint64_t Lo = Target->FileOffset, Hi = Lo + NewBytes.size();
  // Canonical runs already merge equal-kind neighbours. Only overlaps and
  // same-kind neighbours at the write's edges can change: extending through
  // every adjacent run would repeatedly scan an unrelated mixed-kind chain.
  uint64_t First = Lo, End = Hi;
  std::vector<uint64_t> Touched;
  auto It = Rows.lower_bound(Lo);
  if (It != Rows.begin()) {
    const auto Before = std::prev(It);
    const auto &Row = Before->second;
    if (Row.FileOffset + Row.size() > Lo ||
        (Row.FileOffset + Row.size() == Lo && Row.Kind == Kind))
      It = Before;
  }
  for (; It != Rows.end(); ++It) {
    const PatchRow &Row = It->second;
    if (Row.FileOffset > Hi || (Row.FileOffset == Hi && Row.Kind != Kind))
      break;
    if (segmentIndex(Img, Row.Address) != Target->Segment)
      continue;
    Touched.push_back(Row.FileOffset);
    First = std::min(First, Row.FileOffset);
    End = std::max<uint64_t>(End, Row.FileOffset + Row.size());
  }
  const uint64_t Width = End - First;
  const va_t WindowVA = Seg.VA + (First - Input.base() - Seg.FileOff);
  const auto *ImageBytes = Seg.Data.data() + (WindowVA - Seg.VA);
  std::vector<uint8_t> Original(ImageBytes, ImageBytes + Width);
  std::vector<uint8_t> Current = Original;
  std::vector<PatchKind> Kinds(Width, Kind);
  uint64_t Replaced = 0;
  for (uint64_t Offset : Touched) {
    const PatchRow &Row = Rows.at(Offset);
    std::copy(Row.Original.begin(), Row.Original.end(),
              Original.begin() + (Row.FileOffset - First));
    std::copy(Row.Current.begin(), Row.Current.end(),
              Current.begin() + (Row.FileOffset - First));
    std::fill_n(Kinds.begin() + (Row.FileOffset - First), Row.size(), Row.Kind);
    Replaced += Row.size();
  }
  std::copy(NewBytes.begin(), NewBytes.end(), Current.begin() + (Lo - First));
  std::fill_n(Kinds.begin() + (Lo - First), NewBytes.size(), Kind);
  auto Runs = canonicalRuns(Img, Target->Segment, Input.base(), First, Original,
                            Current, Kinds);
  uint64_t Changed = 0;
  for (const auto &Run : Runs)
    Changed += Run.size();
  if (Bytes - Replaced + Changed > patchLimits::kMaxPatchedBytes)
    return patchError(llvm::formatv("a project holds at most {0} patched bytes",
                                    patchLimits::kMaxPatchedBytes));
  if (Rows.size() - Touched.size() + Runs.size() > patchLimits::kMaxPatchRows)
    return patchError(
        llvm::formatv("a project holds at most {0} separate patches",
                      patchLimits::kMaxPatchRows));
  if (!Img.BytePatchOriginal)
    Img.BytePatchOriginal = std::make_shared<BinaryImage>(Img);
  for (uint64_t Offset : Touched) {
    ByAddress.erase(Rows.at(Offset).Address);
    Rows.erase(Offset);
  }
  for (auto &Run : Runs) {
    ByAddress[Run.Address] = Run.FileOffset;
    Rows.emplace(Run.FileOffset, std::move(Run));
  }
  Bytes = Bytes - Replaced + Changed;
  writeImageBytes(Img, Address, NewBytes);
  publishRanges(Img, *this);
  return llvm::Error::success();
}

llvm::Expected<uint64_t> PatchOverlay::revert(BinaryImage &Img, va_t Address,
                                              uint64_t Size) {
  if (Size == 0) {
    const auto Holding = rowsIn(Address, 1);
    if (Holding.empty())
      return patchError("nothing is patched at " + hexAddress(Address));
    Address = Holding.front()->Address;
    Size = Holding.front()->size();
  }
  if (Address > InvalidVA - Size)
    return patchError("the range passes the end of the address space");
  const va_t End = Address + Size;
  std::vector<PatchRow> Pieces;
  std::vector<uint64_t> Reverted;
  uint64_t Restored = 0;
  for (const PatchRow *Row : rowsIn(Address, Size)) {
    Reverted.push_back(Row->FileOffset);
    const va_t From = std::max(Address, Row->Address);
    const va_t To = std::min(End, Row->end());
    Restored += To - From;
    // What stays changed of the run on either side of the range.
    if (Row->Address < From) {
      PatchRow Left = *Row;
      Left.Original.resize(From - Row->Address);
      Left.Current.resize(From - Row->Address);
      Pieces.push_back(std::move(Left));
    }
    if (To < Row->end()) {
      PatchRow Right;
      Right.FileOffset = Row->FileOffset + (To - Row->Address);
      Right.Address = To;
      Right.Original.assign(Row->Original.begin() + (To - Row->Address),
                            Row->Original.end());
      Right.Current.assign(Row->Current.begin() + (To - Row->Address),
                           Row->Current.end());
      Right.Kind = Row->Kind;
      Pieces.push_back(std::move(Right));
    }
  }
  if (Reverted.empty())
    return patchError(llvm::formatv("nothing is patched in {0}..+{1}",
                                    hexAddress(Address), Size));
  if (Rows.size() - Reverted.size() + Pieces.size() >
      patchLimits::kMaxPatchRows)
    return patchError("reverting this range exceeds the patch row limit");
  for (uint64_t Offset : Reverted) {
    const PatchRow Row = std::move(Rows.at(Offset));
    Rows.erase(Offset);
    ByAddress.erase(Row.Address);
    Bytes -= Row.size();
    const va_t From = std::max(Address, Row.Address);
    const va_t To = std::min(End, Row.end());
    writeImageBytes(Img, From,
                    llvm::ArrayRef<uint8_t>(Row.Original)
                        .slice(From - Row.Address, To - From));
  }
  for (auto &Piece : Pieces) {
    Bytes += Piece.size();
    ByAddress[Piece.Address] = Piece.FileOffset;
    Rows.emplace(Piece.FileOffset, std::move(Piece));
  }
  publishRanges(Img, *this);
  return Restored;
}

void PatchOverlay::clear(BinaryImage &Img) {
  for (const auto &[Offset, Row] : Rows)
    writeImageBytes(Img, Row.Address, Row.Original);
  Rows.clear();
  ByAddress.clear();
  Bytes = 0;
  publishRanges(Img, *this);
}

llvm::Error PatchOverlay::replace(BinaryImage &Img, const InputFile &Input,
                                  std::vector<PatchRow> NewRows) {
  llvm::sort(NewRows, [](const PatchRow &L, const PatchRow &R) {
    return L.FileOffset < R.FileOffset;
  });
  if (NewRows.size() > patchLimits::kMaxPatchRows)
    return patchError(
        llvm::formatv("a project holds at most {0} separate patches",
                      patchLimits::kMaxPatchRows));
  // Validate every row against the image as the input file loaded it.
  struct Placed {
    size_t Segment;
    const PatchRow *Row;
  };
  std::vector<Placed> Placement;
  uint64_t Total = 0;
  for (const PatchRow &Row : NewRows) {
    const auto Where = [&] {
      return " (row at " + hexAddress(Row.Address) + ")";
    };
    if (Row.Current.empty() || Row.Original.size() != Row.Current.size())
      return patchError("a patch row needs original and current bytes of "
                        "equal, non-zero length" +
                        Where());
    Total += Row.size();
    if (Total > patchLimits::kMaxPatchedBytes)
      return patchError(
          llvm::formatv("a project holds at most {0} patched bytes",
                        patchLimits::kMaxPatchedBytes));
    auto Target = admit(Img, Input, Row.Address, Row.size());
    if (!Target)
      return patchError(llvm::toString(Target.takeError()) + Where());
    if (Target->FileOffset != Row.FileOffset)
      return patchError(llvm::formatv(
          "the row's address maps to file offset {0}, not {1}: the patches "
          "describe another build or slice of the input{2}",
          hexAddress(Target->FileOffset), hexAddress(Row.FileOffset), Where()));
    auto FileBytes = Input.read(Row.FileOffset, Row.size());
    if (!FileBytes)
      return FileBytes.takeError();
    if (*FileBytes != Row.Original)
      return patchError("the input file's bytes are not the row's original "
                        "bytes: the input changed since the patch was made" +
                        Where());
    if (!Placement.empty()) {
      const PatchRow &Last = *Placement.back().Row;
      if (Last.FileOffset + Last.size() > Row.FileOffset)
        return patchError("patch rows overlap" + Where());
    }
    Placement.push_back({Target->Segment, &Row});
  }
  // Canonical runs from the rows, merging neighbours of one kind and
  // dropping bytes that equal the file's.
  std::vector<PatchRow> Runs;
  for (size_t I = 0; I < Placement.size();) {
    size_t J = I + 1;
    while (J < Placement.size() &&
           Placement[J].Segment == Placement[I].Segment &&
           Placement[J - 1].Row->FileOffset + Placement[J - 1].Row->size() ==
               Placement[J].Row->FileOffset)
      ++J;
    const uint64_t First = Placement[I].Row->FileOffset;
    std::vector<uint8_t> Original, Current;
    std::vector<PatchKind> Kinds;
    for (size_t K = I; K < J; ++K) {
      const PatchRow &Row = *Placement[K].Row;
      llvm::append_range(Original, Row.Original);
      llvm::append_range(Current, Row.Current);
      Kinds.insert(Kinds.end(), Row.size(), Row.Kind);
    }
    for (auto &Run : canonicalRuns(Img, Placement[I].Segment, Input.base(),
                                   First, Original, Current, Kinds))
      Runs.push_back(std::move(Run));
    I = J;
  }
  if (Runs.size() > patchLimits::kMaxPatchRows)
    return patchError("normalized patches exceed the patch row limit");
  auto OriginalImage = Img.BytePatchOriginal;
  if (!Runs.empty() && !OriginalImage)
    OriginalImage = std::make_shared<BinaryImage>(Img);
  clear(Img);
  Img.BytePatchOriginal = std::move(OriginalImage);
  for (auto &Run : Runs) {
    writeImageBytes(Img, Run.Address, Run.Current);
    Bytes += Run.size();
    ByAddress[Run.Address] = Run.FileOffset;
    Rows.emplace(Run.FileOffset, std::move(Run));
  }
  publishRanges(Img, *this);
  return llvm::Error::success();
}

std::vector<const PatchRow *> PatchOverlay::rowsIn(va_t Address,
                                                   uint64_t Size) const {
  std::vector<const PatchRow *> Found;
  if (ByAddress.empty() || Size == 0)
    return Found;
  const va_t End = Address > InvalidVA - Size ? InvalidVA : Address + Size;
  auto It = ByAddress.upper_bound(Address);
  if (It != ByAddress.begin()) {
    auto Before = std::prev(It);
    const PatchRow &Row = Rows.at(Before->second);
    if (Row.end() > Address)
      Found.push_back(&Row);
  }
  for (; It != ByAddress.end() && It->first < End; ++It)
    Found.push_back(&Rows.at(It->second));
  return Found;
}

std::optional<uint8_t> PatchOverlay::originalByte(va_t Address) const {
  const auto Holding = rowsIn(Address, 1);
  if (Holding.empty())
    return std::nullopt;
  return Holding.front()->Original[Address - Holding.front()->Address];
}

void PatchOverlay::restoreOriginal(va_t Address,
                                   llvm::MutableArrayRef<uint8_t> Data) const {
  const va_t End = Address + Data.size();
  for (const PatchRow *Row : rowsIn(Address, Data.size())) {
    const va_t From = std::max(Address, Row->Address);
    const va_t To = std::min(End, Row->end());
    std::copy(Row->Original.begin() + (From - Row->Address),
              Row->Original.begin() + (To - Row->Address),
              Data.begin() + (From - Address));
  }
}
