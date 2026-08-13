//===- LanguageEHGo.h - Go runtime frame metadata -------------*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Normalized Go frame metadata recovered from `pclntab` funcdata and pcdata:
/// how a frame records its deferred calls, where its panic and recover sites
/// are, which stretches of it may be preempted, and the pointer maps that
/// describe its frame.  Plus the image-wide `moduledata` state that names the
/// bases every one of those offsets is relative to.
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_LOADER_LANGUAGEEHGO_H
#define NEVERD_LOADER_LANGUAGEEHGO_H

#include "neverd/loader/ExceptionCommon.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neverd {

/// How a deferred call is recorded.  Go has changed this three times, and the
/// shape decides where the deferred closure is found at run time.
enum class GoDeferKind : uint8_t {
  /// `runtime.deferproc`: heap-allocated `_defer` record.
  Heap,
  /// `runtime.deferprocStack`: caller-allocated `_defer` record.
  Stack,
  /// Open-coded: no record; the frame's defer bits and `FUNCDATA_
  /// OpenCodedDeferInfo` describe the closures directly.
  OpenCoded,
};

const char *getGoDeferKindName(GoDeferKind Kind);

/// One deferred-call site.
struct GoDeferSite {
  va_t CallVA = 0;
  GoDeferKind Kind = GoDeferKind::Heap;
  /// Deferred function when a constant closure could be proven at the site.
  va_t TargetVA = 0;
  std::string TargetName;
};

/// One open-coded defer slot described by `FUNCDATA_OpenCodedDeferInfo`.
struct GoOpenCodedDefer {
  /// Frame offset of the closure pointer, relative to varp.
  int32_t ClosureOffset = 0;
};

/// Which of the three spellings Go has given `FUNCDATA_OpenCodedDeferInfo` a
/// record uses.  The pclntab magic distinguishes none of them: it last changed
/// in Go 1.20 while the record changed in Go 1.18 and again in Go 1.22, so one
/// magic covers two spellings and only the bytes say which is which.
enum class GoOpenCodedDeferLayout : uint8_t {
  /// Go 1.22 and later.  The compiler sorts the closure slots into a single
  /// ascending run, so the record names only where that run begins and the
  /// runtime reaches the rest by indexing it.
  Contiguous,
  /// Go 1.18 through 1.21.  The compiler placed the slots wherever it liked,
  /// so the record names how many there are and then every one of them.
  Enumerated,
  /// Go 1.14, where open-coded defers began, through Go 1.17.  A deferred call
  /// could still take arguments then, so the record also leads with the
  /// largest argument frame any of them needs and gives each defer its
  /// argument size and argument list alongside its closure slot.  Go 1.18 made
  /// deferred functions argumentless and dropped all of it.
  LegacyEnumerated,
};

const char *getGoOpenCodedDeferLayoutName(GoOpenCodedDeferLayout Layout);

/// The whole of `FUNCDATA_OpenCodedDeferInfo`: unsigned varints giving the
/// distance *below* varp of the defer bitmask byte and of the closure slots.
struct GoOpenCodedDeferInfo {
  GoOpenCodedDeferLayout Layout = GoOpenCodedDeferLayout::Contiguous;
  uint32_t DeferBitsOffset = 0;
  /// Frame offset of the closure slot the record names first.
  uint32_t SlotsOffset = 0;
  /// True when the record named its slots outright, which makes
  /// `GoFunctionEH::OpenCodedDefers` the frame's exact set.  A `Contiguous`
  /// record deliberately does not store how many slots are live — the runtime
  /// learns that from the bitmask at unwind time — so there the slots are an
  /// upper bound and a decoder that reports a count has invented it.
  bool SlotCountIsExact = false;
  /// Upper bound on live slots, fixed by the one-byte bitmask.
  static constexpr unsigned MaxSlots = 8;
};

/// What `PCDATA_UnsafePoint` says about a stretch of instructions.
///
/// Go preempts a goroutine asynchronously by delivering a signal and rewriting
/// the interrupted frame so that it calls `asyncPreempt`, which spills every
/// register into the frame for the collector to see.  That is only sound where
/// the compiler says it is, and this table is where it says so.  Read as a
/// statement about the code rather than about the scheduler, an unsafe stretch
/// is one the compiler built assuming nothing can observe the frame part way
/// through it: the pointer maps need not describe the frame there, and a
/// deferred call or a panic taken from inside one is unwinding a frame whose
/// shape is momentarily not the declared one.
enum class GoUnsafePointKind : uint8_t {
  /// `UnsafePointSafe` (-1), which is also the value in effect before the
  /// table's first entry and after its last.
  Safe,
  /// `UnsafePointUnsafe` (-2).
  Unsafe,
  /// `UnsafePointRestart1` (-3) and `UnsafePointRestart2` (-4): a restartable
  /// sequence, which an interrupt resumes at the start of rather than where it
  /// landed.  Two spellings exist only so that two abutting sequences can be
  /// told apart; they carry the same meaning, and which one a range used is in
  /// `GoUnsafePointRange::NativeValue`.
  RestartSequence,
  /// `UnsafePointRestartAtEntry` (-5): an interrupt restarts the function.
  RestartAtEntry,
  /// A value the table is not defined to hold.  Kept rather than dropped so
  /// that a range decoded from a table this decoder does not fully understand
  /// is visible as such instead of silently reading as safe.
  Unknown,
};

const char *getGoUnsafePointKindName(GoUnsafePointKind Kind);

/// One stretch of a function over which `PCDATA_UnsafePoint` holds one value.
struct GoUnsafePointRange {
  ExceptionAddressRange Range;
  GoUnsafePointKind Kind = GoUnsafePointKind::Safe;
  /// The value as the table spells it, which is what keeps the two restart
  /// spellings distinguishable after the kind has normalized them together.
  int32_t NativeValue = -1;
};

/// One bitmap of a `runtime.stackmap`: which pointer-sized slots of a frame
/// region hold a live pointer at the program points the bitmap covers.
struct GoStackMapBitmap {
  /// Index within the owning map, i.e. the `PCDATA_StackMapIndex` value that
  /// selects this bitmap.
  uint32_t Index = 0;
  /// `stackmap.nbit`: how many slots the bitmap describes.  Slot \p I lies at
  /// `I * PtrSize` from the base of the region the map covers -- the argument
  /// area for `FUNCDATA_ArgsPointerMaps`, the locals area below varp for
  /// `FUNCDATA_LocalsPointerMaps`.
  uint32_t BitCount = 0;
  /// The bitmap itself, least significant bit of the first byte first.
  std::vector<uint8_t> Bits;

  bool isPointerSlot(uint32_t Slot) const {
    if (Slot >= BitCount || Slot / 8 >= Bits.size())
      return false;
    return ((Bits[Slot / 8] >> (Slot % 8)) & 1) != 0;
  }
};

/// A decoded `runtime.stackmap`: `n` bitmaps of `nbit` bits each, laid out
/// consecutively with each starting on a byte boundary.
struct GoStackMap {
  va_t RecordVA = 0;
  /// `stackmap.nbit`, repeated here because it is a property of the map rather
  /// than of any one bitmap and a map with no bitmaps still declares it.
  uint32_t BitCount = 0;
  std::vector<GoStackMapBitmap> Bitmaps;

  /// Caps on what a record is allowed to claim before it is treated as not
  /// being a `stackmap` at all.  All three are far above what a Go frame
  /// reaches and far below what would let a mis-resolved funcdata pointer turn
  /// into a large allocation; the last is needed because the first two
  /// multiply, and their product is thirty megabytes for one map.
  static constexpr uint32_t MaxBitmaps = 1u << 12;
  static constexpr uint32_t MaxBits = 1u << 16;
  static constexpr uint32_t MaxTotalBytes = 1u << 20;
};

/// One stretch of a function over which `PCDATA_StackMapIndex` selects one
/// bitmap out of both of the function's pointer maps.
struct GoStackMapRange {
  ExceptionAddressRange Range;
  /// Bitmap index the range selects.  Negative where the table leaves the
  /// value unset, which the runtime reads as "this is the prologue" and
  /// resolves by using index 0; that fallback is the runtime's policy rather
  /// than something the table said, so it is not applied here.
  int32_t Index = -1;
};

/// A `runtime.gorecover` call site: the only place a panicking goroutine can
/// be brought back under program control.
struct GoRecoverSite {
  va_t CallVA = 0;
  /// True when the recover call is lexically inside a deferred closure, which
  /// is the only position where `recover()` is defined to do anything.
  bool InDeferredFrame = false;
};

/// A `runtime.gopanic` call site, including the compiler-inserted panics for
/// bounds, nil, and division checks.
struct GoPanicSite {
  va_t CallVA = 0;
  /// Runtime entry point the site calls, e.g. `runtime.gopanic`,
  /// `runtime.goPanicIndex`, `runtime.panicdivide`.
  std::string RuntimeName;
  /// True for a compiler-inserted check rather than a user `panic(...)`.
  bool IsImplicitCheck = false;
};

/// Normalized Go frame metadata for one function.
struct GoFunctionEH {
  va_t EntryVA = 0;
  std::string Name;
  /// `runtime._func.deferreturn`: offset of the `deferreturn` call from the
  /// function entry, or nullopt when the function defers nothing.
  std::optional<uint32_t> DeferReturnOffset;
  /// Raw `runtime._func.flag` bits.
  uint8_t FuncFlags = 0;
  /// Raw `runtime._func.funcID`.  A nonzero ID names a function the runtime
  /// itself treats specially, which is how `runtime.gopanic` and
  /// `runtime.sigpanic` are identified without trusting a symbol name.
  uint8_t FuncID = 0;
  /// Frame size from the function's `pcsp` table at entry, when available.
  std::optional<int32_t> FrameSize;
  bool UsesOpenCodedDefers = false;
  /// Present when `FUNCDATA_OpenCodedDeferInfo` was decoded.
  std::optional<GoOpenCodedDeferInfo> OpenCodedDeferInfo;
  std::vector<GoOpenCodedDefer> OpenCodedDefers;
  std::vector<GoDeferSite> Defers;
  std::vector<GoRecoverSite> Recovers;
  std::vector<GoPanicSite> Panics;
  /// `PCDATA_UnsafePoint` as a partition of the body, safe stretches included,
  /// so that a stretch the table never covered is distinguishable from one it
  /// covered and called safe.  Empty for a function that declares no such
  /// table, and for every function of a pre-Go 1.16 image, where the table did
  /// not exist.
  std::vector<GoUnsafePointRange> UnsafePointRanges;
  /// `FUNCDATA_ArgsPointerMaps`, describing the incoming argument area.
  std::optional<GoStackMap> ArgsPointerMap;
  /// `FUNCDATA_LocalsPointerMaps`, describing the locals area below varp.
  std::optional<GoStackMap> LocalsPointerMap;
  /// `PCDATA_StackMapIndex`: which bitmap of both maps above is live where.
  std::vector<GoStackMapRange> StackMapRanges;

  bool hasExceptionalControlFlow() const {
    return DeferReturnOffset.has_value() || !Defers.empty() ||
           !Recovers.empty() || !Panics.empty();
  }
};

/// Image-wide state recovered from the Go runtime's `pclntab` and the
/// `moduledata` that anchors it.  A Go image needs both: the `pclntab` holds
/// the per-function records, but the offsets inside them are relative to bases
/// that only `moduledata` names.
struct GoModuleInfo {
  /// Version implied by the `pcHeader` magic, e.g. "go1.20".
  std::string PclnTabVersion;
  uint32_t PclnTabMagic = 0;
  va_t PcHeaderVA = 0;
  va_t ModuleDataVA = 0;
  /// Base that `functab.entryoff` and `_func.entryOff` are relative to.
  va_t TextBase = 0;
  /// Base that `_func` funcdata offsets are relative to (`go:func.*`).  Zero
  /// when `moduledata` could not be located, in which case funcdata — and so
  /// open-coded defer info — is unreadable while everything else still is.
  va_t GoFuncBase = 0;
  va_t FuncNameTabVA = 0;
  va_t PcTabVA = 0;
  va_t FuncTabVA = 0;
  uint64_t FunctionCount = 0;
  uint8_t MinLC = 0;
  uint8_t PtrSize = 0;
  /// True when the image splits text across several sections, so an entry
  /// offset must be mapped through `moduledata.textsectmap` rather than simply
  /// added to `TextBase`.
  bool HasMultipleTextSections = false;
  /// True when the `_func` records lack `deferreturn`, `funcID`, and `flag`,
  /// and spell `nfuncdata` as a full word instead of the record's last byte.
  /// Only the Go 1.2 magic can set this: it spans Go 1.2 through Go 1.15 and
  /// the record grew those fields in Go 1.12 without the magic changing, so
  /// the shape has to be inferred from the records rather than read off the
  /// header.  Every later magic implies the fields are present.
  bool UsesPreGo112FuncLayout = false;
  /// Position of `PCDATA_StackMapIndex` in each `_func`'s pcdata array.  Go
  /// 1.13 moved it from 0 to 1 without changing the magic, so on the Go 1.2
  /// layout this records which position the pointer maps proved; nullopt when
  /// nothing proved one, in which case no function carries stack map ranges.
  std::optional<uint32_t> StackMapPCDataIndex;
  /// Spelling of `FUNCDATA_OpenCodedDeferInfo` the image's records were read
  /// with.  Go 1.22 changed it without changing the pclntab magic, so this is
  /// what the records themselves proved rather than what the header declared.
  GoOpenCodedDeferLayout OpenCodedDeferLayout =
      GoOpenCodedDeferLayout::Contiguous;
};

} // namespace neverd

#endif // NEVERD_LOADER_LANGUAGEEHGO_H
