//===- GoRuntimeDetail.h - Private Go pclntab decoding helpers --*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Implementation detail of the Go runtime metadata decoder.  The types,
/// bounds and helpers here are shared between the translation units under
/// `lib/loader/Go` and are not part of any public interface; nothing outside
/// that directory may include this header.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LIB_LOADER_GO_GORUNTIMEDETAIL_H
#define NEVERD_LIB_LOADER_GO_GORUNTIMEDETAIL_H

#include "neverd/loader/Go/GoRuntimeEH.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Compiler.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace neverd::go_loader {
namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail {

/// `pcHeader.magic`.  The value both identifies the table and, because the
/// four bytes are not a palindrome, proves the byte order it was written in.
inline constexpr uint32_t Go12Magic = 0xFFFFFFFBu;  // Go 1.2 - 1.15
inline constexpr uint32_t Go116Magic = 0xFFFFFFFAu; // Go 1.16 - 1.17
inline constexpr uint32_t Go118Magic = 0xFFFFFFF0u; // Go 1.18 - 1.19
inline constexpr uint32_t Go120Magic = 0xFFFFFFF1u; // Go 1.20 and later

/// `internal/abi.FUNCDATA_OpenCodedDeferInfo`.
inline constexpr unsigned FuncDataOpenCodedDeferInfo = 4;
/// Where the same table sat before Go 1.16 renumbered the array.  Reading it
/// on an older image is safe in both directions: no release that spans the Go
/// 1.2 magic and predates open-coded defers ever emitted six funcdata entries,
/// so index 5 is out of range there rather than pointing at something else.
inline constexpr unsigned FuncDataOpenCodedDeferInfoPreGo116 = 5;
/// `internal/abi.FUNCDATA_ArgsPointerMaps`, which has been index 0 since the
/// array existed.
inline constexpr unsigned FuncDataArgsPointerMaps = 0;
/// `internal/abi.FUNCDATA_LocalsPointerMaps`, likewise fixed at index 1.
inline constexpr unsigned FuncDataLocalsPointerMaps = 1;
/// The sentinel `_func.funcdata[i]` carries when the entry is absent.
inline constexpr uint32_t NoFuncDataOffset = 0xFFFFFFFFu;

/// `internal/abi.PCDATA_UnsafePoint`.  The table is Go 1.16 and later only:
/// index 0 held a register-map index before that, and the async preemption
/// this table describes did not exist to be described.
inline constexpr unsigned PCDataUnsafePoint = 0;
/// `internal/abi.PCDATA_StackMapIndex`.  Go 1.13 moved it here from index 0,
/// which the Go 1.2 magic does not distinguish, so on that layout the position
/// is proven from the pointer maps rather than assumed.
inline constexpr unsigned PCDataStackMapIndex = 1;
inline constexpr unsigned PCDataStackMapIndexPreGo113 = 0;

/// `internal/abi.UnsafePoint*`.
inline constexpr int32_t UnsafePointSafe = -1;
inline constexpr int32_t UnsafePointUnsafe = -2;
inline constexpr int32_t UnsafePointRestart1 = -3;
inline constexpr int32_t UnsafePointRestart2 = -4;
inline constexpr int32_t UnsafePointRestartAtEntry = -5;

/// Bounds.  Each is far above anything a real Go link produces and far below
/// what would let a mis-identified table run the decoder out of time.
inline constexpr uint64_t MaxFunctionCount = 1u << 22;
inline constexpr uint32_t MaxPCDataTables = 64;
inline constexpr uint8_t MaxFuncDataTables = 16;
inline constexpr size_t MaxSymbolNameLength = 4096;
inline constexpr uint64_t MaxFrameSize = 1u << 24;
inline constexpr uint64_t MaxFrameSlotOffset = 1u << 20;
inline constexpr unsigned MaxTextSections = 64;
/// How far past `moduledata.text` the funcdata base is searched for.  The
/// fields between them are a handful of segment bounds whose count has grown
/// by a few words per release; this covers every layout since Go 1.16 with
/// room to spare.
inline constexpr unsigned MaxModuleDataSearchWords = 48;
/// Candidate funcdata bases are confirmed against decoded records.  More
/// samples cost nothing and make a coincidental match implausible.
inline constexpr unsigned FuncDataBaseSampleTarget = 16;
/// Bound on the number of steps taken while walking one pc-value table.
inline constexpr unsigned MaxPCValueSteps = 1u << 16;
/// Bound on how many ranges one pc-value table contributes to a record.  A
/// table can legitimately have more steps than this on a very large function,
/// in which case the record is truncated and marked rather than grown without
/// limit.
inline constexpr unsigned MaxPCValueRanges = 1u << 12;
/// How many functions the Go 1.2 stack-map-index probe examines before it
/// settles.  Only functions whose locals map has more than one bitmap say
/// anything, so the target is a count of those rather than of all functions.
inline constexpr unsigned StackMapProbeTarget = 64;
/// How many records the Go 1.2 `_func` shape vote examines.
inline constexpr unsigned FuncLayoutVoteTarget = 256;

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
    const uint64_t Offset = static_cast<uint64_t>(Index) * PtrSize;
    if (Offset > InvalidVA - Base)
      return std::nullopt;
    return word(Base + Offset);
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
    const int32_t Delta =
        static_cast<int32_t>(-(*Encoded & 1) ^ (*Encoded >> 1));
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

/// One decoded `FUNCDATA_OpenCodedDeferInfo` record.  Every offset is counted
/// downward from varp.
struct OpenCodedDeferRecord {
  GoOpenCodedDeferLayout Layout = GoOpenCodedDeferLayout::Contiguous;
  uint32_t DeferBits = 0;
  /// Frame offset of the closure slot the record names first.
  uint32_t FirstSlot = 0;
  /// Every slot the frame holds, highest offset first.
  llvm::SmallVector<uint32_t, GoOpenCodedDeferInfo::MaxSlots> Slots;
  /// False when the run of slots starts too far from varp for its length to be
  /// bounded, which loses the count but not where the run begins.
  bool SlotsBounded = true;
};

//===----------------------------------------------------------------------===//
// Decoded function set
//===----------------------------------------------------------------------===//

struct GoFunction {
  RawFunc Raw;
  std::string Name;
  ExceptionAddressRange CodeRange;
};

//===----------------------------------------------------------------------===//
// GoPclntab.cpp
//===----------------------------------------------------------------------===//

const char *getMagicVersionName(uint32_t Magic);

std::optional<PcHeader> findPcHeader(const ImageReader &R,
                                     const BinaryImage &Img);

//===----------------------------------------------------------------------===//
// GoFuncTable.cpp
//===----------------------------------------------------------------------===//

FuncLayout getFuncLayout(uint32_t Magic, unsigned PtrSize);

std::optional<RawFunc> decodeFunc(const ImageReader &R, const FuncLayout &L,
                                  va_t RecordVA);

std::optional<uint32_t> getPCDataOffset(const ImageReader &R,
                                        const FuncLayout &L, const RawFunc &F,
                                        unsigned Index);

std::optional<va_t> getFuncRecordAddress(const ImageReader &R,
                                         const FuncLayout &L, const PcHeader &H,
                                         uint64_t Index);

bool usesPreGo112Record(const ImageReader &R, const FuncLayout &L,
                        const PcHeader &H);

std::optional<va_t> getFuncDataAddress(const ImageReader &R,
                                       const FuncLayout &L, const RawFunc &F,
                                       unsigned Index, va_t GoFuncBase);

std::optional<int32_t> decodeMaxFrameSize(const ImageReader &R, va_t PcTab,
                                          uint32_t PcSPOffset, uint8_t MinLC);

//===----------------------------------------------------------------------===//
// GoDeferInfo.cpp
//===----------------------------------------------------------------------===//

std::optional<OpenCodedDeferRecord>
readOpenCodedDeferInfo(const ImageReader &R, va_t RecordVA,
                       GoOpenCodedDeferLayout Layout, unsigned PtrSize,
                       std::optional<int32_t> FrameSize);

bool readsUnderAnyLayout(const ImageReader &R, va_t RecordVA, unsigned PtrSize,
                         std::optional<int32_t> FrameSize);

GoOpenCodedDeferLayout
resolveOpenCodedDeferLayout(const ImageReader &R, const FuncLayout &L,
                            const PcHeader &H,
                            const std::vector<RawFunc> &Funcs, va_t Base);

//===----------------------------------------------------------------------===//
// GoModuleData.cpp
//===----------------------------------------------------------------------===//

std::optional<va_t> findModuleData(const ImageReader &R, const BinaryImage &Img,
                                   const PcHeader &H);

ModuleData decodeModuleData(const ImageReader &R, const BinaryImage &Img,
                            const FuncLayout &L, const PcHeader &H, va_t VA,
                            const std::vector<RawFunc> &Funcs);

std::optional<va_t> resolveTextAddress(const ModuleData &MD, va_t TextBase,
                                       uint64_t Offset);

//===----------------------------------------------------------------------===//
// GoStackMaps.cpp
//===----------------------------------------------------------------------===//

std::optional<unsigned> resolveStackMapPCDataIndex(
    const ImageReader &R, const FuncLayout &L, const PcHeader &H,
    const std::vector<GoFunction> &Funcs, va_t GoFuncBase);

void decodeUnsafePoints(const ImageReader &R, const FuncLayout &L,
                        const PcHeader &H, const GoFunction &G,
                        GoFunctionEH &EH, ExceptionParseStatus &Status,
                        std::vector<std::string> &Diagnostics);

void decodeStackMaps(const ImageReader &R, const FuncLayout &L,
                     const PcHeader &H, const GoFunction &G, va_t GoFuncBase,
                     GoFunctionEH &EH, ExceptionParseStatus &Status,
                     std::vector<std::string> &Diagnostics);

} // namespace LLVM_LIBRARY_VISIBILITY_NAMESPACE detail
} // namespace neverd::go_loader

#endif // NEVERD_LIB_LOADER_GO_GORUNTIMEDETAIL_H
