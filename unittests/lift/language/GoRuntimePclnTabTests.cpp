//===- GoRuntimePclnTabTests.cpp - Go 1.2 function table tests --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "GoRuntimeEHTestsDetail.h"
#include "gtest/gtest.h"

#include "neverd/libc/LibCNames.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"

namespace {

using namespace neverd;
using namespace neverd::go_loader;
using namespace neverd::go_eh_test;

//===----------------------------------------------------------------------===//
// Go 1.2 layout
//===----------------------------------------------------------------------===//

TEST(GoLegacyPclnTab, ReadsTheGo12FunctionTable) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.FuncData = {std::nullopt, std::nullopt};
  GoFuncSpec Other = makeDeferringFunc("main.other", kTextVA + 0x200);
  Other.DeferReturn = 0;
  Other.FuncData = {std::nullopt, std::nullopt};
  T.installPclnTab(
      buildPclnTab(kGo12Magic, {Work, Other}, kTextVA + 0x300).Bytes);

  EXPECT_TRUE(hasGoRuntimeMetadata(T.Img));
  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_EQ(Info.GoModule->PclnTabMagic, kGo12Magic);
  EXPECT_EQ(Info.GoModule->PclnTabVersion, "go1.2");
  EXPECT_EQ(Info.GoModule->FunctionCount, 2u);
  EXPECT_EQ(Info.GoModule->PcHeaderVA, kPclnVA);
  // Names, pc-value tables, and `_func` records all live at offsets from the
  // head of the table, so all three bases are the header itself.
  EXPECT_EQ(Info.GoModule->FuncNameTabVA, kPclnVA);
  EXPECT_EQ(Info.GoModule->PcTabVA, kPclnVA);
  EXPECT_EQ(Info.GoModule->FuncTabVA, kPclnVA + 16);
  EXPECT_FALSE(Info.GoModule->UsesPreGo112FuncLayout);

  // The table is a symbol table too, and on this layout the entries it names
  // are absolute addresses rather than offsets from a base.
  bool NamedWork = false;
  for (const Symbol &S : T.Img.Symbols)
    if (S.Name == "main.work") {
      NamedWork = true;
      EXPECT_EQ(S.Addr, kTextVA + 0x100);
      EXPECT_EQ(S.Size, 0x100u);
    }
  EXPECT_TRUE(NamedWork);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_EQ(F->CodeRange.Begin, kTextVA + 0x100);
  EXPECT_EQ(F->CodeRange.End, kTextVA + 0x200);
  ASSERT_TRUE(F->Go->DeferReturnOffset.has_value());
  EXPECT_EQ(*F->Go->DeferReturnOffset, 0x20u);
  ASSERT_TRUE(F->Go->FrameSize.has_value());
  EXPECT_EQ(*F->Go->FrameSize, 0x30);
  // The sentinel closes the last function, which is what gives `main.other`
  // an end at all.
  EXPECT_EQ(findRecord(Info, kTextVA + 0x200), nullptr);
}

TEST(GoLegacyPclnTab, ReportsThatTheUnsafePointTableDoesNotExistYet) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  // Table 0 is a register map index on this layout, never an unsafe point
  // table, so decoding it as one would report async-preemption facts the
  // image never stated.
  PCValueTable RegMapIndex;
  RegMapIndex.Steps = {{-1, 0x40}, {0, 0x40}};
  Work.PCData = {RegMapIndex};
  T.installPclnTab(buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  EXPECT_TRUE(anyDiagnosticContains(Info.Diagnostics, "PCDATA_UnsafePoint"));
  EXPECT_EQ(Info.ParseStatus, ExceptionParseStatus::Partial);
  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UnsafePointRanges.empty());
}

TEST(GoLegacyPclnTab, ReadsThePreGo112RecordShape) {
  GoTestImage T;
  // The older shape has no `deferreturn`, so a record is only kept when the
  // body reaches a runtime entry point the table also names.
  GoFuncSpec Work;
  Work.Name = "main.work";
  Work.EntryVA = kTextVA + 0x100;
  Work.DeferReturn = 0x20; // the legacy frame size field, not a code offset
  Work.PcSP.Steps = {{0x30, 0x80}};
  Work.FuncData = {std::nullopt, std::nullopt};
  GoFuncSpec Panic;
  Panic.Name = "runtime.gopanic";
  Panic.EntryVA = kTextVA + 0x400;
  Panic.FuncData = {std::nullopt};
  T.installPclnTab(buildPclnTab(kGo12Magic, {Work, Panic}, kTextVA + 0x500,
                                /*MinLC=*/1, /*PreGo112Record=*/true)
                       .Bytes);
  T.writeText(kTextVA + 0x110, makeCall(kTextVA + 0x110, kTextVA + 0x400));

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_TRUE(Info.GoModule->UsesPreGo112FuncLayout);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_EQ(F->Go->Panics.size(), 1u);
  EXPECT_EQ(F->Go->Panics[0].CallVA, kTextVA + 0x110);
  EXPECT_EQ(F->Go->Panics[0].RuntimeName, "runtime.gopanic");
  // The word the newer shape spends on `deferreturn` held the frame size
  // here, and reporting it would name an address in another function.
  EXPECT_FALSE(F->Go->DeferReturnOffset.has_value());
  EXPECT_EQ(F->Go->FuncID, 0);
}

TEST(GoLegacyPclnTab, ProvesWhichPCDataTableHoldsTheStackMapIndex) {
  GoTestImage T;
  const va_t LocalsVA = T.addPayload(buildStackMap(2, {{0b01}, {0b10}}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  // Table 0 names an index the map cannot satisfy, which is what rules it out;
  // table 1 stays inside the map's two bitmaps.
  PCValueTable RegMapIndex;
  RegMapIndex.Steps = {{5, 0x80}};
  PCValueTable StackMapIndex;
  StackMapIndex.Steps = {{0, 0x40}, {1, 0x40}};
  Work.PCData = {RegMapIndex, StackMapIndex};
  Work.FuncData = {std::nullopt, LocalsVA};
  T.installPclnTab(buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  ASSERT_TRUE(Info.GoModule->StackMapPCDataIndex.has_value());
  EXPECT_EQ(*Info.GoModule->StackMapPCDataIndex, 1u);

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  ASSERT_EQ(F->Go->StackMapRanges.size(), 2u);
  EXPECT_EQ(F->Go->StackMapRanges[0].Index, 0);
  EXPECT_EQ(F->Go->StackMapRanges[1].Index, 1);
}

TEST(GoLegacyPclnTab, ReportsNoStackMapRangesWhenBothCandidatesSurvive) {
  GoTestImage T;
  const va_t LocalsVA = T.addPayload(buildStackMap(2, {{0b01}, {0b10}}));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  PCValueTable Ambiguous;
  Ambiguous.Steps = {{0, 0x40}, {1, 0x40}};
  Work.PCData = {Ambiguous, Ambiguous};
  Work.FuncData = {std::nullopt, LocalsVA};
  T.installPclnTab(buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_FALSE(Info.GoModule->StackMapPCDataIndex.has_value());
  EXPECT_TRUE(anyDiagnosticContains(Info.Diagnostics, "PCDATA_StackMapIndex"));

  const ExceptionFunction *F = findRecord(Info, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  // Nothing proved which table selects a bitmap, but the bitmaps themselves
  // were never in doubt.
  EXPECT_TRUE(F->Go->LocalsPointerMap.has_value());
  EXPECT_TRUE(F->Go->StackMapRanges.empty());
}

TEST(GoLegacyPclnTab, ReadsOpenCodedDeferInfoFromItsPreGo116Index) {
  GoTestImage T;
  const va_t DeferInfoVA = T.addPayload(buildContiguousDeferInfo(0x18, 0x10));
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  Work.PcSP.Steps = {{0x20, 0x80}};
  // Six entries is the shape only Go 1.14 and later emit, which is what makes
  // index five unambiguous on a table this old.
  Work.FuncData = {std::nullopt, std::nullopt, std::nullopt,
                   std::nullopt, std::nullopt, DeferInfoVA};
  T.installPclnTab(buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200).Bytes);

  parseGoExceptions(T.Img);

  const ExceptionFunction *F =
      findRecord(T.Img.ExceptionMetadata, kTextVA + 0x100);
  ASSERT_NE(F, nullptr);
  EXPECT_TRUE(F->Go->UsesOpenCodedDefers);
  ASSERT_TRUE(F->Go->OpenCodedDeferInfo.has_value());
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->DeferBitsOffset, 0x18u);
  EXPECT_EQ(F->Go->OpenCodedDeferInfo->SlotsOffset, 0x10u);
  ASSERT_EQ(F->Go->OpenCodedDefers.size(), 2u);
  EXPECT_EQ(F->Go->OpenCodedDefers[0].ClosureOffset, -0x10);
  EXPECT_EQ(F->Go->OpenCodedDefers[1].ClosureOffset, -0x08);
}

TEST(GoLegacyPclnTab, RefusesAHeaderWhoseFileTableOffsetIsMissing) {
  GoTestImage T;
  GoFuncSpec Work = makeDeferringFunc("main.work", kTextVA + 0x100);
  BuiltPclnTab Tab = buildPclnTab(kGo12Magic, {Work}, kTextVA + 0x200);
  // The slot behind the functab sentinel is the only thing that says where
  // the file table is; a zero there is not a table this decoder can walk.
  Tab.put32(16 + 3 * 8, 0);
  T.installPclnTab(Tab.Bytes);

  EXPECT_FALSE(hasGoRuntimeMetadata(T.Img));
  parseGoExceptions(T.Img);
  EXPECT_FALSE(T.Img.ExceptionMetadata.GoModule.has_value());
}

TEST(GoLegacyPclnTab, StopsAtAFunctabEntryPointingOutsideTheImage) {
  GoTestImage T;
  GoFuncSpec First = makeDeferringFunc("main.first", kTextVA + 0x100);
  GoFuncSpec Second = makeDeferringFunc("main.second", kTextVA + 0x200);
  BuiltPclnTab Tab = buildPclnTab(kGo12Magic, {First, Second}, kTextVA + 0x300);
  // Second entry's funcoff, i.e. the fourth word of the functab.
  Tab.put64(16 + 3 * 8, 0x00F00000);
  T.installPclnTab(Tab.Bytes);

  parseGoExceptions(T.Img);

  const ExceptionInfo &Info = T.Img.ExceptionMetadata;
  ASSERT_TRUE(Info.GoModule.has_value());
  EXPECT_EQ(Info.GoModule->FunctionCount, 1u);
  EXPECT_TRUE(anyDiagnosticContains(Info.Diagnostics, "ended early"));
  EXPECT_EQ(Info.ParseStatus, ExceptionParseStatus::Partial);
}

//===----------------------------------------------------------------------===//
// Go code and the runtime's write barriers
//===----------------------------------------------------------------------===//

/// A function the Go toolchain built: its record carries a pc-SP table.
GoFuncSpec makeGoCode(const std::string &Name, va_t EntryVA) {
  GoFuncSpec F;
  F.Name = Name;
  F.EntryVA = EntryVA;
  F.PcSP.Steps = {{0, 0x40}};
  return F;
}

TEST(GoCodeRanges, LeaveOutCodeWhoseRecordHasNoFrameTable) {
  // An internally linked cgo image lists its C functions too, without a
  // pc-SP table: they keep the platform's register contract.
  GoTestImage T;
  GoFuncSpec C;
  C.Name = "x_cgo_init";
  C.EntryVA = kTextVA + 0x300;
  T.installPclnTab(buildPclnTab(kGo116Magic,
                                {makeGoCode("main.a", kTextVA + 0x100),
                                 makeGoCode("main.b", kTextVA + 0x200), C,
                                 makeGoCode("main.c", kTextVA + 0x400)},
                                kTextVA + 0x500)
                       .Bytes);
  parseGoExceptions(T.Img);

  const auto &Module = T.Img.ExceptionMetadata.GoModule;
  ASSERT_TRUE(Module.has_value());
  ASSERT_EQ(Module->GoCode.size(), 2u);
  // Adjacent Go functions merge into one range.
  EXPECT_EQ(Module->GoCode[0].Begin, kTextVA + 0x100);
  EXPECT_EQ(Module->GoCode[0].End, kTextVA + 0x300);
  EXPECT_EQ(Module->GoCode[1].Begin, kTextVA + 0x400);
  EXPECT_EQ(Module->GoCode[1].End, kTextVA + 0x500);
  EXPECT_TRUE(Module->WriteBarriers.empty());
}

TEST(GoWriteBarriers, NameEachContractTheRuntimeDocuments) {
  GoTestImage T;
  GoFuncSpec Unbuilt = makeGoCode("runtime.gcWriteBarrier9", kTextVA + 0x500);
  T.installPclnTab(
      buildPclnTab(kGo116Magic,
                   {makeGoCode("runtime.gcWriteBarrier", kTextVA + 0x100),
                    makeGoCode("runtime.gcWriteBarrierR8", kTextVA + 0x200),
                    makeGoCode("runtime.gcWriteBarrier3", kTextVA + 0x300),
                    makeGoCode("runtime.gcWriteBarrierAX", kTextVA + 0x400),
                    makeGoCode("gcWriteBarrier", kTextVA + 0x480), Unbuilt},
                   kTextVA + 0x600)
          .Bytes);
  parseGoExceptions(T.Img);

  const auto &Module = T.Img.ExceptionMetadata.GoModule;
  ASSERT_TRUE(Module.has_value());
  ASSERT_EQ(Module->WriteBarriers.size(), 4u);
  // The body the numbered barriers share is an assembly local, which the
  // pclntab names without its package.
  const GoWriteBarrier &Shared = Module->WriteBarriers[3];
  EXPECT_EQ(Shared.Entry, kTextVA + 0x480);
  EXPECT_EQ(Shared.Kind, GoWriteBarrierKind::BufferBody);
  const GoWriteBarrier &Store = Module->WriteBarriers[0];
  EXPECT_EQ(Store.Entry, kTextVA + 0x100);
  EXPECT_EQ(Store.Kind, GoWriteBarrierKind::Store);
  EXPECT_EQ(Store.Destination, x86reg::RDI);
  EXPECT_EQ(Store.Value, x86reg::RAX);
  const GoWriteBarrier &Variant = Module->WriteBarriers[1];
  EXPECT_EQ(Variant.Kind, GoWriteBarrierKind::Store);
  EXPECT_EQ(Variant.Value, x86reg::R8);
  const GoWriteBarrier &Buffer = Module->WriteBarriers[2];
  EXPECT_EQ(Buffer.Entry, kTextVA + 0x300);
  EXPECT_EQ(Buffer.Kind, GoWriteBarrierKind::Buffer);
}

TEST(GoWriteBarriers, ASharedBodyNeedsANumberedBarrier) {
  // Only the numbered barriers jump to the body they share.  An assembly
  // local of that bare name in a module that has none of them, whose runtime
  // still stores through `runtime.gcWriteBarrier`, is something else.
  GoTestImage T;
  T.installPclnTab(
      buildPclnTab(kGo116Magic,
                   {makeGoCode("runtime.gcWriteBarrier", kTextVA + 0x100),
                    makeGoCode("gcWriteBarrier", kTextVA + 0x200)},
                   kTextVA + 0x300)
          .Bytes);
  parseGoExceptions(T.Img);

  const auto &Module = T.Img.ExceptionMetadata.GoModule;
  ASSERT_TRUE(Module.has_value());
  ASSERT_EQ(Module->WriteBarriers.size(), 1u);
  EXPECT_EQ(Module->WriteBarriers[0].Entry, kTextVA + 0x100);
  EXPECT_EQ(Module->WriteBarriers[0].Kind, GoWriteBarrierKind::Store);
}

TEST(GoArgumentComponents, CountEachScalarTheTracebackLists) {
  // A traceback lists each argument's scalar components (FUNCDATA_ArgInfo):
  // an offset and a size each, or an offset too large alone, with structs
  // and arrays bracketed, and the rest elided past its limit.  Under Go's
  // internal ABI each takes one register at most.
  GoTestImage T;
  // gopark(unlockf, lock, reason uint8, traceReason uint8, traceskip int):
  // five components, though the two bytes share a word of `_func.args`.
  const va_t Five = T.addPayload(
      {0x00, 0x08, 0x08, 0x08, 0x10, 0x01, 0x11, 0x01, 0x18, 0x08, 0xFF});
  // f(s string, p struct{ a, b int }): a string's words and a struct's.
  const va_t Aggregates =
      T.addPayload({0xFE, 0x00, 0x08, 0x08, 0x08, 0xFD, 0xFE, 0x10, 0x08, 0x18,
                    0x08, 0xFD, 0xFF});
  // g(huge [32]int, x int): an offset past what a byte holds.
  const va_t Large = T.addPayload({0x00, 0x08, 0xFB, 0xFF});
  // A function named after a generic type: the table leaves its first
  // parameter out, as a shape function's dictionary.
  const va_t Generic = T.addPayload({0x08, 0x08, 0xFF});
  // ten components and more, elided.
  std::vector<uint8_t> Many;
  for (uint8_t I = 0; I < 10; ++I) {
    Many.push_back(static_cast<uint8_t>(8 * I));
    Many.push_back(8);
  }
  Many.push_back(0xFC);
  Many.push_back(0xFF);
  const va_t Elided = T.addPayload(Many);
  std::vector<GoFuncSpec> Funcs;
  const std::pair<const char *, std::optional<va_t>> Tables[] = {
      {"runtime.gopark", Five},
      {"main.f", Aggregates},
      {"main.g", Large},
      {"main.h", Elided},
      {"main.none", std::nullopt},
      {"type:.eq.sync/atomic.Pointer[main.T]", Generic}};
  va_t Entry = kTextVA + 0x100;
  for (const auto &[Name, Table] : Tables) {
    GoFuncSpec F = makeGoCode(Name, Entry);
    if (Table)
      F.FuncData = {std::nullopt, std::nullopt, std::nullopt,
                    std::nullopt, std::nullopt, *Table};
    Funcs.push_back(std::move(F));
    Entry += 0x100;
  }
  T.installPclnTab(buildPclnTab(kGo116Magic, Funcs, Entry).Bytes);
  parseGoExceptions(T.Img);
  const auto &Module = T.Img.ExceptionMetadata.GoModule;
  ASSERT_TRUE(Module.has_value());
  ASSERT_EQ(Module->FrameArguments.size(), 6u);
  EXPECT_EQ(Module->FrameArguments[0].ArgumentComponents, 5);
  EXPECT_EQ(Module->FrameArguments[1].ArgumentComponents, 4);
  EXPECT_EQ(Module->FrameArguments[2].ArgumentComponents, 2);
  EXPECT_FALSE(Module->FrameArguments[3].ArgumentComponents);
  EXPECT_FALSE(Module->FrameArguments[4].ArgumentComponents);
  EXPECT_EQ(Module->FrameArguments[5].ArgumentComponents, 2);
}

TEST(GoNoReturn, NameTheRoutinesTheCompilerEndsABlockWith) {
  for (const char *Name :
       {"runtime.gopanic", "runtime.throw", "runtime.panicBounds",
        "runtime.panicSlice3AcapU", "runtime.goPanicIndex",
        "runtime.panicExtendSliceB", "runtime.panicdivide",
        "runtime.panicdottypeE", "runtime.fatal", "runtime.panicindex"})
    EXPECT_TRUE(goRuntimeNeverReturns(Name)) << Name;
  // Routines that return, or may: a check that throws only sometimes, the
  // growth routine that starts its caller again, the exit syscall whose
  // caller the compiler continues, and names of other packages.
  for (const char *Name :
       {"runtime.panicCheck1", "runtime.printstring", "runtime.morestack",
        "runtime.exit", "runtime.Goexit", "main.throw", "runtime.panicIndexX",
        "runtime.panic"})
    EXPECT_FALSE(goRuntimeNeverReturns(Name)) << Name;
}

TEST(GoNoReturn, AreListedWhereTheImagesArgumentPassingIsKnown) {
  // A Go 1.16 amd64 image whose prologues load g from thread-local storage
  // passes arguments on the stack, its bounds stubs' registers in their own
  // order (goRegisterStubArguments), so its entries are listed.  Go 1.17's
  // `cmp rsp, [r14+0x10]` under the same pclntab is a register ABI the
  // loader does not model, and without a prologue nothing is proved: there
  // they stay unlisted.
  const std::vector<uint8_t> ThreadLocal = {0x64, 0x48, 0x8B, 0x0C, 0x25, 0xF8,
                                            0xFF, 0xFF, 0xFF, 0x48, 0x3B, 0x61,
                                            0x10, 0x76, 0x20, 0xC3};
  const std::vector<uint8_t> Register = {0x49, 0x3B, 0x66, 0x10, 0x76, 0x20,
                                         0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
                                         0xCC, 0xCC, 0xCC, 0xCC};
  enum class Prologue { ThreadLocal, Go117, None };
  for (const Prologue Kind :
       {Prologue::ThreadLocal, Prologue::Go117, Prologue::None}) {
    SCOPED_TRACE(static_cast<int>(Kind));
    GoTestImage T;
    if (Kind != Prologue::None)
      T.writeText(kTextVA + 0x100, ThreadLocal);
    if (Kind == Prologue::Go117)
      T.writeText(kTextVA + 0x200, Register);
    T.installPclnTab(
        buildPclnTab(kGo116Magic,
                     {makeGoCode("runtime.gopanic", kTextVA + 0x100),
                      makeGoCode("main.f", kTextVA + 0x200)},
                     kTextVA + 0x300)
            .Bytes);
    parseGoExceptions(T.Img);
    const auto &Module = T.Img.ExceptionMetadata.GoModule;
    ASSERT_TRUE(Module.has_value());
    EXPECT_EQ(Module->StackArguments, Kind == Prologue::ThreadLocal);
    if (Kind == Prologue::ThreadLocal)
      EXPECT_EQ(Module->NoReturnEntries, std::vector<va_t>{kTextVA + 0x100});
    else
      EXPECT_TRUE(Module->NoReturnEntries.empty());
  }
}

TEST(GoNoReturn, EveryLookupKnowsTheirEntries) {
  GoTestImage T;
  GoModuleInfo Module;
  Module.RegisterArguments = true;
  Module.NoReturnEntries = {kTextVA + 0x100};
  T.Img.ExceptionMetadata.GoModule = std::move(Module);
  EXPECT_TRUE(libc::isNoReturnTarget(T.Img, kTextVA + 0x100));
  EXPECT_FALSE(libc::isNoReturnTarget(T.Img, kTextVA + 0x200));
  const libc::NoReturnTargetIndex Index(T.Img);
  EXPECT_TRUE(Index.contains(T.Img, kTextVA + 0x100));
  EXPECT_FALSE(Index.contains(T.Img, kTextVA + 0x200));
}

TEST(GoCallbacks, TheLoaderNamesTheRoutineThatReturnsPastItsCaller) {
  // runtime.callbackasm1 pops the return address each Windows callback
  // entry's CALL leaves, and returns to that entry's caller
  // (src/runtime/sys_windows_amd64.s).
  GoTestImage T;
  T.installPclnTab(
      buildPclnTab(kGo116Magic,
                   {makeGoCode("runtime.callbackasm1", kTextVA + 0x100),
                    makeGoCode("main.f", kTextVA + 0x200)},
                   kTextVA + 0x300)
          .Bytes);
  parseGoExceptions(T.Img);
  const auto &Module = T.Img.ExceptionMetadata.GoModule;
  ASSERT_TRUE(Module.has_value());
  EXPECT_EQ(Module->ReturnsPastCallerEntries,
            std::vector<va_t>{kTextVA + 0x100});
}

TEST(GoStackArguments, ThreadLocalProloguesProveTheStackABI) {
  // Go 1.16 amd64: main.f's prologue loads g from thread-local storage,
  // `mov rcx, fs:[-8]; cmp rsp, [rcx+0x10]`, and every function takes its
  // arguments on the stack.  Go 1.17, under the same pclntab, compares the
  // stack with g in R14 (`cmp rsp, [r14+0x10]`) and passes registers.
  const std::vector<uint8_t> ThreadLocal = {0x64, 0x48, 0x8B, 0x0C, 0x25, 0xF8,
                                            0xFF, 0xFF, 0xFF, 0x48, 0x3B, 0x61,
                                            0x10, 0x76, 0x20, 0xC3};
  const std::vector<uint8_t> Register = {0x49, 0x3B, 0x66, 0x10, 0x76, 0x20,
                                         0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
                                         0xCC, 0xCC, 0xCC, 0xCC};
  for (const bool Go117 : {false, true}) {
    SCOPED_TRACE(Go117);
    GoTestImage T;
    GoFuncSpec F = makeGoCode("main.f", kTextVA + 0x100);
    F.Args = 24;
    T.writeText(kTextVA + 0x100, ThreadLocal);
    if (Go117)
      T.writeText(kTextVA + 0x200, Register);
    T.installPclnTab(buildPclnTab(kGo116Magic,
                                  {F, makeGoCode("main.g", kTextVA + 0x200)},
                                  kTextVA + 0x300)
                         .Bytes);
    parseGoExceptions(T.Img);
    const auto &Module = T.Img.ExceptionMetadata.GoModule;
    ASSERT_TRUE(Module.has_value());
    EXPECT_EQ(Module->StackArguments, !Go117);
    ASSERT_EQ(Module->FrameArguments.size(), 2u);
    EXPECT_EQ(Module->FrameArguments[0].Entry, kTextVA + 0x100);
    EXPECT_EQ(Module->FrameArguments[0].Bytes, 24u);
  }
  // Nothing proves it without a prologue that loads g.
  GoTestImage Bare;
  Bare.installPclnTab(buildPclnTab(kGo116Magic,
                                   {makeGoCode("main.f", kTextVA + 0x100)},
                                   kTextVA + 0x200)
                          .Bytes);
  parseGoExceptions(Bare.Img);
  ASSERT_TRUE(Bare.Img.ExceptionMetadata.GoModule.has_value());
  EXPECT_FALSE(Bare.Img.ExceptionMetadata.GoModule->StackArguments);
}

TEST(GoStackArguments, AnARM64ImageBeforeGo118PassesThemOnTheStack) {
  // arm64 gained the register ABI with the Go 1.18 pclntab.
  GoTestImage T(Arch::AArch64);
  T.installPclnTab(buildPclnTab(kGo116Magic,
                                {makeGoCode("main.f", kTextVA + 0x100)},
                                kTextVA + 0x200)
                       .Bytes);
  parseGoExceptions(T.Img);
  ASSERT_TRUE(T.Img.ExceptionMetadata.GoModule.has_value());
  EXPECT_TRUE(T.Img.ExceptionMetadata.GoModule->StackArguments);
}

TEST(GoPanicBounds, TheLoaderReadsEveryCallsEntry) {
  // main.f's PCDATA_PanicBounds table gives its first five bytes, a call to
  // runtime.panicBounds, the value abi.BoundsEncode makes of a signed index
  // in CX against DX.
  GoTestImage T;
  GoFuncSpec F = makeGoCode("main.f", kTextVA + 0x100);
  F.PCData = {std::nullopt, std::nullopt, std::nullopt, std::nullopt,
              PCValueTable{{{2439, 5}}}};
  T.installPclnTab(
      buildPclnTab(kGo116Magic,
                   {F, makeGoCode("runtime.panicBounds", kTextVA + 0x200)},
                   kTextVA + 0x300)
          .Bytes);
  parseGoExceptions(T.Img);
  const auto &Module = T.Img.ExceptionMetadata.GoModule;
  ASSERT_TRUE(Module.has_value());
  EXPECT_EQ(Module->PanicBoundsEntry, kTextVA + 0x200);
  ASSERT_EQ(Module->PanicBounds.size(), 1u);
  EXPECT_EQ(Module->PanicBounds[0].Range.Begin, kTextVA + 0x100);
  EXPECT_EQ(Module->PanicBounds[0].Range.End, kTextVA + 0x105);
  EXPECT_EQ(Module->PanicBounds[0].Value, 2439);
}

TEST(GoWriteBarriers, ARM64TakesItsOwnRegisters) {
  GoTestImage T(Arch::AArch64);
  T.installPclnTab(
      buildPclnTab(kGo116Magic,
                   {makeGoCode("runtime.gcWriteBarrier", kTextVA + 0x100),
                    makeGoCode("runtime.gcWriteBarrierCX", kTextVA + 0x200),
                    makeGoCode("runtime.gcWriteBarrier8", kTextVA + 0x300)},
                   kTextVA + 0x400)
          .Bytes);
  parseGoExceptions(T.Img);

  const auto &Module = T.Img.ExceptionMetadata.GoModule;
  ASSERT_TRUE(Module.has_value());
  // arm64 never had the register variants amd64 has.
  ASSERT_EQ(Module->WriteBarriers.size(), 2u);
  EXPECT_EQ(Module->WriteBarriers[0].Destination, a64reg::X2);
  EXPECT_EQ(Module->WriteBarriers[0].Value, a64reg::X3);
  EXPECT_EQ(Module->WriteBarriers[1].Kind, GoWriteBarrierKind::Buffer);
}

TEST(GoRegisterArguments, NeedAPclntabThatMarksAssembly) {
  // Go 1.16-1.17 records no assembly flag, so no function of such an image
  // is taken to use the internal register ABI; its stack-growth routines
  // are still found by name.
  GoTestImage T;
  T.installPclnTab(
      buildPclnTab(kGo116Magic,
                   {makeGoCode("main.a", kTextVA + 0x100),
                    makeGoCode("runtime.morestack_noctxt", kTextVA + 0x200),
                    makeGoCode("runtime.morestack", kTextVA + 0x300)},
                   kTextVA + 0x400)
          .Bytes);
  parseGoExceptions(T.Img);

  const auto &Module = T.Img.ExceptionMetadata.GoModule;
  ASSERT_TRUE(Module.has_value());
  EXPECT_FALSE(Module->RegisterArguments);
  EXPECT_TRUE(Module->GoAssembly.empty());
  EXPECT_EQ(Module->MorestackEntries,
            (std::vector<va_t>{kTextVA + 0x200, kTextVA + 0x300}));
}

TEST(GoDuffDevices, AreFoundByName) {
  // Go's compiler enters `runtime.duffzero` and `runtime.duffcopy` at the
  // point that zeroes or copies as many bytes as it needs.
  GoTestImage T;
  T.installPclnTab(
      buildPclnTab(kGo116Magic,
                   {makeGoCode("runtime.duffzero", kTextVA + 0x100),
                    makeGoCode("runtime.duffcopy", kTextVA + 0x200),
                    makeGoCode("main.a", kTextVA + 0x300)},
                   kTextVA + 0x400)
          .Bytes);
  parseGoExceptions(T.Img);

  const auto &Module = T.Img.ExceptionMetadata.GoModule;
  ASSERT_TRUE(Module.has_value());
  ASSERT_EQ(Module->DuffDevices.size(), 2u);
  EXPECT_EQ(Module->DuffDevices[0].Begin, kTextVA + 0x100);
  EXPECT_EQ(Module->DuffDevices[0].End, kTextVA + 0x200);
  EXPECT_EQ(Module->DuffDevices[1].Begin, kTextVA + 0x200);
  EXPECT_EQ(Module->DuffDevices[1].End, kTextVA + 0x300);
}

TEST(GoRegisterArgumentTransfers, AreFoundByName) {
  // Reflection's call helpers load a call's arguments with
  // `runtime.unspillArgs` and store its results with `runtime.spillArgs`.
  GoTestImage T;
  T.installPclnTab(
      buildPclnTab(kGo116Magic,
                   {makeGoCode("runtime.unspillArgs", kTextVA + 0x100),
                    makeGoCode("runtime.spillArgs", kTextVA + 0x180),
                    makeGoCode("main.a", kTextVA + 0x300)},
                   kTextVA + 0x400)
          .Bytes);
  parseGoExceptions(T.Img);

  const auto &Module = T.Img.ExceptionMetadata.GoModule;
  ASSERT_TRUE(Module.has_value());
  ASSERT_EQ(Module->RegisterArgumentTransfers.size(), 2u);
  EXPECT_EQ(Module->RegisterArgumentTransfers[0].Begin, kTextVA + 0x100);
  EXPECT_EQ(Module->RegisterArgumentTransfers[0].End, kTextVA + 0x180);
  EXPECT_EQ(Module->RegisterArgumentTransfers[1].Begin, kTextVA + 0x180);
  EXPECT_EQ(Module->RegisterArgumentTransfers[1].End, kTextVA + 0x300);
  EXPECT_TRUE(Module->DuffDevices.empty());
}

} // namespace
