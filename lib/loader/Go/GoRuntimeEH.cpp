//===- GoRuntimeEH.cpp - Go runtime frame metadata -----------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "neverd/loader/Go/GoRuntimeEH.h"

#include "GoRuntimeDetail.h"

#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/loader/DirectBranch.h"
#include "neverd/loader/Limits.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
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

/// The runtime write barrier routine the pclntab names \p Name at \p Entry
/// on \p A, or none for any other function (GoWriteBarrierKind).
std::optional<GoWriteBarrier> writeBarrierAt(Arch A, va_t Entry,
                                             llvm::StringRef Name) {
  if (A != Arch::X64 && A != Arch::AArch64)
    return std::nullopt;
  GoWriteBarrier Barrier;
  Barrier.Entry = Entry;
  // The numbered barriers' shared body is an assembly local
  // (`gcWriteBarrier<>`), which the pclntab names without its package.  The
  // caller keeps it only beside a numbered barrier.
  if (Name == "gcWriteBarrier") {
    Barrier.Kind = GoWriteBarrierKind::BufferBody;
    return Barrier;
  }
  if (!Name.consume_front("runtime.gcWriteBarrier"))
    return std::nullopt;
  if (Name.size() == 1 && Name.front() >= '1' && Name.front() <= '8') {
    Barrier.Kind = GoWriteBarrierKind::Buffer;
    return Barrier;
  }
  Barrier.Kind = GoWriteBarrierKind::Store;
  if (A == Arch::AArch64) {
    if (!Name.empty())
      return std::nullopt;
    Barrier.Destination = a64reg::X2;
    Barrier.Value = a64reg::X3;
    return Barrier;
  }
  // Go 1.16-1.20 amd64 also takes the written pointer in another register.
  static constexpr std::pair<llvm::StringLiteral, uint64_t> ValueRegisters[] = {
      {"", x86reg::RAX},   {"CX", x86reg::RCX}, {"DX", x86reg::RDX},
      {"BX", x86reg::RBX}, {"BP", x86reg::RBP}, {"SI", x86reg::RSI},
      {"R8", x86reg::R8},  {"R9", x86reg::R9}};
  for (const auto &[Suffix, Register] : ValueRegisters)
    if (Name == Suffix) {
      Barrier.Destination = x86reg::RDI;
      Barrier.Value = Register;
      return Barrier;
    }
  return std::nullopt;
}

} // namespace

bool hasGoRuntimeMetadata(const BinaryImage &Img) {
  const ImageReader R(Img);
  return findPcHeader(R, Img).has_value();
}

void parseGoExceptions(BinaryImage &Img) {
  if (!Img.LoadOnlyFunctionEntries.empty())
    return;
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
    const va_t NameOffset = static_cast<va_t>(G.Raw.NameOffset);
    if (NameOffset <= InvalidVA - Header->FuncNameTab) {
      if (std::optional<std::string> Name =
              R.cstring(Header->FuncNameTab + NameOffset))
        G.Name = std::move(*Name);
    }
    Funcs.push_back(std::move(G));
  }

  // The code Go's register contract governs, and the write barriers that
  // depart from it (GoModuleInfo::GoCode, GoModuleInfo::WriteBarriers).
  for (const GoFunction &G : Funcs)
    if (G.Raw.PcSP != 0 && G.CodeRange.isValid())
      Module.GoCode.push_back(G.CodeRange);
  llvm::sort(Module.GoCode,
             [](const ExceptionAddressRange &A,
                const ExceptionAddressRange &B) { return A.Begin < B.Begin; });
  std::vector<ExceptionAddressRange> Merged;
  for (const ExceptionAddressRange &Range : Module.GoCode)
    if (!Merged.empty() && Range.Begin <= Merged.back().End)
      Merged.back().End = std::max(Merged.back().End, Range.End);
    else
      Merged.push_back(Range);
  Module.GoCode = std::move(Merged);
  const bool TextBaseProven =
      !Layout.EntryIsOffset || MD.TextBase != 0 || Header->TextStart != 0;
  if (TextBaseProven)
    for (const GoFunction &G : Funcs)
      if (G.Raw.PcSP != 0 && G.CodeRange.isValid())
        if (std::optional<GoWriteBarrier> Barrier =
                writeBarrierAt(Img.Arch, G.CodeRange.Begin, G.Name))
          Module.WriteBarriers.push_back(*Barrier);
  // Only the numbered barriers jump to the body they share: a function of
  // its bare name in a module that has none of them is something else.
  if (llvm::none_of(Module.WriteBarriers, [](const GoWriteBarrier &Barrier) {
        return Barrier.Kind == GoWriteBarrierKind::Buffer;
      }))
    llvm::erase_if(Module.WriteBarriers, [](const GoWriteBarrier &Barrier) {
      return Barrier.Kind == GoWriteBarrierKind::BufferBody;
    });
  llvm::sort(Module.WriteBarriers,
             [](const GoWriteBarrier &A, const GoWriteBarrier &B) {
               return A.Entry < B.Entry;
             });
  // `FuncFlagAsm` (internal/abi/symtab.go) exists from Go 1.18 on.
  constexpr uint8_t FuncFlagAsm = 1u << 2;
  if (Header->Magic == Go118Magic || Header->Magic == Go120Magic) {
    Module.RegisterArguments =
        Img.Arch == Arch::X64 || Img.Arch == Arch::AArch64;
    for (const GoFunction &G : Funcs)
      if (G.Raw.PcSP != 0 && (G.Raw.Flag & FuncFlagAsm) &&
          G.CodeRange.isValid())
        Module.GoAssembly.push_back(G.CodeRange);
    llvm::sort(Module.GoAssembly, [](const ExceptionAddressRange &A,
                                     const ExceptionAddressRange &B) {
      return A.Begin < B.Begin;
    });
    std::vector<ExceptionAddressRange> Assembly;
    for (const ExceptionAddressRange &Range : Module.GoAssembly)
      if (!Assembly.empty() && Range.Begin <= Assembly.back().End)
        Assembly.back().End = std::max(Assembly.back().End, Range.End);
      else
        Assembly.push_back(Range);
    Module.GoAssembly = std::move(Assembly);
  }
  if (TextBaseProven)
    for (const GoFunction &G : Funcs) {
      if (G.Raw.PcSP == 0 || !G.CodeRange.isValid())
        continue;
      if (G.Name == "runtime.morestack" ||
          G.Name == "runtime.morestack_noctxt" ||
          G.Name == "runtime.morestackc") {
        Module.MorestackEntries.push_back(G.CodeRange.Begin);
        if (G.Name == "runtime.morestackc")
          Module.ThrowingMorestackEntries.push_back(G.CodeRange.Begin);
      }
      if (G.Name == "runtime.duffzero" || G.Name == "runtime.duffcopy")
        Module.DuffDevices.push_back(G.CodeRange);
      if (G.Name == "runtime.spillArgs" || G.Name == "runtime.unspillArgs")
        Module.RegisterArgumentTransfers.push_back(G.CodeRange);
    }
  llvm::sort(Module.MorestackEntries);
  llvm::sort(Module.ThrowingMorestackEntries);
  for (auto *Ranges : {&Module.DuffDevices, &Module.RegisterArgumentTransfers})
    llvm::sort(*Ranges, [](const ExceptionAddressRange &A,
                           const ExceptionAddressRange &B) {
      return A.Begin < B.Begin;
    });
  // Before Go's internal ABI every function takes its arguments and returns
  // its results on the stack.  arm64 gained the ABI with the Go 1.18 pclntab;
  // amd64 a release earlier, under the 1.16 one, so there the prologues
  // decide: Go 1.17 compares the stack with g in R14 (`cmp rsp, [r14+0x10]`),
  // and earlier releases load g from thread-local storage first (`mov rcx,
  // fs:[-8]`, or gs on Darwin and Windows).
  if (TextBaseProven && !Module.RegisterArguments &&
      (Header->Magic == Go12Magic || Header->Magic == Go116Magic)) {
    if (Img.Arch == Arch::AArch64) {
      Module.StackArguments = true;
    } else if (Img.Arch == Arch::X64) {
      bool ThreadLocalG = false, RegisterG = false;
      constexpr unsigned Window =
          ::neverd::limits::loader::kGoStackCheckPrologueBytes;
      for (const GoFunction &G : Funcs) {
        if (G.Raw.PcSP == 0 || !G.CodeRange.isValid() ||
            G.CodeRange.End - G.CodeRange.Begin < Window)
          continue;
        const uint8_t *Code = Img.readVA(G.CodeRange.Begin, Window);
        if (!Code)
          continue;
        ThreadLocalG |= (Code[0] == 0x64 || Code[0] == 0x65) &&
                        Code[1] == 0x48 && Code[2] == 0x8B && Code[3] == 0x0C &&
                        Code[4] == 0x25;
        for (unsigned I = 0; I + 4 <= Window; ++I)
          RegisterG |= (Code[I] == 0x49 || Code[I] == 0x4D) &&
                       Code[I + 1] == 0x3B && Code[I + 2] == 0x66 &&
                       (Code[I + 3] == 0x10 || Code[I + 3] == 0x18);
      }
      Module.StackArguments = ThreadLocalG && !RegisterG;
    }
  }
  // Only where Go's argument passing is modelled: the register ABI, and the
  // stack ABI with its register stubs (goRegisterStubArguments).  Go 1.17's
  // register ABI under the 1.16 pclntab is neither.
  if (TextBaseProven && (Module.RegisterArguments || Module.StackArguments))
    for (const GoFunction &G : Funcs)
      if (G.Raw.PcSP != 0 && G.CodeRange.isValid() &&
          goRuntimeNeverReturns(G.Name))
        Module.NoReturnEntries.push_back(G.CodeRange.Begin);
  llvm::sort(Module.NoReturnEntries);
  // ArgInfo sits at index 5 of the Go 1.16 numbering from Go 1.17; before
  // Go 1.16 that index held the open-coded defer record.
  const bool ArgInfoIndexed =
      Layout.OpenCodedDeferInfoIndex == FuncDataOpenCodedDeferInfo &&
      (MD.GoFuncBase != 0 || Layout.FuncDataIsPointer);
  if (TextBaseProven)
    for (const GoFunction &G : Funcs)
      if (G.CodeRange.isValid()) {
        GoFrameArguments Frame;
        Frame.Entry = G.CodeRange.Begin;
        if (G.Raw.Args >= 0)
          Frame.Bytes = static_cast<uint32_t>(G.Raw.Args);
        if (ArgInfoIndexed)
          if (const std::optional<va_t> Table = getFuncDataAddress(
                  R, Layout, G.Raw, FuncDataArgInfo, MD.GoFuncBase)) {
            Frame.ArgumentComponents = readArgumentComponents(R, *Table);
            // The compiler leaves the first parameter out of a function
            // whose symbol names type arguments, taking it for a generic
            // dictionary (`EmitArgInfo`): it is one for a shape function,
            // and `p` for `type:.eq.sync/atomic.Pointer[T]`.
            if (Frame.ArgumentComponents &&
                G.Name.find('[') != std::string::npos &&
                *Frame.ArgumentComponents < UINT8_MAX)
              ++*Frame.ArgumentComponents;
          }
        Module.FrameArguments.push_back(Frame);
      }
  llvm::sort(Module.FrameArguments,
             [](const GoFrameArguments &A, const GoFrameArguments &B) {
               return A.Entry < B.Entry;
             });
  if (TextBaseProven)
    for (const GoFunction &G : Funcs)
      if (G.Raw.PcSP != 0 && G.CodeRange.isValid() &&
          G.Name == "runtime.callbackasm1")
        Module.ReturnsPastCallerEntries.push_back(G.CodeRange.Begin);
  llvm::sort(Module.ReturnsPastCallerEntries);
  // Go 1.26 reports every bounds failure through one routine and records, at
  // each call to it, which registers or constants hold the failing index and
  // length (PCDATA_PanicBounds, internal/abi/symtab.go).
  constexpr unsigned PCDataPanicBounds = 4;
  if (TextBaseProven)
    for (const GoFunction &G : Funcs)
      if (G.Raw.PcSP != 0 && G.CodeRange.isValid() &&
          G.Name == "runtime.panicBounds")
        Module.PanicBoundsEntry = G.CodeRange.Begin;
  if (Module.PanicBoundsEntry != InvalidVA) {
    for (const GoFunction &G : Funcs) {
      if (G.Raw.PcSP == 0 || !G.CodeRange.isValid())
        continue;
      const std::optional<uint32_t> Offset =
          getPCDataOffset(R, Layout, G.Raw, PCDataPanicBounds);
      if (!Offset || *Offset == 0)
        continue;
      // A table that does not decode to its end holds nothing to trust.
      std::vector<GoPanicBoundsRange> Ranges;
      const bool Walked = forEachPCValue(
          R, Header->PcTab, *Offset, G.CodeRange.Begin, Header->MinLC,
          [&](const PCValueRange &V) {
            if (Ranges.size() >= MaxPCValueRanges)
              return false;
            if (V.Value >= 0)
              Ranges.push_back(
                  {ExceptionAddressRange{V.Begin, V.End}, V.Value});
            return true;
          });
      if (Walked)
        Module.PanicBounds.insert(Module.PanicBounds.end(), Ranges.begin(),
                                  Ranges.end());
    }
    llvm::sort(Module.PanicBounds,
               [](const GoPanicBoundsRange &A, const GoPanicBoundsRange &B) {
                 return A.Range.Begin < B.Range.Begin;
               });
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
      if (S.Origin < NameOrigin::Stated) {
        S.Name = It->second->Name;
        S.Origin = NameOrigin::Stated;
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
      S.Origin = NameOrigin::Stated;
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
  const size_t BranchLength = getDirectBranchLength(Img.Arch);
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

    // Attribute the branch sites in this body.  A site is read exactly as
    // far as a direct branch of the architecture reaches -- five bytes on x86,
    // one instruction elsewhere -- which is the span readVA proves mapped, so
    // the decoder never inspects a byte past it.
    if (G.CodeRange.isValid() && !RuntimeTargets.empty()) {
      const uint64_t Size = G.CodeRange.size();
      for (uint64_t Off = 0; Off + BranchLength <= Size; Off += Stride) {
        const va_t SiteVA = G.CodeRange.Begin + Off;
        const uint8_t *Code = Img.readVA(SiteVA, BranchLength);
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
            Img.Arch, Img.Mode, Code, BranchLength, SiteVA, Length);
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

namespace neverd {

bool goRuntimeNeverReturns(std::string_view Name) {
  constexpr std::string_view Prefix = "runtime.";
  if (Name.substr(0, Prefix.size()) != Prefix)
    return false;
  const std::string_view Routine = Name.substr(Prefix.size());
  // The bounds-check panics: Go 1.26's one (panicExtend on 32-bit targets),
  // the earlier families, each with a Go half named goPanic..., their 32-bit
  // panicExtend... forms, and Go 1.10's panicindex and panicslice.
  if (Routine == "panicBounds" || Routine == "panicExtend" ||
      Routine == "panicindex" || Routine == "panicslice")
    return true;
  constexpr std::string_view Kinds[] = {
      "Index",      "IndexU",      "SliceAlen", "SliceAlenU", "SliceAcap",
      "SliceAcapU", "SliceB",      "SliceBU",   "Slice3Alen", "Slice3AlenU",
      "Slice3Acap", "Slice3AcapU", "Slice3B",   "Slice3BU",   "Slice3C",
      "Slice3CU",   "SliceConvert"};
  for (const std::string_view Family :
       {std::string_view("panic"), std::string_view("goPanic"),
        std::string_view("panicExtend"), std::string_view("goPanicExtend")})
    if (Routine.substr(0, Family.size()) == Family)
      for (const std::string_view Kind : Kinds)
        if (Routine.substr(Family.size()) == Kind)
          return true;
  constexpr std::string_view Exits[] = {
      // The calls ssagen ends a block with by name.
      "throw", "throwinit", "gopanic", "panicwrap", "block",
      "panicmakeslicelen", "panicmakeslicecap", "panicunsafeslicelen",
      "panicunsafeslicenilptr", "panicunsafestringlen",
      "panicunsafestringnilptr", "panicrangestate",
      // The calls ssagen makes with rtcall(fn, returns=false).
      "panicdivide", "panicshift", "panicdottypeE", "panicdottypeI",
      "panicnildottype", "panicoverflow", "panicSimdImm",
      // The routines that end the process.
      "fatal", "fatalthrow", "fatalpanic"};
  return llvm::is_contained(Exits, Routine);
}

} // namespace neverd
