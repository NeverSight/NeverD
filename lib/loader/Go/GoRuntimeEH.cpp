//===- GoRuntimeEH.cpp - Go runtime frame metadata -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/Go/GoRuntimeEH.h"

#include "neverd/Object/SectionNames.h"
#include "neverd/Support/BinaryEncoding.h"
#include "neverd/loader/DirectBranch.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace neverd::go_loader {
namespace {

/// `pcHeader.magic`.  The value both identifies the table and, because the
/// four bytes are not a palindrome, proves the byte order it was written in.
constexpr uint32_t Go12Magic = 0xFFFFFFFBu;  // Go 1.2 - 1.15
constexpr uint32_t Go116Magic = 0xFFFFFFFAu; // Go 1.16 - 1.17
constexpr uint32_t Go118Magic = 0xFFFFFFF0u; // Go 1.18 - 1.19
constexpr uint32_t Go120Magic = 0xFFFFFFF1u; // Go 1.20 and later

/// `internal/abi.FUNCDATA_OpenCodedDeferInfo`.
constexpr unsigned FuncDataOpenCodedDeferInfo = 4;
/// Where the same table sat before Go 1.16 renumbered the array.  Reading it
/// on an older image is safe in both directions: no release that spans the Go
/// 1.2 magic and predates open-coded defers ever emitted six funcdata entries,
/// so index 5 is out of range there rather than pointing at something else.
constexpr unsigned FuncDataOpenCodedDeferInfoPreGo116 = 5;
/// `internal/abi.FUNCDATA_ArgsPointerMaps`, which has been index 0 since the
/// array existed.
constexpr unsigned FuncDataArgsPointerMaps = 0;
/// `internal/abi.FUNCDATA_LocalsPointerMaps`, likewise fixed at index 1.
constexpr unsigned FuncDataLocalsPointerMaps = 1;
/// The sentinel `_func.funcdata[i]` carries when the entry is absent.
constexpr uint32_t NoFuncDataOffset = 0xFFFFFFFFu;

/// `internal/abi.PCDATA_UnsafePoint`.  The table is Go 1.16 and later only:
/// index 0 held a register-map index before that, and the async preemption
/// this table describes did not exist to be described.
constexpr unsigned PCDataUnsafePoint = 0;
/// `internal/abi.PCDATA_StackMapIndex`.  Go 1.13 moved it here from index 0,
/// which the Go 1.2 magic does not distinguish, so on that layout the position
/// is proven from the pointer maps rather than assumed.
constexpr unsigned PCDataStackMapIndex = 1;
constexpr unsigned PCDataStackMapIndexPreGo113 = 0;

/// `internal/abi.UnsafePoint*`.
constexpr int32_t UnsafePointSafe = -1;
constexpr int32_t UnsafePointUnsafe = -2;
constexpr int32_t UnsafePointRestart1 = -3;
constexpr int32_t UnsafePointRestart2 = -4;
constexpr int32_t UnsafePointRestartAtEntry = -5;

/// Bounds.  Each is far above anything a real Go link produces and far below
/// what would let a mis-identified table run the decoder out of time.
constexpr uint64_t MaxFunctionCount = 1u << 22;
constexpr uint32_t MaxPCDataTables = 64;
constexpr uint8_t MaxFuncDataTables = 16;
constexpr size_t MaxSymbolNameLength = 4096;
constexpr uint64_t MaxFrameSize = 1u << 24;
constexpr uint64_t MaxFrameSlotOffset = 1u << 20;
constexpr unsigned MaxTextSections = 64;
/// How far past `moduledata.text` the funcdata base is searched for.  The
/// fields between them are a handful of segment bounds whose count has grown
/// by a few words per release; this covers every layout since Go 1.16 with
/// room to spare.
constexpr unsigned MaxModuleDataSearchWords = 48;
/// Candidate funcdata bases are confirmed against decoded records.  More
/// samples cost nothing and make a coincidental match implausible.
constexpr unsigned FuncDataBaseSampleTarget = 16;
/// Bound on the number of steps taken while walking one pc-value table.
constexpr unsigned MaxPCValueSteps = 1u << 16;
/// Bound on how many ranges one pc-value table contributes to a record.  A
/// table can legitimately have more steps than this on a very large function,
/// in which case the record is truncated and marked rather than grown without
/// limit.
constexpr unsigned MaxPCValueRanges = 1u << 12;
/// How many functions the Go 1.2 stack-map-index probe examines before it
/// settles.  Only functions whose locals map has more than one bitmap say
/// anything, so the target is a count of those rather than of all functions.
constexpr unsigned StackMapProbeTarget = 64;
/// How many records the Go 1.2 `_func` shape vote examines.
constexpr unsigned FuncLayoutVoteTarget = 256;

const char *getMagicVersionName(uint32_t Magic) {
  switch (Magic) {
  case Go12Magic:
    return "go1.2";
  case Go116Magic:
    return "go1.16";
  case Go118Magic:
    return "go1.18";
  case Go120Magic:
    return "go1.20";
  default:
    return "unknown";
  }
}

bool isKnownMagic(uint32_t Magic) {
  return Magic == Go12Magic || Magic == Go116Magic || Magic == Go118Magic ||
         Magic == Go120Magic;
}

//===----------------------------------------------------------------------===//
// Checked reads
//===----------------------------------------------------------------------===//

/// Every read of the image goes through this so that a table which claims an
/// address outside the image fails the read instead of being followed.
class ImageReader {
public:
  explicit ImageReader(const BinaryImage &Img)
      : Img(Img), Is64(Img.is64Bit()), PtrSize(Img.is64Bit() ? 8 : 4) {}

  unsigned pointerSize() const { return PtrSize; }

  std::optional<uint8_t> u8(va_t VA) const {
    if (const uint8_t *P = Img.readVA(VA, 1))
      return *P;
    return std::nullopt;
  }

  std::optional<uint32_t> u32(va_t VA) const {
    if (const uint8_t *P = Img.readVA(VA, 4))
      return readLE<uint32_t>(P);
    return std::nullopt;
  }

  std::optional<int32_t> i32(va_t VA) const {
    if (std::optional<uint32_t> V = u32(VA))
      return static_cast<int32_t>(*V);
    return std::nullopt;
  }

  /// A pointer-sized field, which in `pcHeader` and `moduledata` is how every
  /// offset and base is spelled.
  std::optional<uint64_t> word(va_t VA) const {
    if (const uint8_t *P = Img.readVA(VA, PtrSize))
      return readPtr(P, Is64);
    return std::nullopt;
  }

  /// \p Index-th pointer-sized field of a structure starting at \p Base.
  std::optional<uint64_t> wordAt(va_t Base, unsigned Index) const {
    if (Index > (InvalidVA - Base) / PtrSize)
      return std::nullopt;
    return word(Base + Index * PtrSize);
  }

  /// An LEB128-style unsigned varint, as `runtime.readvarint` decodes it.
  /// Returns the value and advances \p VA past the encoding.
  std::optional<uint32_t> uvarint(va_t &VA) const {
    uint32_t Value = 0;
    unsigned Shift = 0;
    for (unsigned I = 0; I < 5; ++I) {
      std::optional<uint8_t> Byte = u8(VA + I);
      if (!Byte)
        return std::nullopt;
      Value |= static_cast<uint32_t>(*Byte & 0x7F) << Shift;
      if ((*Byte & 0x80) == 0) {
        VA += I + 1;
        return Value;
      }
      Shift += 7;
    }
    return std::nullopt;
  }

  /// A NUL-terminated symbol name.  Returns nullopt rather than a truncated
  /// name when no terminator is mapped, because a name that runs off the end
  /// of the section means the offset was wrong.
  std::optional<std::string> cstring(va_t VA) const {
    std::string Result;
    for (size_t I = 0; I < MaxSymbolNameLength; ++I) {
      std::optional<uint8_t> Byte = u8(VA + I);
      if (!Byte)
        return std::nullopt;
      if (*Byte == 0)
        return Result;
      Result.push_back(static_cast<char>(*Byte));
    }
    return std::nullopt;
  }

  /// A fully mapped run of bytes, or nullptr when any of it is not mapped.
  /// Used for the bitmaps, which are the only payload here that is read as
  /// bytes rather than as fields.
  const uint8_t *bytes(va_t VA, size_t Size) const {
    return Img.readVA(VA, Size);
  }

  bool isMapped(va_t VA) const { return Img.readVA(VA, 1) != nullptr; }

  bool isMappedCode(va_t VA) const {
    const Segment *Seg = Img.getSegmentFor(VA);
    return Seg && Seg->isExecutable() && isMapped(VA);
  }

private:
  const BinaryImage &Img;
  bool Is64 = true;
  unsigned PtrSize = 8;
};

//===----------------------------------------------------------------------===//
// pcHeader
//===----------------------------------------------------------------------===//

/// The decoded `runtime.pcHeader`.  Every field after the first eight bytes is
/// pointer sized, and each offset is relative to the header itself, which is
/// what makes the table position independent.
struct PcHeader {
  va_t VA = 0;
  uint32_t Magic = 0;
  uint8_t MinLC = 0;
  uint8_t PtrSize = 0;
  uint64_t FuncCount = 0;
  uint64_t FileCount = 0;
  /// Present from Go 1.18; the linker stopped populating it in a later release
  /// because it needed a relocation, so zero here is normal and means the base
  /// must come from `moduledata` instead.
  va_t TextStart = 0;
  va_t FuncNameTab = 0;
  va_t CuTab = 0;
  va_t FileTab = 0;
  va_t PcTab = 0;
  va_t FuncTab = 0;
  /// Base that a `functab` entry's second word is an offset from.  It is the
  /// `functab` itself from Go 1.16, where the linker made the two tables one
  /// symbol, but the header itself on the Go 1.2 layout, where every offset in
  /// the table is measured from the start of the whole thing.
  va_t FuncRecordBase = 0;
};

/// Check the invariants `runtime.moduledataverify1` checks, which is what lets
/// a scan tell a real header from four bytes that happen to match the magic.
bool isPlausibleHeaderAt(const ImageReader &R, const BinaryImage &Img,
                         va_t VA) {
  std::optional<uint32_t> Magic = R.u32(VA);
  if (!Magic || !isKnownMagic(*Magic))
    return false;
  std::optional<uint8_t> Pad1 = R.u8(VA + 4);
  std::optional<uint8_t> Pad2 = R.u8(VA + 5);
  std::optional<uint8_t> MinLC = R.u8(VA + 6);
  std::optional<uint8_t> PtrSize = R.u8(VA + 7);
  if (!Pad1 || !Pad2 || !MinLC || !PtrSize)
    return false;
  if (*Pad1 != 0 || *Pad2 != 0)
    return false;
  // `sys.PCQuantum`: 1 on x86, 4 on the fixed-width RISC targets, 2 on Thumb.
  if (*MinLC != 1 && *MinLC != 2 && *MinLC != 4)
    return false;
  return *PtrSize == (Img.is64Bit() ? 8 : 4);
}

/// The Go 1.2 table is a different structure rather than a shorter one: after
/// the function count there is no offset block at all, and the sub-tables are
/// reached by walking.  Everything an offset in this table names -- a symbol
/// name, a pc-value table, a `_func` record, a file name -- is measured from
/// the start of the header, so the four bases below are all the header itself
/// and only the file table has to be found by walking to it.
std::optional<PcHeader> decodeGo12PcHeader(const ImageReader &R, PcHeader &H) {
  const va_t VA = H.VA;
  H.FuncNameTab = VA;
  H.PcTab = VA;
  H.FuncRecordBase = VA;

  std::optional<uint64_t> FuncCount = R.word(VA + 8);
  if (!FuncCount || *FuncCount > MaxFunctionCount)
    return std::nullopt;
  H.FuncCount = *FuncCount;
  H.FuncTab = VA + 8 + R.pointerSize();

  // `functab` is `[nfunc+1]{entry, funcoff}` of pointer-sized words, but the
  // trailing element is only half a pair: its `entry` closes the last real
  // function and its `funcoff` slot holds the file table's offset instead.
  const uint64_t FuncTabWords = 2 * H.FuncCount + 1;
  if (FuncTabWords > (InvalidVA - H.FuncTab) / R.pointerSize())
    return std::nullopt;
  const va_t FileTabSlot = H.FuncTab + FuncTabWords * R.pointerSize();
  std::optional<uint32_t> FileTabOffset = R.u32(FileTabSlot);
  if (!FileTabOffset || *FileTabOffset == 0 || *FileTabOffset > InvalidVA - VA)
    return std::nullopt;
  H.FileTab = VA + *FileTabOffset;
  std::optional<uint32_t> FileCount = R.u32(H.FileTab);
  if (!FileCount)
    return std::nullopt;
  H.FileCount = *FileCount;
  return H;
}

std::optional<PcHeader> decodePcHeader(const ImageReader &R, va_t VA) {
  PcHeader H;
  H.VA = VA;
  std::optional<uint32_t> Magic = R.u32(VA);
  std::optional<uint8_t> MinLC = R.u8(VA + 6);
  std::optional<uint8_t> PtrSize = R.u8(VA + 7);
  if (!Magic || !MinLC || !PtrSize)
    return std::nullopt;
  H.Magic = *Magic;
  H.MinLC = *MinLC;
  H.PtrSize = *PtrSize;

  if (H.Magic == Go12Magic)
    return decodeGo12PcHeader(R, H);

  // Fields after the fixed eight-byte prefix, in pointer-sized words.
  const va_t Fields = VA + 8;
  auto field = [&](unsigned Index) { return R.wordAt(Fields, Index); };

  std::optional<uint64_t> FuncCount = field(0);
  std::optional<uint64_t> FileCount = field(1);
  if (!FuncCount || !FileCount || *FuncCount > MaxFunctionCount)
    return std::nullopt;
  H.FuncCount = *FuncCount;
  H.FileCount = *FileCount;

  // Go 1.18 inserted `textStart` ahead of the offset block.
  const unsigned OffsetBase = H.Magic == Go116Magic ? 2 : 3;
  if (H.Magic != Go116Magic) {
    if (std::optional<uint64_t> TextStart = field(2))
      H.TextStart = static_cast<va_t>(*TextStart);
  }

  auto resolve = [&](unsigned Index, va_t &Out) {
    std::optional<uint64_t> Offset = field(OffsetBase + Index);
    if (!Offset || *Offset > InvalidVA - VA)
      return false;
    Out = VA + static_cast<va_t>(*Offset);
    return R.isMapped(Out);
  };
  if (!resolve(0, H.FuncNameTab) || !resolve(1, H.CuTab) ||
      !resolve(2, H.FileTab) || !resolve(3, H.PcTab) || !resolve(4, H.FuncTab))
    return std::nullopt;
  H.FuncRecordBase = H.FuncTab;
  return H;
}

/// Find the `pcHeader`.  ELF and Mach-O linkers give the table its own
/// section, but the PE linker folds it into `.rdata`, so a scan of the mapped
/// data is the only way to find it there.  The scan is cheap because the
/// header is pointer aligned and the magic is checked before anything else.
std::optional<PcHeader> findPcHeader(const ImageReader &R,
                                     const BinaryImage &Img) {
  static constexpr const char *SectionNames[] = {
      section_names::elf::GoPclnTab,
      section_names::macho::GoPclnTab,
      ".data.rel.ro.gopclntab",
  };
  for (const char *Name : SectionNames)
    if (const Section *Sec = Img.getSectionByName(Name))
      if (Sec->VA != 0 && isPlausibleHeaderAt(R, Img, Sec->VA))
        if (std::optional<PcHeader> H = decodePcHeader(R, Sec->VA))
          return H;

  // Executable segments are searched too.  Mach-O puts read-only data in
  // `__TEXT`, so on that format the table lives in a segment an
  // ELF-shaped assumption would skip.
  const unsigned Step = R.pointerSize();
  for (const Segment &Seg : Img.Segments) {
    if (Seg.Data.empty())
      continue;
    const uint64_t Limit = std::min<uint64_t>(Seg.Size, Seg.Data.size());
    if (Limit < 8)
      continue;
    for (uint64_t Off = 0; Off + 8 <= Limit; Off += Step) {
      if (!isKnownMagic(readLE<uint32_t>(Seg.Data.data() + Off)))
        continue;
      const va_t VA = Seg.VA + Off;
      if (!isPlausibleHeaderAt(R, Img, VA))
        continue;
      if (std::optional<PcHeader> H = decodePcHeader(R, VA))
        return H;
    }
  }
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// functab and _func
//===----------------------------------------------------------------------===//

/// Layout of `runtime._func`, which has grown twice in ways that move every
/// field after the growth point.
struct FuncLayout {
  /// Byte size of the fixed part, i.e. where the pcdata array starts.
  unsigned HeaderSize = 0;
  unsigned FuncIDOffset = 0;
  /// True when `functab` and `_func` name code by offset from the text base
  /// rather than by absolute address.  Go 1.18 made this change so that the
  /// table needs no relocations.
  bool EntryIsOffset = true;
  /// True when the trailing funcdata array holds relocated pointers rather
  /// than offsets from `moduledata.gofunc`.  Go 1.18 moved the payloads into
  /// one `go:func.*` symbol and shrank the array to 32-bit offsets; before
  /// that each entry was a pointer, and on a 64-bit target the array was
  /// aligned up to a pointer boundary first.
  bool FuncDataIsPointer = false;
  /// Byte size of one `functab` entry.
  unsigned FuncTabEntrySize = 8;
  /// True for the record shape Go used before 1.12, in which `nfuncdata` is a
  /// full word at `FuncIDOffset` and the three fields later releases put
  /// around it -- `deferreturn`, `funcID`, and `flag` -- do not exist.  The
  /// word where `deferreturn` later went held the frame size the linker used
  /// before `pcsp` replaced it, so reading it as a code offset would name an
  /// address inside a completely unrelated function.  Only the Go 1.2 magic
  /// can carry this shape, and only a vote over the records decides it,
  /// because the magic covers both shapes.
  bool PreGo112Record = false;
  /// Position of `PCDATA_StackMapIndex` in the pcdata array, or nullopt when
  /// nothing proved one.
  std::optional<unsigned> StackMapPCDataIndex = PCDataStackMapIndex;
  /// Position of `FUNCDATA_OpenCodedDeferInfo`.
  unsigned OpenCodedDeferInfoIndex = FuncDataOpenCodedDeferInfo;
  /// Whether the release the magic names emits `PCDATA_UnsafePoint`.
  bool HasUnsafePointTable = true;
};

FuncLayout getFuncLayout(uint32_t Magic, unsigned PtrSize) {
  FuncLayout L;
  if (Magic == Go12Magic) {
    L.EntryIsOffset = false;
    L.FuncDataIsPointer = true;
    L.FuncTabEntrySize = 2 * PtrSize;
    // `nfuncdata` closes the fixed part in both shapes this magic covers: the
    // older one spells it as a word at PtrSize+28 and the newer one as the
    // last byte of the word at PtrSize+28, and the pcdata array starts after
    // that word either way.
    L.FuncIDOffset = PtrSize + 28;
    L.HeaderSize = PtrSize + 32;
    L.OpenCodedDeferInfoIndex = FuncDataOpenCodedDeferInfoPreGo116;
    L.HasUnsafePointTable = false;
    L.StackMapPCDataIndex = std::nullopt;
    return L;
  }
  if (Magic == Go116Magic) {
    // `entry uintptr` leads the record, and the functab is pairs of pointers.
    L.EntryIsOffset = false;
    L.FuncDataIsPointer = true;
    L.FuncTabEntrySize = 2 * PtrSize;
    L.FuncIDOffset = PtrSize + 32;
    L.HeaderSize = PtrSize + 36;
    return L;
  }
  // Go 1.20 inserted `startLine` ahead of the trailing byte fields.
  L.FuncIDOffset = Magic == Go118Magic ? 36 : 40;
  L.HeaderSize = L.FuncIDOffset + 4;
  return L;
}

/// One `runtime._func` as read from the table, before any interpretation.
struct RawFunc {
  va_t RecordVA = 0;
  uint64_t EntryOffset = 0;
  int32_t NameOffset = 0;
  uint32_t DeferReturn = 0;
  uint32_t PcSP = 0;
  uint32_t PCDataCount = 0;
  uint8_t FuncDataCount = 0;
  uint8_t FuncID = 0;
  uint8_t Flag = 0;
};

std::optional<RawFunc> decodeFunc(const ImageReader &R, const FuncLayout &L,
                                  va_t RecordVA) {
  RawFunc F;
  F.RecordVA = RecordVA;
  if (L.EntryIsOffset) {
    std::optional<uint32_t> EntryOff = R.u32(RecordVA);
    if (!EntryOff)
      return std::nullopt;
    F.EntryOffset = *EntryOff;
  } else {
    std::optional<uint64_t> Entry = R.word(RecordVA);
    if (!Entry)
      return std::nullopt;
    F.EntryOffset = *Entry;
  }
  const va_t Rest = RecordVA + (L.EntryIsOffset ? 4 : R.pointerSize());
  std::optional<int32_t> NameOff = R.i32(Rest);
  std::optional<uint32_t> PcSP = R.u32(Rest + 12);
  std::optional<uint32_t> PCDataCount = R.u32(Rest + 24);
  if (!NameOff || !PcSP || !PCDataCount)
    return std::nullopt;
  if (*NameOff < 0 || *PCDataCount > MaxPCDataTables)
    return std::nullopt;
  F.NameOffset = *NameOff;
  F.PcSP = *PcSP;
  F.PCDataCount = *PCDataCount;

  if (L.PreGo112Record) {
    std::optional<uint32_t> FuncDataCount = R.u32(RecordVA + L.FuncIDOffset);
    if (!FuncDataCount || *FuncDataCount > MaxFuncDataTables)
      return std::nullopt;
    F.FuncDataCount = static_cast<uint8_t>(*FuncDataCount);
    return F;
  }

  std::optional<uint32_t> DeferReturn = R.u32(Rest + 8);
  std::optional<uint8_t> FuncID = R.u8(RecordVA + L.FuncIDOffset);
  std::optional<uint8_t> Flag = R.u8(RecordVA + L.FuncIDOffset + 1);
  std::optional<uint8_t> FuncDataCount = R.u8(RecordVA + L.FuncIDOffset + 3);
  if (!DeferReturn || !FuncID || !Flag || !FuncDataCount)
    return std::nullopt;
  if (*FuncDataCount > MaxFuncDataTables)
    return std::nullopt;
  F.DeferReturn = *DeferReturn;
  F.FuncID = *FuncID;
  F.Flag = *Flag;
  F.FuncDataCount = *FuncDataCount;
  return F;
}

/// Offset into `pctab` of the \p Index-th pc-value table.  Zero, which the
/// record uses to mean the table is absent, is reported as absent.
std::optional<uint32_t> getPCDataOffset(const ImageReader &R,
                                        const FuncLayout &L, const RawFunc &F,
                                        unsigned Index) {
  if (Index >= F.PCDataCount)
    return std::nullopt;
  std::optional<uint32_t> Offset = R.u32(F.RecordVA + L.HeaderSize + Index * 4);
  if (!Offset || *Offset == 0)
    return std::nullopt;
  return *Offset;
}

/// Address of the \p Index-th `_func` record, resolved through the functab.
std::optional<va_t> getFuncRecordAddress(const ImageReader &R,
                                         const FuncLayout &L, const PcHeader &H,
                                         uint64_t Index) {
  if (Index > (InvalidVA - H.FuncTab) / L.FuncTabEntrySize)
    return std::nullopt;
  const va_t Slot = H.FuncTab + Index * L.FuncTabEntrySize;
  std::optional<uint64_t> Offset;
  if (L.EntryIsOffset) {
    if (std::optional<uint32_t> Narrow = R.u32(Slot + 4))
      Offset = *Narrow;
  } else {
    Offset = R.wordAt(Slot, 1);
  }
  if (!Offset || *Offset > InvalidVA - H.FuncRecordBase)
    return std::nullopt;
  return H.FuncRecordBase + static_cast<va_t>(*Offset);
}

/// Decide which of the two `_func` shapes a Go 1.2 table uses.
///
/// The magic spans Go 1.2 through Go 1.15 and the record changed shape in the
/// middle of that span, so the header cannot say which.  `nfuncdata` can: the
/// older shape spells it as a whole word and the newer one as that word's high
/// byte, with `funcID` taking the low byte and the two in between fixed at
/// zero.  A function declares only a handful of funcdata entries, so a high
/// byte in range can only be the newer shape — under the older reading it
/// would be claiming sixteen million tables — and a low byte in range with a
/// zero high byte is evidence for the older one.  Neither reading is decidable
/// from a single record, because a runtime function with a special `funcID`
/// and no funcdata looks exactly like an older record, so the shape is voted
/// on: a real image has far more functions with pointer maps than it has
/// special ones.
bool usesPreGo112Record(const ImageReader &R, const FuncLayout &L,
                        const PcHeader &H) {
  unsigned PreGo112Votes = 0;
  unsigned Go112Votes = 0;
  const uint64_t Limit = std::min<uint64_t>(H.FuncCount, FuncLayoutVoteTarget);
  for (uint64_t I = 0; I < Limit; ++I) {
    std::optional<va_t> RecordVA = getFuncRecordAddress(R, L, H, I);
    if (!RecordVA)
      break;
    std::optional<uint32_t> Word = R.u32(*RecordVA + L.FuncIDOffset);
    // A zero word reads as no funcdata under either shape, so it is not a
    // record either side can claim.
    if (!Word || *Word == 0)
      continue;
    const uint32_t High = *Word >> 24;
    const uint32_t Padding = (*Word >> 8) & 0xFFFF;
    if (Padding == 0 && High != 0 && High <= MaxFuncDataTables)
      ++Go112Votes;
    else if (*Word <= MaxFuncDataTables)
      ++PreGo112Votes;
  }
  return PreGo112Votes > Go112Votes;
}

/// Address of the \p Index-th funcdata payload.  Absent both when the function
/// declares fewer tables and when it declares the table but has no data for
/// it.  \p GoFuncBase is ignored on the pointer layout, where each entry is
/// already a relocated address.
std::optional<va_t> getFuncDataAddress(const ImageReader &R,
                                       const FuncLayout &L, const RawFunc &F,
                                       unsigned Index, va_t GoFuncBase) {
  if (Index >= F.FuncDataCount)
    return std::nullopt;
  const va_t ArrayStart = F.RecordVA + L.HeaderSize + F.PCDataCount * 4;
  if (L.FuncDataIsPointer) {
    const unsigned PtrSize = R.pointerSize();
    // `runtime.funcdata` rounds the array up to a pointer boundary, because
    // the pcdata array ahead of it is 32-bit and can leave it half aligned.
    const va_t Aligned =
        PtrSize == 8 && (ArrayStart & 4) != 0 ? ArrayStart + 4 : ArrayStart;
    std::optional<uint64_t> Pointer = R.wordAt(Aligned, Index);
    if (!Pointer || *Pointer == 0)
      return std::nullopt;
    return static_cast<va_t>(*Pointer);
  }
  std::optional<uint32_t> Offset = R.u32(ArrayStart + Index * 4);
  if (!Offset || *Offset == NoFuncDataOffset)
    return std::nullopt;
  if (*Offset > InvalidVA - GoFuncBase)
    return std::nullopt;
  return GoFuncBase + static_cast<va_t>(*Offset);
}

//===----------------------------------------------------------------------===//
// pc-value tables
//===----------------------------------------------------------------------===//

/// One step of a pc-value table: the value that holds over [Begin, End).
struct PCValueRange {
  va_t Begin = 0;
  va_t End = 0;
  int32_t Value = 0;
};

/// Walk a pc-value table exactly as `runtime.step` does, handing each step to
/// \p Emit.  A table is a run of pairs -- a zigzag varint value delta and an
/// unsigned varint pc delta in `MinLC` units -- terminated by a zero value
/// delta, which is why the first pair is the one case where a zero delta is
/// data rather than the end.
///
/// \p Emit returns false to stop the walk early.
///
/// Returns true when the walk reached the terminator or \p Emit stopped it,
/// and false when it could not get that far: bytes the image does not map, a
/// varint with no terminating byte, a running value that leaves the range an
/// `int32` holds, or more steps than the bound allows.  A caller that gets
/// false holds a prefix of something that is not a table and has to discard
/// it, because the steps it did decode are as likely to be misread bytes as
/// they are to be a truncated answer.
template <typename EmitT>
bool forEachPCValue(const ImageReader &R, va_t PcTab, uint32_t Offset,
                    va_t EntryVA, uint8_t MinLC, EmitT Emit) {
  // A record spells "no table" as offset zero rather than as an absent field,
  // because offset zero in `pctab` is a byte the linker reserves for exactly
  // this purpose and no table starts there.
  if (Offset == 0 || MinLC == 0 || Offset > InvalidVA - PcTab)
    return false;
  va_t Cursor = PcTab + Offset;
  va_t Pc = EntryVA;
  int32_t Value = -1;
  for (unsigned Steps = 0; Steps < MaxPCValueSteps; ++Steps) {
    // The runtime tests the entry address rather than the step count, so a
    // leading pair that advances the pc by zero keeps the exemption.
    const bool First = Pc == EntryVA;
    std::optional<uint8_t> Lead = R.u8(Cursor);
    if (!Lead)
      return false;
    if (*Lead == 0 && !First)
      return true;
    std::optional<uint32_t> Encoded = R.uvarint(Cursor);
    if (!Encoded)
      return false;
    // The value delta is zigzag encoded so a negative delta stays short.
    const int32_t Delta = static_cast<int32_t>(-(*Encoded & 1) ^ (*Encoded >> 1));
    const int64_t Updated = static_cast<int64_t>(Value) + Delta;
    if (Updated < std::numeric_limits<int32_t>::min() ||
        Updated > std::numeric_limits<int32_t>::max())
      return false;
    Value = static_cast<int32_t>(Updated);
    std::optional<uint32_t> PcDelta = R.uvarint(Cursor);
    if (!PcDelta)
      return false;
    const uint64_t Advance = static_cast<uint64_t>(*PcDelta) * MinLC;
    if (Advance > InvalidVA - Pc)
      return false;
    const va_t Next = static_cast<va_t>(Pc + Advance);
    if (!Emit(PCValueRange{Pc, Next, Value}))
      return true;
    Pc = Next;
  }
  return false;
}

/// Largest stack-pointer delta the function reaches, decoded from its `pcsp`
/// table.  This is the frame size the open-coded defer offsets are measured
/// against, so without it those offsets cannot be turned into stack slots.
std::optional<int32_t> decodeMaxFrameSize(const ImageReader &R, va_t PcTab,
                                          uint32_t PcSPOffset, uint8_t MinLC) {
  int32_t Largest = 0;
  bool Implausible = false;
  // The addresses this walk produces are discarded, so the entry it measures
  // them from does not matter and is not asked for.
  const bool Walked =
      forEachPCValue(R, PcTab, PcSPOffset, 0, MinLC, [&](const PCValueRange &V) {
        if (V.Value < 0 || static_cast<uint64_t>(V.Value) > MaxFrameSize) {
          Implausible = true;
          return false;
        }
        Largest = std::max(Largest, V.Value);
        return true;
      });
  if (!Walked || Implausible)
    return std::nullopt;
  return Largest;
}

GoUnsafePointKind classifyUnsafePoint(int32_t Value) {
  switch (Value) {
  case UnsafePointSafe:
    return GoUnsafePointKind::Safe;
  case UnsafePointUnsafe:
    return GoUnsafePointKind::Unsafe;
  case UnsafePointRestart1:
  case UnsafePointRestart2:
    return GoUnsafePointKind::RestartSequence;
  case UnsafePointRestartAtEntry:
    return GoUnsafePointKind::RestartAtEntry;
  default:
    return GoUnsafePointKind::Unknown;
  }
}

//===----------------------------------------------------------------------===//
// Stack maps
//===----------------------------------------------------------------------===//

/// Decode the `runtime.stackmap` a pointer-map funcdata entry addresses:
/// `n int32`, `nbit int32`, then `n` bitmaps of `(nbit+7)/8` bytes each.
///
/// Every field is checked against a bound before it is used to size anything,
/// because a funcdata pointer that was resolved through the wrong base still
/// addresses mapped memory and would otherwise turn four arbitrary bytes into
/// an allocation request.
std::optional<GoStackMap> decodeStackMap(const ImageReader &R, va_t RecordVA) {
  std::optional<int32_t> Count = R.i32(RecordVA);
  std::optional<int32_t> BitCount = R.i32(RecordVA + 4);
  if (!Count || !BitCount || *Count < 0 || *BitCount < 0)
    return std::nullopt;
  if (static_cast<uint32_t>(*Count) > GoStackMap::MaxBitmaps ||
      static_cast<uint32_t>(*BitCount) > GoStackMap::MaxBits)
    return std::nullopt;

  GoStackMap Map;
  Map.RecordVA = RecordVA;
  Map.BitCount = static_cast<uint32_t>(*BitCount);
  const uint32_t Stride = (Map.BitCount + 7) / 8;
  if (static_cast<uint64_t>(*Count) * Stride > GoStackMap::MaxTotalBytes)
    return std::nullopt;

  // A zero-bit map carries no bytes, but it still declares `n` bitmaps: the
  // linker hands every function whose argument area holds no pointer the same
  // shared `n=1, nbit=0` record.  Reporting no bitmaps for it would make the
  // index a stack map names unsatisfiable and throw away a table that is in
  // fact well formed, so the bitmaps are published empty rather than dropped.
  Map.Bitmaps.reserve(static_cast<size_t>(*Count));
  for (uint32_t I = 0; I < static_cast<uint32_t>(*Count); ++I) {
    GoStackMapBitmap Bitmap;
    Bitmap.Index = I;
    Bitmap.BitCount = Map.BitCount;
    if (Stride != 0) {
      const uint64_t Offset = uint64_t(8) + uint64_t(I) * Stride;
      if (Offset > InvalidVA - RecordVA)
        return std::nullopt;
      const uint8_t *Data =
          R.bytes(RecordVA + static_cast<va_t>(Offset), Stride);
      if (!Data)
        return std::nullopt;
      Bitmap.Bits.assign(Data, Data + Stride);
    }
    Map.Bitmaps.push_back(std::move(Bitmap));
  }
  return Map;
}

//===----------------------------------------------------------------------===//
// moduledata
//===----------------------------------------------------------------------===//

/// Mapping for an image whose text is split across several sections.  The
/// linker then makes `entryoff` an offset into the concatenation of the
/// sections as it laid them out, not into any one of them.
struct TextSection {
  uint64_t VirtualOffset = 0;
  uint64_t End = 0;
  va_t BaseVA = 0;
};

/// The parts of `runtime.moduledata` this decoder needs.  The struct has
/// gained fields in most releases, so nothing here is read at a fixed offset
/// unless that offset has been stable since Go 1.16.
struct ModuleData {
  va_t VA = 0;
  va_t TextBase = 0;
  va_t ETextVA = 0;
  /// Base for funcdata offsets, i.e. the `go:func.*` symbol.  Zero when it
  /// could not be confirmed.
  va_t GoFuncBase = 0;
  std::vector<TextSection> TextSections;
};

/// The two frame offsets `FUNCDATA_OpenCodedDeferInfo` holds, both counted
/// downward from varp.
struct OpenCodedDeferOffsets {
  uint32_t DeferBits = 0;
  uint32_t Slots = 0;
};

std::optional<OpenCodedDeferOffsets>
readOpenCodedDeferInfo(const ImageReader &R, va_t RecordVA) {
  va_t Cursor = RecordVA;
  std::optional<uint32_t> DeferBits = R.uvarint(Cursor);
  if (!DeferBits)
    return std::nullopt;
  std::optional<uint32_t> Slots = R.uvarint(Cursor);
  if (!Slots)
    return std::nullopt;
  return OpenCodedDeferOffsets{*DeferBits, *Slots};
}

/// Whether a decoded open-coded defer record describes a frame that could
/// exist.  Both offsets name a slot inside the frame, the closure slots are
/// pointers and so are pointer aligned, and the defer bitmask is a single byte
/// that the frame layout places below the pointer slots.  A record that
/// violates any of these was read at the wrong base.
bool isPlausibleOpenCodedDeferInfo(const OpenCodedDeferOffsets &Offsets,
                                   unsigned PtrSize,
                                   std::optional<int32_t> FrameSize) {
  if (Offsets.Slots == 0 || Offsets.DeferBits == 0)
    return false;
  if (Offsets.Slots % PtrSize != 0)
    return false;
  if (Offsets.DeferBits <= Offsets.Slots)
    return false;
  const uint64_t Bound =
      FrameSize && *FrameSize > 0
          ? std::min<uint64_t>(static_cast<uint64_t>(*FrameSize),
                               MaxFrameSlotOffset)
          : MaxFrameSlotOffset;
  return Offsets.DeferBits < Bound && Offsets.Slots < Bound;
}

/// Confirm a candidate funcdata base by resolving real funcdata through it.
/// Open-coded defer records are what make this decisive: each one has to
/// describe a frame this image actually builds, and a base that is off by even
/// a byte turns the pointer-aligned slot offset into an unaligned one.
bool validatesAsFuncDataBase(const ImageReader &R, const FuncLayout &L,
                             const PcHeader &H,
                             const std::vector<RawFunc> &Funcs, va_t Base) {
  if (!R.isMapped(Base))
    return false;
  unsigned Confirmed = 0;
  unsigned Fallback = 0;
  for (const RawFunc &F : Funcs) {
    if (std::optional<va_t> RecordVA =
            getFuncDataAddress(R, L, F, L.OpenCodedDeferInfoIndex, Base)) {
      std::optional<OpenCodedDeferOffsets> Offsets =
          readOpenCodedDeferInfo(R, *RecordVA);
      if (!Offsets)
        return false;
      if (!isPlausibleOpenCodedDeferInfo(
              *Offsets, R.pointerSize(),
              decodeMaxFrameSize(R, H.PcTab, F.PcSP, H.MinLC)))
        return false;
      ++Confirmed;
    } else if (Confirmed == 0 && Fallback < FuncDataBaseSampleTarget) {
      // Only reached for an image whose functions all defer nothing.  An
      // argument pointer map is a far weaker witness — it merely has to start
      // with a believable bitmap count — but it is better than accepting a
      // base no record was resolved through at all.
      if (std::optional<va_t> MapVA =
              getFuncDataAddress(R, L, F, FuncDataArgsPointerMaps, Base)) {
        std::optional<int32_t> BitmapCount = R.i32(*MapVA);
        if (!BitmapCount || *BitmapCount < 0 || *BitmapCount > (1 << 20))
          return false;
        ++Fallback;
      }
    }
    if (Confirmed >= FuncDataBaseSampleTarget)
      break;
  }
  return Confirmed > 0 || Fallback >= FuncDataBaseSampleTarget;
}

/// Locate `runtime.firstmoduledata` by the one field whose value is already
/// known: it opens with a pointer to the `pcHeader`, immediately followed by
/// the `funcnametab` slice whose base the header also names.  Two independent
/// matches at fixed relative positions is enough to identify the structure
/// without depending on a symbol that a stripped image does not have.
std::optional<va_t> findModuleData(const ImageReader &R, const BinaryImage &Img,
                                   const PcHeader &H) {
  const unsigned PtrSize = R.pointerSize();
  for (const Segment &Seg : Img.Segments) {
    if (Seg.Data.empty())
      continue;
    const uint64_t Limit = std::min<uint64_t>(Seg.Size, Seg.Data.size());
    if (Limit < 2 * PtrSize)
      continue;
    for (uint64_t Off = 0; Off + 2 * PtrSize <= Limit; Off += PtrSize) {
      if (readPtr(Seg.Data.data() + Off, PtrSize == 8) != H.VA)
        continue;
      const va_t Candidate = Seg.VA + Off;
      std::optional<uint64_t> FuncNameTab = R.wordAt(Candidate, 1);
      if (FuncNameTab && *FuncNameTab == H.FuncNameTab)
        return Candidate;
    }
  }
  return std::nullopt;
}

/// Word index of `moduledata.text`.  Everything ahead of it — the header
/// pointer, six slices, the find-func table, and the pc bounds — has been at
/// these positions since Go 1.16.
constexpr unsigned ModuleDataTextWordIndex = 22;
constexpr unsigned ModuleDataMinPCWordIndex = 20;

/// Read the three words at \p Index as a `[]textsect` and confirm every entry
/// describes a real text section.  Used as the structural landmark that fixes
/// where the surrounding fields are, so it is deliberately strict.
bool readTextSectionMap(const ImageReader &R, va_t ModuleVA, unsigned Index,
                        std::vector<TextSection> &Out) {
  std::optional<uint64_t> DataVA = R.wordAt(ModuleVA, Index);
  std::optional<uint64_t> Length = R.wordAt(ModuleVA, Index + 1);
  std::optional<uint64_t> Capacity = R.wordAt(ModuleVA, Index + 2);
  if (!DataVA || !Length || !Capacity)
    return false;
  if (*Length == 0 || *Length != *Capacity || *Length > MaxTextSections)
    return false;
  const uint64_t Stride = uint64_t(3) * R.pointerSize();
  if (*Length > (InvalidVA - *DataVA) / Stride)
    return false;
  std::vector<TextSection> Sections;
  for (uint64_t I = 0; I < *Length; ++I) {
    const va_t Entry = static_cast<va_t>(*DataVA + I * Stride);
    std::optional<uint64_t> VirtualOffset = R.wordAt(Entry, 0);
    std::optional<uint64_t> End = R.wordAt(Entry, 1);
    std::optional<uint64_t> BaseVA = R.wordAt(Entry, 2);
    if (!VirtualOffset || !End || !BaseVA)
      return false;
    if (*VirtualOffset >= *End)
      return false;
    if (!R.isMappedCode(static_cast<va_t>(*BaseVA)))
      return false;
    Sections.push_back({*VirtualOffset, *End, static_cast<va_t>(*BaseVA)});
  }
  Out = std::move(Sections);
  return true;
}

ModuleData decodeModuleData(const ImageReader &R, const BinaryImage &Img,
                            const FuncLayout &L, const PcHeader &H, va_t VA,
                            const std::vector<RawFunc> &Funcs) {
  ModuleData MD;
  MD.VA = VA;

  // `text` is confirmed against `minpc`, which the runtime itself requires to
  // equal the entry of the first function in the table.
  std::optional<uint64_t> MinPC = R.wordAt(VA, ModuleDataMinPCWordIndex);
  std::optional<uint64_t> Text = R.wordAt(VA, ModuleDataTextWordIndex);
  std::optional<uint64_t> EText = R.wordAt(VA, ModuleDataTextWordIndex + 1);
  if (Text && MinPC && !Funcs.empty() && L.EntryIsOffset &&
      *Text + Funcs.front().EntryOffset == *MinPC) {
    MD.TextBase = static_cast<va_t>(*Text);
    if (EText)
      MD.ETextVA = static_cast<va_t>(*EText);
  } else if (Text && R.isMappedCode(static_cast<va_t>(*Text))) {
    MD.TextBase = static_cast<va_t>(*Text);
    if (EText)
      MD.ETextVA = static_cast<va_t>(*EText);
  }

  // An image that kept its symbol table names the base outright.
  for (const Symbol &Sym : Img.Symbols) {
    if (Sym.Name != "go:func.*" && Sym.Name != "go.func.*")
      continue;
    if (validatesAsFuncDataBase(R, L, H, Funcs, Sym.Addr)) {
      MD.GoFuncBase = Sym.Addr;
      break;
    }
  }

  // The funcdata base is one of a run of segment bounds whose length has grown
  // in most releases, so its word index is not fixed.  What is fixed is what
  // comes immediately after it: the `textsectmap` slice, separated from it by
  // at most one word.  A slice is far more recognizable than a bare address —
  // its length and capacity are equal, and each element it points at is a text
  // section whose base is executable — so the search anchors on the slice and
  // then confirms the one or two candidates in front of it against real
  // funcdata.
  for (unsigned Index = ModuleDataTextWordIndex + 2;
       Index < ModuleDataTextWordIndex + MaxModuleDataSearchWords; ++Index) {
    std::vector<TextSection> Sections;
    if (!readTextSectionMap(R, VA, Index, Sections))
      continue;
    if (MD.GoFuncBase != 0) {
      MD.TextSections = std::move(Sections);
      return MD;
    }
    for (unsigned Back = 1; Back <= 2 && Index >= Back; ++Back) {
      std::optional<uint64_t> Candidate = R.wordAt(VA, Index - Back);
      if (!Candidate)
        continue;
      if (!validatesAsFuncDataBase(R, L, H, Funcs,
                                   static_cast<va_t>(*Candidate)))
        continue;
      MD.GoFuncBase = static_cast<va_t>(*Candidate);
      MD.TextSections = std::move(Sections);
      return MD;
    }
  }
  return MD;
}

/// `runtime.moduledata.textAddr`.  Returns nothing when the offset does not
/// land in the address space, rather than the wrapped address that would look
/// like a plausible entry.
std::optional<va_t> resolveTextAddress(const ModuleData &MD, va_t TextBase,
                                       uint64_t Offset) {
  if (MD.TextSections.size() > 1) {
    for (size_t I = 0; I < MD.TextSections.size(); ++I) {
      const TextSection &S = MD.TextSections[I];
      const bool Last = I + 1 == MD.TextSections.size();
      if ((Offset >= S.VirtualOffset && Offset < S.End) ||
          (Last && Offset == S.End)) {
        const uint64_t Delta = Offset - S.VirtualOffset;
        if (Delta > InvalidVA - S.BaseVA)
          return std::nullopt;
        return static_cast<va_t>(S.BaseVA + Delta);
      }
    }
  }
  if (Offset > InvalidVA - TextBase)
    return std::nullopt;
  return static_cast<va_t>(TextBase + Offset);
}

//===----------------------------------------------------------------------===//
// Runtime entry points
//===----------------------------------------------------------------------===//

/// What a call to a runtime entry point means for control flow.
enum class RuntimeCallKind : uint8_t {
  DeferProc,
  DeferProcStack,
  DeferReturn,
  Recover,
  ExplicitPanic,
  ImplicitPanic,
};

std::optional<RuntimeCallKind> classifyRuntimeName(llvm::StringRef Name) {
  if (Name == "runtime.deferproc")
    return RuntimeCallKind::DeferProc;
  if (Name == "runtime.deferprocStack")
    return RuntimeCallKind::DeferProcStack;
  if (Name == "runtime.deferreturn")
    return RuntimeCallKind::DeferReturn;
  if (Name == "runtime.gorecover")
    return RuntimeCallKind::Recover;
  if (Name == "runtime.gopanic")
    return RuntimeCallKind::ExplicitPanic;
  // `sigpanic` is reached from a fault, never from a call, but a tail jump to
  // it does appear in the runtime's own assembly.
  if (Name == "runtime.sigpanic")
    return RuntimeCallKind::ImplicitPanic;
  // `panicCheck1`/`panicCheck2` are internal to the panic helpers below, so
  // counting them would attribute a panic edge to the runtime rather than to
  // the code whose check failed.
  if (Name.starts_with("runtime.panicCheck"))
    return std::nullopt;
  if (Name.starts_with("runtime.goPanic") || Name.starts_with("runtime.panic"))
    return RuntimeCallKind::ImplicitPanic;
  return std::nullopt;
}

//===----------------------------------------------------------------------===//
// Decoded function set
//===----------------------------------------------------------------------===//

struct GoFunction {
  RawFunc Raw;
  std::string Name;
  ExceptionAddressRange CodeRange;
};

/// Which pcdata table holds `PCDATA_StackMapIndex` on the Go 1.2 layout.
///
/// Go 1.13 moved the table from index 0 to index 1, putting a register-map
/// index where it had been, and left the magic alone.  What settles it is the
/// pointer map the index selects into: an index has to name one of the map's
/// `n` bitmaps, so a candidate table that ever yields a value outside
/// `[-1, n)` is not the stack map index, and one counterexample rules it out
/// for the whole module.  Only functions whose map holds more than one bitmap
/// can rule anything out — where there is one bitmap every in-range index is
/// zero and the two candidates are indistinguishable — so those are the only
/// ones sampled.  Those same functions are why a candidate that is absent
/// everywhere is not evidence against the other one: a frame with two bitmaps
/// has to carry the table that picks between them or the runtime could not
/// pick, so the position that never appears is the position the table is not
/// at.  When both candidates survive, or neither does, nothing was proven and
/// the caller reports no ranges rather than picking one.
std::optional<unsigned>
resolveStackMapPCDataIndex(const ImageReader &R, const FuncLayout &L,
                           const PcHeader &H,
                           const std::vector<GoFunction> &Funcs,
                           va_t GoFuncBase) {
  constexpr size_t NumCandidates = 2;
  constexpr unsigned Candidates[NumCandidates] = {PCDataStackMapIndexPreGo113,
                                                  PCDataStackMapIndex};
  bool Disqualified[NumCandidates] = {};
  unsigned Confirmed[NumCandidates] = {};
  unsigned Sampled = 0;
  for (const GoFunction &G : Funcs) {
    if (Sampled >= StackMapProbeTarget)
      break;
    std::optional<va_t> MapVA =
        getFuncDataAddress(R, L, G.Raw, FuncDataLocalsPointerMaps, GoFuncBase);
    if (!MapVA)
      continue;
    std::optional<int32_t> BitmapCount = R.i32(*MapVA);
    if (!BitmapCount || *BitmapCount <= 1 ||
        static_cast<uint32_t>(*BitmapCount) > GoStackMap::MaxBitmaps)
      continue;
    ++Sampled;
    for (size_t C = 0; C < NumCandidates; ++C) {
      std::optional<uint32_t> Offset =
          getPCDataOffset(R, L, G.Raw, Candidates[C]);
      if (!Offset)
        continue;
      bool InRange = true;
      const bool Walked = forEachPCValue(
          R, H.PcTab, *Offset, G.CodeRange.Begin, H.MinLC,
          [&](const PCValueRange &V) {
            if (V.Value < -1 || V.Value >= *BitmapCount) {
              InRange = false;
              return false;
            }
            return true;
          });
      if (Walked && InRange)
        ++Confirmed[C];
      else
        Disqualified[C] = true;
    }
  }
  std::optional<unsigned> Winner;
  for (size_t C = 0; C < NumCandidates; ++C) {
    if (Disqualified[C] || Confirmed[C] == 0)
      continue;
    if (Winner)
      return std::nullopt;
    Winner = Candidates[C];
  }
  return Winner;
}

/// Decode `PCDATA_UnsafePoint` into a partition of the body.
void decodeUnsafePoints(const ImageReader &R, const FuncLayout &L,
                        const PcHeader &H, const GoFunction &G,
                        GoFunctionEH &EH, ExceptionParseStatus &Status,
                        std::vector<std::string> &Diagnostics) {
  if (!L.HasUnsafePointTable)
    return;
  std::optional<uint32_t> Offset =
      getPCDataOffset(R, L, G.Raw, PCDataUnsafePoint);
  if (!Offset)
    return;
  std::vector<GoUnsafePointRange> Ranges;
  bool Truncated = false;
  const bool Walked = forEachPCValue(
      R, H.PcTab, *Offset, G.CodeRange.Begin, H.MinLC,
      [&](const PCValueRange &V) {
        if (Ranges.size() >= MaxPCValueRanges) {
          Truncated = true;
          return false;
        }
        GoUnsafePointRange Range;
        Range.Range = ExceptionAddressRange{V.Begin, V.End};
        Range.Kind = classifyUnsafePoint(V.Value);
        Range.NativeValue = V.Value;
        Ranges.push_back(std::move(Range));
        return true;
      });
  if (!Walked) {
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
    Diagnostics.push_back("Go unsafe-point table at pctab offset " +
                          std::to_string(*Offset) +
                          " is not a readable pc-value table");
    return;
  }
  if (Truncated) {
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
    Diagnostics.push_back("Go unsafe-point table has more than " +
                          std::to_string(MaxPCValueRanges) +
                          " ranges, so the tail was not decoded");
  }
  EH.UnsafePointRanges = std::move(Ranges);
}

/// Decode both pointer maps and the `PCDATA_StackMapIndex` table that selects
/// between their bitmaps.
void decodeStackMaps(const ImageReader &R, const FuncLayout &L,
                     const PcHeader &H, const GoFunction &G, va_t GoFuncBase,
                     GoFunctionEH &EH, ExceptionParseStatus &Status,
                     std::vector<std::string> &Diagnostics) {
  auto readMap = [&](unsigned Index, const char *What,
                     std::optional<GoStackMap> &Out) {
    std::optional<va_t> MapVA =
        getFuncDataAddress(R, L, G.Raw, Index, GoFuncBase);
    if (!MapVA)
      return;
    std::optional<GoStackMap> Map = decodeStackMap(R, *MapVA);
    if (!Map) {
      Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
      Diagnostics.push_back(std::string("Go ") + What + " pointer map at " +
                            llvm::utohexstr(*MapVA) +
                            " is not a readable stackmap");
      return;
    }
    Out = std::move(Map);
  };
  readMap(FuncDataArgsPointerMaps, "argument", EH.ArgsPointerMap);
  readMap(FuncDataLocalsPointerMaps, "locals", EH.LocalsPointerMap);
  if (!EH.ArgsPointerMap && !EH.LocalsPointerMap)
    return;

  if (!L.StackMapPCDataIndex)
    return;
  std::optional<uint32_t> Offset =
      getPCDataOffset(R, L, G.Raw, *L.StackMapPCDataIndex);
  if (!Offset)
    return;
  std::vector<GoStackMapRange> Ranges;
  bool Truncated = false;
  const bool Walked = forEachPCValue(
      R, H.PcTab, *Offset, G.CodeRange.Begin, H.MinLC,
      [&](const PCValueRange &V) {
        if (Ranges.size() >= MaxPCValueRanges) {
          Truncated = true;
          return false;
        }
        Ranges.push_back(
            GoStackMapRange{ExceptionAddressRange{V.Begin, V.End}, V.Value});
        return true;
      });
  if (!Walked) {
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
    Diagnostics.push_back("Go stack map index table at pctab offset " +
                          std::to_string(*Offset) +
                          " is not a readable pc-value table");
    return;
  }
  if (Truncated) {
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
    Diagnostics.push_back("Go stack map index table has more than " +
                          std::to_string(MaxPCValueRanges) +
                          " ranges, so the tail was not decoded");
  }
  // An index neither map can satisfy means the table being read is not the
  // one the maps belong to, so the ranges describe nothing and are dropped
  // rather than published as a selection into bitmaps that do not exist.  The
  // bar is the larger of the two counts rather than the smaller because the
  // two are allowed to differ: a function whose arguments hold no pointers
  // gets the linker's shared single-bitmap map for them while keeping a full
  // one for its locals.
  const int32_t Available = static_cast<int32_t>(std::max(
      EH.ArgsPointerMap ? EH.ArgsPointerMap->Bitmaps.size() : 0,
      EH.LocalsPointerMap ? EH.LocalsPointerMap->Bitmaps.size() : 0));
  for (const GoStackMapRange &Range : Ranges) {
    if (Range.Index < Available)
      continue;
    Status = mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
    Diagnostics.push_back("Go stack map index " +
                          std::to_string(Range.Index) + " at " +
                          llvm::utohexstr(Range.Range.Begin) +
                          " names no bitmap this function declares");
    return;
  }
  EH.StackMapRanges = std::move(Ranges);
}

} // namespace

bool hasGoRuntimeMetadata(const BinaryImage &Img) {
  const ImageReader R(Img);
  return findPcHeader(R, Img).has_value();
}

void parseGoExceptions(BinaryImage &Img) {
  if (Img.Arch != Arch::X64 && Img.Arch != Arch::X86 &&
      Img.Arch != Arch::AArch64 && Img.Arch != Arch::ARM)
    return;

  const ImageReader R(Img);
  std::optional<PcHeader> Header = findPcHeader(R, Img);
  if (!Header)
    return;

  ExceptionInfo &Info = Img.ExceptionMetadata;
  // Anything the module walk could not prove degrades the whole table, not
  // just the record it was noticed on: a funcdata base that stayed unconfirmed
  // silently costs every function its open-coded defer state, and a functab
  // that ended early costs whichever functions were past the break.  Carrying
  // that on the image status is what stops a caller from treating the result
  // as complete enough to regenerate metadata from.
  ExceptionParseStatus ModuleStatus = ExceptionParseStatus::Complete;
  auto note = [&](const std::string &Message,
                  ExceptionParseStatus Status = ExceptionParseStatus::Partial) {
    Info.Diagnostics.push_back(Message);
    ModuleStatus = mergeExceptionParseStatus(ModuleStatus, Status);
  };

  FuncLayout Layout = getFuncLayout(Header->Magic, R.pointerSize());
  if (Header->Magic == Go12Magic)
    Layout.PreGo112Record = usesPreGo112Record(R, Layout, *Header);

  // Pass one: the raw records, which the moduledata search needs in order to
  // confirm a funcdata base.
  std::vector<RawFunc> RawFuncs;
  RawFuncs.reserve(static_cast<size_t>(Header->FuncCount));
  bool TruncatedTable = false;
  for (uint64_t I = 0; I < Header->FuncCount; ++I) {
    std::optional<va_t> RecordVA = getFuncRecordAddress(R, Layout, *Header, I);
    if (!RecordVA) {
      TruncatedTable = true;
      break;
    }
    std::optional<RawFunc> F = decodeFunc(R, Layout, *RecordVA);
    if (!F) {
      TruncatedTable = true;
      break;
    }
    RawFuncs.push_back(*F);
  }
  if (RawFuncs.empty())
    return;

  // The functab's final entry is a sentinel naming the address past the last
  // function, which is what gives the last real function its end.
  std::optional<uint64_t> SentinelOffset;
  if (Layout.EntryIsOffset) {
    if (std::optional<uint32_t> Off = R.u32(
            Header->FuncTab + Header->FuncCount * Layout.FuncTabEntrySize))
      SentinelOffset = *Off;
  } else if (std::optional<uint64_t> Off = R.wordAt(
                 Header->FuncTab + Header->FuncCount * Layout.FuncTabEntrySize,
                 0)) {
    SentinelOffset = *Off;
  }

  // The Go 1.2 layout measures nothing from `moduledata`: entries are absolute
  // addresses, funcdata entries are relocated pointers, and every other offset
  // is relative to the header.  Searching for a structure whose field
  // positions moved in most of the releases this magic spans would risk
  // reading a base that is not one, in exchange for nothing.
  ModuleData MD;
  if (Header->Magic != Go12Magic)
    if (std::optional<va_t> ModuleVA = findModuleData(R, Img, *Header))
      MD = decodeModuleData(R, Img, Layout, *Header, *ModuleVA, RawFuncs);

  va_t TextBase = MD.TextBase;
  if (TextBase == 0)
    TextBase = Header->TextStart;
  if (TextBase == 0 && Layout.EntryIsOffset) {
    // Last resort.  The text section's start is where the linker normally puts
    // the first function, but nothing in the image proves it is the base the
    // offsets were measured from, so every address derived from it is a guess.
    if (const Section *Text = Img.getTextSection()) {
      TextBase = Text->VA;
      note("Go text base was assumed to be the text section start because "
           "neither pcHeader nor moduledata proved one");
    }
  }
  if (Layout.EntryIsOffset && TextBase == 0) {
    note("Go pclntab found but no text base could be proven, so no function "
         "address is recoverable");
    Info.ParseStatus =
        mergeExceptionParseStatus(Info.ParseStatus, ModuleStatus);
    return;
  }

  GoModuleInfo Module;
  Module.PclnTabVersion = getMagicVersionName(Header->Magic);
  Module.PclnTabMagic = Header->Magic;
  Module.PcHeaderVA = Header->VA;
  Module.ModuleDataVA = MD.VA;
  Module.TextBase = TextBase;
  Module.GoFuncBase = MD.GoFuncBase;
  Module.FuncNameTabVA = Header->FuncNameTab;
  Module.PcTabVA = Header->PcTab;
  Module.FuncTabVA = Header->FuncTab;
  Module.FunctionCount = RawFuncs.size();
  Module.MinLC = Header->MinLC;
  Module.PtrSize = Header->PtrSize;
  Module.HasMultipleTextSections = MD.TextSections.size() > 1;

  if (TruncatedTable)
    note("Go pclntab function table ended early after " +
         std::to_string(RawFuncs.size()) + " of " +
         std::to_string(Header->FuncCount) + " records");
  if (!SentinelOffset)
    note("Go pclntab has no functab sentinel, so the last function's end "
         "address is unknown");
  // Only the offset layout needs the base; on the pointer layouts each
  // funcdata entry is already a relocated address, so a module structure that
  // was never looked for costs nothing.
  if (!Layout.FuncDataIsPointer) {
    if (MD.VA == 0)
      note("Go moduledata not found, so funcdata-derived state including "
           "open-coded defer info is unavailable");
    else if (MD.GoFuncBase == 0)
      note("Go funcdata base could not be confirmed in moduledata at " +
           llvm::utohexstr(MD.VA));
  }
  if (!Layout.HasUnsafePointTable)
    note("Go pclntab predates Go 1.16, which is when PCDATA_UnsafePoint was "
         "introduced, so no async-preemption safety was recovered");

  // Pass two: names and code ranges.
  std::vector<GoFunction> Funcs;
  Funcs.reserve(RawFuncs.size());
  for (size_t I = 0; I < RawFuncs.size(); ++I) {
    GoFunction G;
    G.Raw = RawFuncs[I];
    auto toAddress = [&](uint64_t Offset) -> va_t {
      if (!Layout.EntryIsOffset)
        return static_cast<va_t>(Offset);
      return resolveTextAddress(MD, TextBase, Offset).value_or(0);
    };
    uint64_t NextOffset = 0;
    if (I + 1 < RawFuncs.size())
      NextOffset = RawFuncs[I + 1].EntryOffset;
    else if (SentinelOffset)
      NextOffset = *SentinelOffset;
    G.CodeRange = ExceptionAddressRange{toAddress(G.Raw.EntryOffset),
                                        toAddress(NextOffset)};
    if (std::optional<std::string> Name = R.cstring(
            Header->FuncNameTab + static_cast<va_t>(G.Raw.NameOffset)))
      G.Name = std::move(*Name);
    Funcs.push_back(std::move(G));
  }

  if (!Layout.StackMapPCDataIndex) {
    Layout.StackMapPCDataIndex =
        resolveStackMapPCDataIndex(R, Layout, *Header, Funcs, MD.GoFuncBase);
    if (!Layout.StackMapPCDataIndex)
      note("Go pclntab predates Go 1.16 and nothing in it distinguishes the "
           "release that moved PCDATA_StackMapIndex from table 0 to table 1, "
           "so no stack map was tied to a PC range");
  }
  Module.StackMapPCDataIndex = Layout.StackMapPCDataIndex;
  Module.UsesPreGo112FuncLayout = Layout.PreGo112Record;

  // The pclntab is a symbol table.  It names every function the Go linker
  // kept, and it survives in a binary stripped of everything the container
  // format could carry, which is the normal shape of a shipped Go program.
  // Publishing those names is what lets the rest of the pipeline work from
  // Go's own naming instead of from nothing -- personality resolution among
  // it, since the one routine Go installs on windows/amd64 is named here and
  // nowhere else in the image.
  //
  // Function discovery has already run, so many of these addresses carry a
  // placeholder symbol it invented.  A placeholder is exactly what the pclntab
  // name should replace; a name that came from the container is not, because
  // that one was written by the linker rather than derived from an address.
  {
    llvm::DenseMap<va_t, const GoFunction *> ByEntry;
    for (const GoFunction &G : Funcs)
      if (!G.Name.empty() && G.CodeRange.isValid())
        ByEntry.try_emplace(G.CodeRange.Begin, &G);
    for (Symbol &S : Img.Symbols) {
      auto It = ByEntry.find(S.Addr);
      if (It == ByEntry.end())
        continue;
      if (llvm::StringRef(S.Name).starts_with(kAutoFuncPrefix)) {
        S.Name = It->second->Name;
        S.IsFunc = true;
        if (S.Size == 0)
          S.Size = It->second->CodeRange.size();
      }
      ByEntry.erase(It);
    }
    for (const GoFunction &G : Funcs) {
      if (G.Name.empty() || !G.CodeRange.isValid() ||
          !ByEntry.count(G.CodeRange.Begin))
        continue;
      Symbol S = Symbol::makeFunc(G.CodeRange.Begin, G.CodeRange.size());
      S.Name = G.Name;
      Img.Symbols.push_back(std::move(S));
    }
  }

  // The runtime entry points this image actually links, keyed by entry
  // address.  Only a branch that lands on one of these is treated as an edge.
  llvm::DenseMap<va_t, std::pair<RuntimeCallKind, const std::string *>>
      RuntimeTargets;
  for (const GoFunction &G : Funcs) {
    if (G.Name.empty() || !G.CodeRange.isValid())
      continue;
    if (std::optional<RuntimeCallKind> Kind = classifyRuntimeName(G.Name))
      RuntimeTargets.try_emplace(G.CodeRange.Begin,
                                 std::make_pair(*Kind, &G.Name));
  }
  if (RuntimeTargets.empty())
    note("Go image links no recognized defer/panic/recover runtime entry "
         "points, so no call-site edges were attributed");

  const unsigned Stride = getBranchScanStride(Img.Arch, Img.Mode);
  size_t RecordsAdded = 0;
  for (const GoFunction &G : Funcs) {
    GoFunctionEH EH;
    EH.EntryVA = G.CodeRange.Begin;
    EH.Name = G.Name;
    EH.FuncID = G.Raw.FuncID;
    EH.FuncFlags = G.Raw.Flag;
    if (G.Raw.DeferReturn != 0)
      EH.DeferReturnOffset = G.Raw.DeferReturn;
    EH.FrameSize =
        decodeMaxFrameSize(R, Header->PcTab, G.Raw.PcSP, Header->MinLC);

    ExceptionParseStatus Status = ExceptionParseStatus::Complete;
    std::vector<std::string> Diagnostics;

    // The offset layout needs a proven funcdata base; the pointer layout
    // carries relocated addresses and so needs none.
    if (MD.GoFuncBase != 0 || Layout.FuncDataIsPointer) {
      if (std::optional<va_t> RecordVA =
              getFuncDataAddress(R, Layout, G.Raw,
                                 Layout.OpenCodedDeferInfoIndex,
                                 MD.GoFuncBase)) {
        EH.UsesOpenCodedDefers = true;
        std::optional<OpenCodedDeferOffsets> Offsets =
            readOpenCodedDeferInfo(R, *RecordVA);
        if (Offsets && isPlausibleOpenCodedDeferInfo(*Offsets, R.pointerSize(),
                                                     EH.FrameSize)) {
          GoOpenCodedDeferInfo OpenInfo;
          OpenInfo.DeferBitsOffset = Offsets->DeferBits;
          OpenInfo.SlotsOffset = Offsets->Slots;
          EH.OpenCodedDeferInfo = OpenInfo;
          // The record does not store how many slots the frame holds.  What it
          // does fix is where the array starts, and the array runs upward from
          // there to varp, so its length is bounded by the distance between
          // them.  The bound is the exact count whenever the closure slots are
          // the topmost pointer locals, which is how the frame is normally
          // laid out, and it can only ever be too large — never too small.
          const uint32_t SlotBound = Offsets->Slots / R.pointerSize();
          if (SlotBound <= GoOpenCodedDeferInfo::MaxSlots) {
            for (uint32_t Slot = 0; Slot < SlotBound; ++Slot)
              EH.OpenCodedDefers.push_back({-static_cast<int32_t>(
                  Offsets->Slots - Slot * R.pointerSize())});
          } else {
            // The frame is too deep for the bound to be the slot count, so
            // enumerating it would invent slots the function does not have.
            // The `where` is still sound; only the `how many` is lost.
            Status = ExceptionParseStatus::Partial;
            Diagnostics.push_back(
                "open-coded defer slot array at frame offset -" +
                std::to_string(Offsets->Slots) +
                " is too far from varp for its length to be bounded");
          }
        } else {
          Status = ExceptionParseStatus::Partial;
          Diagnostics.push_back("open-coded defer info at " +
                                llvm::utohexstr(*RecordVA) +
                                " does not describe a frame this function "
                                "builds");
        }
      }
    }

    // Attribute the branch sites in this body.
    if (G.CodeRange.isValid() && !RuntimeTargets.empty()) {
      const uint64_t Size = G.CodeRange.size();
      for (uint64_t Off = 0; Off + 4 <= Size; Off += Stride) {
        const va_t SiteVA = G.CodeRange.Begin + Off;
        const size_t Available =
            static_cast<size_t>(std::min<uint64_t>(Size - Off, 16));
        const uint8_t *Code =
            Img.readVA(SiteVA, std::min<size_t>(Available, 4));
        if (!Code) {
          // The table says this body extends further than the image maps, so
          // whatever edges are past here were never looked for.
          Status =
              mergeExceptionParseStatus(Status, ExceptionParseStatus::Partial);
          Diagnostics.push_back("Go function body is unmapped from " +
                                llvm::utohexstr(SiteVA) +
                                ", so later call sites were not scanned");
          break;
        }
        size_t Length = Stride;
        std::optional<va_t> Target = decodeDirectBranchTarget(
            Img.Arch, Img.Mode, Code, Available, SiteVA, Length);
        if (!Target)
          continue;
        // Every Go function ends its stack-growth path with a jump back to its
        // own entry so the frame is retried on the bigger stack.  In a runtime
        // function that is itself a branch target of interest, that jump would
        // otherwise be read as the function calling itself.
        if (*Target == G.CodeRange.Begin)
          continue;
        auto It = RuntimeTargets.find(*Target);
        if (It == RuntimeTargets.end())
          continue;
        const RuntimeCallKind Kind = It->second.first;
        const std::string &TargetName = *It->second.second;
        switch (Kind) {
        case RuntimeCallKind::DeferProc:
        case RuntimeCallKind::DeferProcStack: {
          GoDeferSite Site;
          Site.CallVA = SiteVA;
          Site.Kind = Kind == RuntimeCallKind::DeferProc ? GoDeferKind::Heap
                                                         : GoDeferKind::Stack;
          EH.Defers.push_back(std::move(Site));
          break;
        }
        case RuntimeCallKind::DeferReturn:
          // The `deferreturn` call is the frame's re-entry point and is
          // already named by `_func.deferreturn`; recording it again as a
          // defer site would double count it.
          break;
        case RuntimeCallKind::Recover: {
          GoRecoverSite Site;
          Site.CallVA = SiteVA;
          // Only the compiler's deferred-call wrapper carries this suffix, so
          // it proves the frame is a deferred one.  A recover reached any
          // other way returns nil, and this stays false rather than claiming
          // a position it did not prove.
          Site.InDeferredFrame = llvm::StringRef(G.Name).contains(".deferwrap");
          EH.Recovers.push_back(std::move(Site));
          break;
        }
        case RuntimeCallKind::ExplicitPanic:
        case RuntimeCallKind::ImplicitPanic: {
          GoPanicSite Site;
          Site.CallVA = SiteVA;
          Site.RuntimeName = TargetName;
          Site.IsImplicitCheck = Kind == RuntimeCallKind::ImplicitPanic;
          EH.Panics.push_back(std::move(Site));
          break;
        }
        }
      }
    }

    if (!EH.hasExceptionalControlFlow())
      continue;

    // Every function has pointer maps and an unsafe-point table, so decoding
    // them for all of them would grow the result by the size of the image
    // while saying nothing about most of it.  What makes them worth carrying
    // is the frame they describe being unwound, so they are decoded exactly
    // where a frame can be: after this record is known to be kept.
    if (MD.GoFuncBase != 0 || Layout.FuncDataIsPointer)
      decodeStackMaps(R, Layout, *Header, G, MD.GoFuncBase, EH, Status,
                      Diagnostics);
    decodeUnsafePoints(R, Layout, *Header, G, EH, Status, Diagnostics);

    ExceptionFunction F;
    F.CodeRange = G.CodeRange;
    F.Encoding = ExceptionEncoding::GoFuncTable;
    F.Personality = ExceptionPersonality::GoRuntimeDispatch;
    F.PersonalityName = "runtime.gopanic";
    F.ParseStatus = Status;
    F.Diagnostics = std::move(Diagnostics);
    if (!G.CodeRange.isValid())
      F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus,
                                                ExceptionParseStatus::Partial);
    // A frame that defers must have a `deferreturn` for the runtime to resume
    // it at; the runtime treats the absence as fatal, so a record missing it
    // is a decode that went wrong rather than a program that is unusual.
    if ((EH.UsesOpenCodedDefers || !EH.Defers.empty()) &&
        !EH.DeferReturnOffset.has_value())
      F.ParseStatus = mergeExceptionParseStatus(F.ParseStatus,
                                                ExceptionParseStatus::Partial);
    F.Go = std::move(EH);
    Info.Functions.push_back(std::move(F));
    ++RecordsAdded;
  }

  Info.GoModule = std::move(Module);
  Info.ParseStatus = mergeExceptionParseStatus(Info.ParseStatus, ModuleStatus);
  if (RecordsAdded != 0) {
    Info.addModel(ExceptionModel::GoRuntime);
    Info.rebuildIndex();
  }
}

} // namespace neverd::go_loader
