//===- ExceptionCFGSeedTests.cpp - Handlers as CFG roots ------------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// A handler, filter, or landing pad is entered by the unwinder or by a
/// language runtime, never by a branch the function itself contains.  Recursive
/// descent therefore cannot reach one, and a body that is never decoded is a
/// body the lift silently drops.  These tests pin the addresses each table
/// model contributes as extra decode roots, and pin the cases that must *not*
/// be read as addresses.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/ExceptionInfo.h"

#include <vector>

namespace {

using namespace neverd;

constexpr va_t kBase = 0x140000000;
constexpr va_t kEntry = kBase + 0x1000;

/// A function whose entry returns immediately, followed by short `nop; ret`
/// bodies at two-byte spacing.  Nothing falls through into them and nothing
/// branches to them, so any block past the entry can only come from a table.
BinaryImage makeImage(Arch TheArch, BinaryFormat Format) {
  BinaryImage Img;
  Img.Arch = TheArch;
  Img.Bits = TheArch == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Img.Format = Format;
  Img.Base = kBase;

  Segment Text;
  Text.Name = ".text";
  Text.VA = kEntry;
  Text.Size = 0x10;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Text.Size, 0x90);
  Text.Data[0] = 0xc3; // entry: ret
  for (size_t Body = 1; Body + 1 < Text.Size; Body += 2)
    Text.Data[Body + 1] = 0xc3; // nop; ret
  Img.Segments.push_back(std::move(Text));
  return Img;
}

/// A record covering the whole of the text above, so that every address these
/// tests hand to a table falls inside the owning range.
ExceptionFunction makeRecord(ExceptionEncoding Encoding,
                             ExceptionPersonality Personality) {
  ExceptionFunction EH;
  EH.CodeRange = {kEntry, kEntry + 0x10};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.Encoding = Encoding;
  EH.Personality = Personality;
  return EH;
}

LowFunc buildWith(BinaryImage &Img, ExceptionFunction EH, Arch TheArch) {
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  Decoder Dec;
  EXPECT_TRUE(Dec.init(TheArch));
  CFGBuilder Builder;
  return Builder.build(Img, Dec, kEntry, "seeded");
}

std::vector<va_t> blockStarts(const LowFunc &Func) {
  std::vector<va_t> Starts;
  Starts.reserve(Func.Blocks.size());
  for (const LowBlock &Block : Func.Blocks)
    Starts.push_back(Block.StartAddr);
  return Starts;
}

//===----------------------------------------------------------------------===//
// Itanium LSDA
//===----------------------------------------------------------------------===//

TEST(ExceptionCFGSeed, DecodesAnItaniumLandingPadNoBranchReaches) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::DwarfFDE,
                                    ExceptionPersonality::GxxPersonalityV0);
  ItaniumEHInfo Itanium;
  ItaniumCallSite Site;
  Site.GuardedRange = {kEntry, kEntry + 1};
  Site.LandingPadVA = kEntry + 1;
  Itanium.CallSites.push_back(Site);
  EH.Itanium = std::move(Itanium);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  EXPECT_EQ(blockStarts(Func), (std::vector<va_t>{kEntry, kEntry + 1}));
}

TEST(ExceptionCFGSeed, SkipsAnItaniumCallSiteThatNamesNoLandingPad) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::DwarfFDE,
                                    ExceptionPersonality::GxxPersonalityV0);
  ItaniumEHInfo Itanium;
  ItaniumCallSite Site;
  Site.GuardedRange = {kEntry, kEntry + 1};
  Site.LandingPadVA = 0; // the ABI's spelling of "this call may not unwind"
  Itanium.CallSites.push_back(Site);
  EH.Itanium = std::move(Itanium);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  EXPECT_EQ(blockStarts(Func), (std::vector<va_t>{kEntry}));
}

TEST(ExceptionCFGSeed, RefusesToReadAnSJLJCallSiteTableAsAddresses) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::DwarfFDE,
                                    ExceptionPersonality::GxxPersonalitySJ0);
  ItaniumEHInfo Itanium;
  // An SJLJ table's fields are call-site indices.  Read as addresses they would
  // land on whatever byte the index happens to name.
  Itanium.IsCallSiteAddressForm = false;
  ItaniumCallSite Site;
  Site.GuardedRange = {kEntry, kEntry + 1};
  Site.LandingPadVA = kEntry + 1;
  Itanium.CallSites.push_back(Site);
  EH.Itanium = std::move(Itanium);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  EXPECT_EQ(blockStarts(Func), (std::vector<va_t>{kEntry}));
}

TEST(ExceptionCFGSeed, LeavesALandingPadOutsideTheOwningRangeToItsOwnRecord) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::DwarfFDE,
                                    ExceptionPersonality::GxxPersonalityV0);
  EH.CodeRange = {kEntry, kEntry + 2};
  ItaniumEHInfo Itanium;
  ItaniumCallSite Site;
  Site.GuardedRange = {kEntry, kEntry + 1};
  Site.LandingPadVA = kEntry + 5; // past the end of this record
  Itanium.CallSites.push_back(Site);
  EH.Itanium = std::move(Itanium);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  EXPECT_EQ(blockStarts(Func), (std::vector<va_t>{kEntry}));
}

//===----------------------------------------------------------------------===//
// Rust
//===----------------------------------------------------------------------===//

TEST(ExceptionCFGSeed, DecodesRustDropGlueAndCatchUnwindPads) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::DwarfFDE,
                                    ExceptionPersonality::RustEhPersonality);
  RustFunctionEH Rust;
  RustLandingPad Drop;
  Drop.GuardedRange = {kEntry, kEntry + 1};
  Drop.PadVA = kEntry + 1;
  Drop.Kind = RustLandingPadKind::DropGlue;
  Rust.LandingPads.push_back(Drop);
  RustLandingPad Catch;
  Catch.GuardedRange = {kEntry, kEntry + 1};
  Catch.PadVA = kEntry + 3;
  Catch.Kind = RustLandingPadKind::CatchUnwind;
  Rust.LandingPads.push_back(Catch);
  EH.Rust = std::move(Rust);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  EXPECT_EQ(blockStarts(Func),
            (std::vector<va_t>{kEntry, kEntry + 1, kEntry + 3}));
}

//===----------------------------------------------------------------------===//
// x86-32 registration chain
//===----------------------------------------------------------------------===//

TEST(ExceptionCFGSeed, DecodesAnExceptFilterAndItsHandlerBody) {
  BinaryImage Img = makeImage(Arch::X86, BinaryFormat::COFF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::X86ScopeTableEH3,
                                    ExceptionPersonality::ExceptHandler3);
  RegistrationChainInfo Chain;
  RegistrationScopeRecord Scope;
  Scope.FilterVA = kEntry + 1;
  Scope.HandlerVA = kEntry + 3;
  Chain.Scopes.push_back(Scope);
  EH.Registration = std::move(Chain);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X86);
  EXPECT_EQ(blockStarts(Func),
            (std::vector<va_t>{kEntry, kEntry + 1, kEntry + 3}));
}

TEST(ExceptionCFGSeed, DecodesAFinallyBodyThatDeclaresNoFilter) {
  BinaryImage Img = makeImage(Arch::X86, BinaryFormat::COFF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::X86ScopeTableEH4,
                                    ExceptionPersonality::ExceptHandler4);
  RegistrationChainInfo Chain;
  RegistrationScopeRecord Scope;
  Scope.FilterVA = 0; // zero filter is how a __finally is spelled
  Scope.HandlerVA = kEntry + 5;
  Scope.IsFinally = true;
  Chain.Scopes.push_back(Scope);
  EH.Registration = std::move(Chain);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X86);
  EXPECT_EQ(blockStarts(Func), (std::vector<va_t>{kEntry, kEntry + 5}));
}

//===----------------------------------------------------------------------===//
// Delphi
//===----------------------------------------------------------------------===//

TEST(ExceptionCFGSeed, DecodesDelphiExceptAndFinallyBodies) {
  BinaryImage Img = makeImage(Arch::X86, BinaryFormat::COFF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::DelphiX86Chain,
                                    ExceptionPersonality::DelphiX86Handler);
  DelphiFrameInfo Delphi;
  Delphi.Kind = DelphiHandlerKind::Finally;
  Delphi.FinallyBodyVA = kEntry + 1;
  Delphi.ExceptBodyVA = kEntry + 3;
  EH.Delphi = std::move(Delphi);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X86);
  EXPECT_EQ(blockStarts(Func),
            (std::vector<va_t>{kEntry, kEntry + 1, kEntry + 3}));
}

TEST(ExceptionCFGSeed, DecodesEveryDelphiOnExceptionArm) {
  BinaryImage Img = makeImage(Arch::X86, BinaryFormat::COFF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::DelphiX86Chain,
                                    ExceptionPersonality::DelphiX86Handler);
  DelphiFrameInfo Delphi;
  Delphi.Kind = DelphiHandlerKind::OnException;
  DelphiOnExceptionEntry First;
  First.ClassVA = 0x140004000;
  First.HandlerVA = kEntry + 1;
  Delphi.OnExceptions.push_back(First);
  DelphiOnExceptionEntry Else;
  Else.IsCatchAll = true;
  Else.HandlerVA = kEntry + 5;
  Delphi.OnExceptions.push_back(Else);
  EH.Delphi = std::move(Delphi);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X86);
  EXPECT_EQ(blockStarts(Func),
            (std::vector<va_t>{kEntry, kEntry + 1, kEntry + 5}));
}

//===----------------------------------------------------------------------===//
// Go
//===----------------------------------------------------------------------===//

TEST(ExceptionCFGSeed, DecodesTheGoDeferReturnResumptionPoint) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::GoFuncTable,
                                    ExceptionPersonality::None);
  GoFunctionEH Go;
  Go.EntryVA = kEntry;
  Go.DeferReturnOffset = 3;
  EH.Go = std::move(Go);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  EXPECT_EQ(blockStarts(Func), (std::vector<va_t>{kEntry, kEntry + 3}));
}

TEST(ExceptionCFGSeed, AddsNoRootForAGoFunctionThatDefersNothing) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::GoFuncTable,
                                    ExceptionPersonality::None);
  GoFunctionEH Go;
  Go.EntryVA = kEntry;
  EH.Go = std::move(Go);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  EXPECT_EQ(blockStarts(Func), (std::vector<va_t>{kEntry}));
}

//===----------------------------------------------------------------------===//
// Exceptional edges
//
// Reaching a handler's body is only half of what a table says.  The other half
// is which blocks can transfer there, which is what these pin.  Edges are kept
// out of `Succs` on purpose, so an analysis that ignores them still sees the
// ordinary control flow it expects.
//===----------------------------------------------------------------------===//

/// Every (kind, target) an edge list names, for a block, in encounter order.
std::vector<std::pair<ExceptionalEdgeKind, va_t>>
edgesFrom(const LowFunc &Func, va_t BlockStart) {
  std::vector<std::pair<ExceptionalEdgeKind, va_t>> Edges;
  for (const LowBlock &Block : Func.Blocks) {
    if (Block.StartAddr != BlockStart)
      continue;
    for (const ExceptionalEdge &Edge : Block.ExceptionalSuccs)
      Edges.emplace_back(Edge.Kind, Edge.TargetVA);
  }
  return Edges;
}

TEST(ExceptionCFGEdge,
     NormalizesLegacyAArch64SEHEndBeforeSplittingProtectedBlocks) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = kBase;

  // Four protected NOPs, one unprotected RET, and a separately rooted handler
  // RET.  The raw End at entry+0xd covers the NOP at entry+0xc and is
  // semantically the aligned boundary at entry+0x10.
  Segment Text;
  Text.Name = ".text";
  Text.VA = kEntry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {
      0x1f, 0x20, 0x03, 0xd5, 0x1f, 0x20, 0x03, 0xd5,
      0x1f, 0x20, 0x03, 0xd5, 0x1f, 0x20, 0x03, 0xd5,
      0xc0, 0x03, 0x5f, 0xd6, 0xc0, 0x03, 0x5f, 0xd6,
  };
  Text.Size = Text.Data.size();
  Img.Segments.push_back(std::move(Text));

  ExceptionFunction EH =
      makeRecord(ExceptionEncoding::ARM64Unpacked,
                 ExceptionPersonality::CSpecificHandler);
  EH.CodeRange = {kEntry, kEntry + 0x18};
  EH.PersonalityVA = kBase + 0x3000;
  EH.SEH.emplace();
  SEHScopeRecord Scope;
  Scope.GuardedRange = {kEntry, kEntry + 0x0d};
  Scope.Kind = SEHScopeKind::CatchAll;
  Scope.HandlerVA = kEntry + 0x14;
  Scope.ContinuationVA = Scope.HandlerVA;
  EH.SEH->Scopes.push_back(Scope);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::AArch64);
  EXPECT_EQ(blockStarts(Func),
            (std::vector<va_t>{kEntry, kEntry + 0x10, kEntry + 0x14}));
  EXPECT_EQ(edgesFrom(Func, kEntry),
            (std::vector<std::pair<ExceptionalEdgeKind, va_t>>{
                {ExceptionalEdgeKind::SEHHandler, kEntry + 0x14}}));
  EXPECT_TRUE(edgesFrom(Func, kEntry + 0x10).empty());
}

TEST(ExceptionCFGEdge, ReadsACallSiteWithNoActionAsAnUnconditionalCleanup) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::DwarfFDE,
                                    ExceptionPersonality::GxxPersonalityV0);
  ItaniumEHInfo Itanium;
  ItaniumCallSite Site;
  Site.GuardedRange = {kEntry, kEntry + 1};
  Site.LandingPadVA = kEntry + 1;
  Itanium.CallSites.push_back(Site);
  EH.Itanium = std::move(Itanium);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  EXPECT_EQ(edgesFrom(Func, kEntry),
            (std::vector<std::pair<ExceptionalEdgeKind, va_t>>{
                {ExceptionalEdgeKind::ItaniumCleanupPad, kEntry + 1}}));
}

TEST(ExceptionCFGEdge, TellsItaniumCatchCleanupAndSpecActionsApart) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::DwarfFDE,
                                    ExceptionPersonality::GxxPersonalityV0);
  ItaniumEHInfo Itanium;
  // A chain of three: catch type 1, then an exception specification, then the
  // cleanup that always runs.
  ItaniumAction Catch;
  Catch.TableOffset = 0;
  Catch.TypeFilter = 1;
  Catch.NextActionOffset = 8;
  ItaniumAction Spec;
  Spec.TableOffset = 8;
  Spec.TypeFilter = -1;
  Spec.NextActionOffset = 16;
  ItaniumAction Cleanup;
  Cleanup.TableOffset = 16;
  Cleanup.TypeFilter = 0;
  Itanium.Actions = {Catch, Spec, Cleanup};

  ItaniumCallSite Site;
  Site.GuardedRange = {kEntry, kEntry + 1};
  Site.LandingPadVA = kEntry + 1;
  Site.FirstActionOffset = 0;
  Itanium.CallSites.push_back(Site);
  EH.Itanium = std::move(Itanium);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  EXPECT_EQ(edgesFrom(Func, kEntry),
            (std::vector<std::pair<ExceptionalEdgeKind, va_t>>{
                {ExceptionalEdgeKind::ItaniumCatchPad, kEntry + 1},
                {ExceptionalEdgeKind::ItaniumSpecPad, kEntry + 1},
                {ExceptionalEdgeKind::ItaniumCleanupPad, kEntry + 1}}));
}

TEST(ExceptionCFGEdge, TerminatesOnAnActionChainThatPointsAtItself) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::DwarfFDE,
                                    ExceptionPersonality::GxxPersonalityV0);
  ItaniumEHInfo Itanium;
  ItaniumAction Loop;
  Loop.TableOffset = 0;
  Loop.TypeFilter = 1;
  Loop.NextActionOffset = 0;
  Itanium.Actions = {Loop};

  ItaniumCallSite Site;
  Site.GuardedRange = {kEntry, kEntry + 1};
  Site.LandingPadVA = kEntry + 1;
  Site.FirstActionOffset = 0;
  Itanium.CallSites.push_back(Site);
  EH.Itanium = std::move(Itanium);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  // A cycle names one clause however many times it is walked.
  EXPECT_EQ(edgesFrom(Func, kEntry),
            (std::vector<std::pair<ExceptionalEdgeKind, va_t>>{
                {ExceptionalEdgeKind::ItaniumCatchPad, kEntry + 1}}));
}

TEST(ExceptionCFGEdge, LeavesACallSiteWithNoLandingPadUnlinked) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::DwarfFDE,
                                    ExceptionPersonality::GxxPersonalityV0);
  ItaniumEHInfo Itanium;
  ItaniumCallSite Site;
  Site.GuardedRange = {kEntry, kEntry + 1};
  Itanium.CallSites.push_back(Site);
  EH.Itanium = std::move(Itanium);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  EXPECT_TRUE(edgesFrom(Func, kEntry).empty());
}

TEST(ExceptionCFGEdge, LinksEachDelphiX64ScopeOverTheRangeItGuards) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::COFF);
  ExceptionFunction EH = makeRecord(
      ExceptionEncoding::X64UnwindV1, ExceptionPersonality::DelphiExceptionHandler);
  DelphiScopeTable Table;
  DelphiScopeRecord Cleanup;
  Cleanup.GuardedRange = {kEntry, kEntry + 1};
  Cleanup.Kind = DelphiScopeKind::Finally;
  Cleanup.TargetVA = kEntry + 1;
  Table.Scopes.push_back(std::move(Cleanup));
  // Guarding the cleanup the first scope names is what puts this scope over a
  // block that exists: nothing branches into the middle of the text, so a
  // range no root reaches would select nothing and prove nothing.
  DelphiScopeRecord Arms;
  Arms.GuardedRange = {kEntry + 1, kEntry + 3};
  Arms.Kind = DelphiScopeKind::OnException;
  DelphiOnExceptionEntry Arm;
  Arm.HandlerVA = kEntry + 7;
  Arms.OnExceptions.push_back(Arm);
  Table.Scopes.push_back(std::move(Arms));
  EH.DelphiScopes = std::move(Table);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  // A scope's range is what selects the blocks it applies to, so neither edge
  // reaches the other scope's block.
  EXPECT_EQ(edgesFrom(Func, kEntry),
            (std::vector<std::pair<ExceptionalEdgeKind, va_t>>{
                {ExceptionalEdgeKind::DelphiFinally, kEntry + 1}}));
  EXPECT_EQ(edgesFrom(Func, kEntry + 1),
            (std::vector<std::pair<ExceptionalEdgeKind, va_t>>{
                {ExceptionalEdgeKind::DelphiOnException, kEntry + 7}}));
}

TEST(ExceptionCFGEdge, LinksGoPanicAndDeferSitesToDeferReturn) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::GoFuncTable,
                                    ExceptionPersonality::None);
  GoFunctionEH Go;
  Go.EntryVA = kEntry;
  Go.DeferReturnOffset = 3;
  GoDeferSite Defer;
  Defer.CallVA = kEntry;
  Go.Defers.push_back(Defer);
  GoPanicSite Panic;
  Panic.CallVA = kEntry;
  Panic.RuntimeName = "runtime.gopanic";
  Go.Panics.push_back(Panic);
  EH.Go = std::move(Go);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  // Both sites sit in the entry block and name the same resumption point, so
  // they describe one edge rather than two.
  EXPECT_EQ(edgesFrom(Func, kEntry),
            (std::vector<std::pair<ExceptionalEdgeKind, va_t>>{
                {ExceptionalEdgeKind::GoDeferReturn, kEntry + 3}}));
}

TEST(ExceptionCFGEdge, KeepsExceptionalEdgesOutOfOrdinarySuccessors) {
  BinaryImage Img = makeImage(Arch::X64, BinaryFormat::ELF);
  ExceptionFunction EH = makeRecord(ExceptionEncoding::DwarfFDE,
                                    ExceptionPersonality::GxxPersonalityV0);
  ItaniumEHInfo Itanium;
  ItaniumCallSite Site;
  Site.GuardedRange = {kEntry, kEntry + 1};
  Site.LandingPadVA = kEntry + 1;
  Itanium.CallSites.push_back(Site);
  EH.Itanium = std::move(Itanium);

  const LowFunc Func = buildWith(Img, std::move(EH), Arch::X64);
  for (const LowBlock &Block : Func.Blocks)
    if (Block.StartAddr == kEntry) {
      EXPECT_TRUE(Block.Succs.empty());
      EXPECT_EQ(Block.ExceptionalSuccs.size(), 1u);
    }
}

} // namespace
