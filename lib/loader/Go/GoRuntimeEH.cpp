//===- GoRuntimeEH.cpp - Go runtime frame metadata -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/Go/GoRuntimeEH.h"

#include "GoRuntimeDetail.h"

#include "neverd/loader/DirectBranch.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace neverd::go_loader {

using namespace detail;

namespace {

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

  // Settled once for the image, because the evidence is statistical: a single
  // record can read as either spelling, and reaching a record at all needs the
  // funcdata base the module walk just confirmed.
  GoOpenCodedDeferLayout DeferLayout = GoOpenCodedDeferLayout::Contiguous;
  if (MD.GoFuncBase != 0 || Layout.FuncDataIsPointer)
    DeferLayout = resolveOpenCodedDeferLayout(R, Layout, *Header, RawFuncs,
                                              MD.GoFuncBase);

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
  Module.OpenCodedDeferLayout = DeferLayout;
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
      if (std::optional<va_t> RecordVA = getFuncDataAddress(
              R, Layout, G.Raw, Layout.OpenCodedDeferInfoIndex,
              MD.GoFuncBase)) {
        EH.UsesOpenCodedDefers = true;
        std::optional<OpenCodedDeferRecord> Record = readOpenCodedDeferInfo(
            R, *RecordVA, DeferLayout, R.pointerSize(), EH.FrameSize);
        if (Record) {
          GoOpenCodedDeferInfo OpenInfo;
          OpenInfo.Layout = Record->Layout;
          OpenInfo.DeferBitsOffset = Record->DeferBits;
          OpenInfo.SlotsOffset = Record->FirstSlot;
          OpenInfo.SlotCountIsExact =
              Record->Layout != GoOpenCodedDeferLayout::Contiguous;
          EH.OpenCodedDeferInfo = OpenInfo;
          for (uint32_t Slot : Record->Slots)
            EH.OpenCodedDefers.push_back({-static_cast<int32_t>(Slot)});
          if (!Record->SlotsBounded) {
            // The frame is too deep for the bound to be the slot count, so
            // enumerating it would invent slots the function does not have.
            // The `where` is still sound; only the `how many` is lost.
            Status = ExceptionParseStatus::Partial;
            Diagnostics.push_back(
                "open-coded defer slot array at frame offset -" +
                std::to_string(Record->FirstSlot) +
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
        // x86 direct branches are five bytes; every fixed-width form decoded
        // here is four.  Pass exactly the span readVA proved mapped so the
        // decoder can never inspect an unvalidated fifth byte.
        const size_t DecodeWindow =
            Img.Arch == Arch::X64 || Img.Arch == Arch::X86 ? 5 : 4;
        const size_t ReadSize = std::min(Available, DecodeWindow);
        const uint8_t *Code = Img.readVA(SiteVA, ReadSize);
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
            Img.Arch, Img.Mode, Code, ReadSize, SiteVA, Length);
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
