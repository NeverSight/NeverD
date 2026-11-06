//===- ResumableTrapBoundaryTests.cpp - Debugger trap boundaries ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Check function ownership at resumable traps without suppressing real
/// debugger continuations or ordinary transfers into another function.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/LowIntrinsicEffects.h"
#include "neverd/ir/low/LowNoReturn.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExecutableCodeOwnerIndex.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/ProloguePatterns.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <set>
#include <string>
#include <vector>

using namespace neverd;

namespace {

constexpr va_t Entry = 0x1000;
constexpr va_t Next = Entry + 6;

BinaryImage image(Arch Architecture = Arch::X64,
                  BinaryFormat Format = BinaryFormat::COFF) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Architecture == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
  Image.Format = Format;
  Image.Base = Entry;
  Image.Entry = Entry;
  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = {0xb8, 7,  0, 0, 0, // mov eax, 7
               0xcc,              // int3
               0xb8, 42, 0, 0, 0, // mov eax, 42
               0xc3};             // ret
  Text.Size = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  return Image;
}

void addSymbol(BinaryImage &Image, va_t Address, uint64_t Size,
               const char *Name) {
  auto Sym = Symbol::makeFunc(Address, Size);
  Sym.Name = Name;
  Image.Symbols.push_back(std::move(Sym));
}

LowFunc build(const BinaryImage &Image, const std::set<va_t> &Entries = {}) {
  Decoder Dec;
  if (!Dec.init(Image)) {
    ADD_FAILURE() << "decoder initialization failed";
    return {};
  }
  CFGBuilder Builder;
  Builder.setKnownFuncEntries(&Entries);
  return Builder.build(Image, Dec, Entry, "trap_owner");
}

void expectStoppedAtTrap(const LowFunc &Function,
                         Arch Architecture = Arch::X64) {
  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  EXPECT_FALSE(lowFunctionNeverReturns(Function, Architecture));
  auto Error = validateLowInstructionBoundaries(
      Function, LowInstructionBoundaryRequirement::Required);
  EXPECT_FALSE(bool(Error)) << llvm::toString(std::move(Error));
  ASSERT_EQ(Function.Blocks.size(), 1u);
  const auto &Block = Function.Blocks.front();
  EXPECT_EQ(Block.EndAddr, Next);
  EXPECT_TRUE(Block.Succs.empty());
  ASSERT_EQ(Block.InstructionBoundaries.size(), 2u);
  const auto &Trap = Block.InstructionBoundaries.back();
  EXPECT_EQ(Trap.Address, Next - 1);
  EXPECT_EQ(Trap.Control, LowInstructionControl::Terminator);
  EXPECT_TRUE(hasLowInstructionControlFlag(
      Trap.ControlFlags, LowInstructionControlFlag::Resumable));
  for (const auto &Op : Block.Ops) {
    EXPECT_NE(Op.Opcode, NdOp::CALL);
    EXPECT_NE(Op.Opcode, NdOp::RETURN);
  }
}

TEST(ResumableTrapBoundary, SizedMetadataStopsBeforeDecodableUnownedBytes) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool SizedSymbol : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << static_cast<int>(Architecture) << "/"
                     << static_cast<int>(Format) << "/" << SizedSymbol);
        auto Image = image(Architecture, Format);
        if (SizedSymbol)
          addSymbol(Image, Entry, Next - Entry, "trap_owner");
        else
          Image.KnownCodeRanges.emplace_back(Entry, Next);
        expectStoppedAtTrap(build(Image), Architecture);
      }
}

TEST(ResumableTrapBoundary, AdjacentFunctionEntryDoesNotBecomeATailCall) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (unsigned Evidence = 0; Evidence != 4; ++Evidence) {
      SCOPED_TRACE(::testing::Message()
                   << static_cast<int>(Architecture) << "/" << Evidence);
      auto Image = image(Architecture);
      std::set<va_t> Entries;
      switch (Evidence) {
      case 0:
        Entries.insert(Next);
        break;
      case 1:
        Image.KnownCodeRanges.emplace_back(Next, Next + 6);
        break;
      case 2:
        addSymbol(Image, Next, 0, "neighbor");
        break;
      case 3:
        Image.RuntimeFunctionAddrs.insert(Next);
        break;
      }
      expectStoppedAtTrap(build(Image, Entries), Architecture);
    }
}

TEST(ResumableTrapBoundary, UnwindEndStopsWithoutAnAdjacentEntry) {
  auto Image = image();
  ExceptionFunction Owner;
  Owner.CodeRange = {Entry, Next};
  Owner.Kind = RuntimeFunctionKind::Primary;
  Owner.Encoding = ExceptionEncoding::X64UnwindV1;
  Owner.ParseStatus = ExceptionParseStatus::Complete;
  Image.ExceptionMetadata.Functions.push_back(std::move(Owner));
  Image.ExceptionMetadata.rebuildIndex();
  expectStoppedAtTrap(build(Image));
}

TEST(ResumableTrapBoundary, InteriorDebugBreakRetainsItsContinuation) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (bool Bounded : {false, true}) {
      auto Image = image(Architecture);
      if (Bounded)
        Image.KnownCodeRanges.emplace_back(Entry, Next + 6);
      const auto Function = build(Image);
      EXPECT_TRUE(Function.hasCompleteLiftCoverage());
      EXPECT_EQ(Function.DecodedInstructionCount, 4u);
      ASSERT_EQ(Function.Blocks.size(), 1u);
      EXPECT_EQ(Function.Blocks.front().EndAddr, Next + 6);
      EXPECT_EQ(Function.Blocks.front().InstructionBoundaries.back().Control,
                LowInstructionControl::Return);
    }
}

TEST(ResumableTrapBoundary, AdjacentOwnedFragmentRetainsItsContinuation) {
  auto Image = image();
  Image.KnownCodeRanges = {{Entry, Next}, {Next, Next + 6}};
  ExceptionFunction Owner;
  Owner.CodeRange = {Entry, Next};
  Owner.Kind = RuntimeFunctionKind::Primary;
  Owner.Encoding = ExceptionEncoding::X64UnwindV1;
  Owner.ParseStatus = ExceptionParseStatus::Complete;
  Owner.FragmentRanges.push_back({Next, Next + 6});
  Image.ExceptionMetadata.Functions.push_back(std::move(Owner));
  Image.ExceptionMetadata.rebuildIndex();
  const auto Function = build(Image, {Entry, Next});
  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  EXPECT_EQ(Function.DecodedInstructionCount, 4u);
  ASSERT_EQ(Function.Blocks.size(), 1u);
  EXPECT_EQ(Function.Blocks.front().EndAddr, Next + 6);
}

TEST(ResumableTrapBoundary, OrdinaryFallthroughStillCallsAdjacentFunction) {
  auto Image = image();
  Image.Segments.front().Data[5] = 0x90; // nop instead of int3
  Image.KnownCodeRanges = {{Entry, Next}, {Next, Next + 6}};
  const auto Function = build(Image);
  unsigned Calls = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      if (Boundary.Control == LowInstructionControl::TailCall) {
        EXPECT_EQ(Boundary.Immediate, Next);
        ++Calls;
      }
  EXPECT_EQ(Calls, 1u);
}

TEST(ResumableTrapBoundary, ExplicitTailJumpAfterDebugBreakIsPreserved) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    auto Image = image(Architecture);
    auto &Text = Image.Segments.front();
    Text.Data.resize(13, 0xcc);
    Text.Data[6] = 0xeb; // jmp Entry + 12
    Text.Data[7] = 4;
    Text.Data[12] = 0xc3;
    Text.Size = Text.Data.size();
    Image.KnownCodeRanges = {{Entry, Entry + 8}, {Entry + 12, Entry + 13}};
    const auto Function = build(Image);
    EXPECT_TRUE(Function.hasCompleteLiftCoverage());
    EXPECT_EQ(Function.DecodedInstructionCount, 3u);
    ASSERT_EQ(Function.Blocks.size(), 1u);
    const auto &Boundaries = Function.Blocks.front().InstructionBoundaries;
    ASSERT_EQ(Boundaries.size(), 3u);
    EXPECT_EQ(Boundaries[1].Control, LowInstructionControl::Terminator);
    EXPECT_EQ(Boundaries[2].Control, LowInstructionControl::TailCall);
    EXPECT_EQ(Boundaries[2].Immediate, Entry + 12);
  }
}

TEST(ResumableTrapBoundary, BothSourceRoutesKeepTrapWithoutCallingNeighbor) {
  for (bool LLVMRoute : {false, true})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(::testing::Message() << LLVMRoute << "/" << NoOpt);
      auto Image = image();
      Image.KnownCodeRanges = {{Entry, Next}, {Next, Next + 6}};
      addSymbol(Image, Entry, Next - Entry, "trap_owner");
      addSymbol(Image, Next, 6, "neighbor");
      // A returning call followed by int3 must keep both operations, without
      // adding a call to the independent function immediately after the trap.
      auto &Text = Image.Segments.front();
      Text.Data[0] = 0xe8; // call Entry + 0x10
      Text.Data[1] = 0x0b;
      Text.Data.resize(0x10, 0xcc);
      // mov dword ptr [rcx], 7; ret -- the call has an observable effect even
      // when the enclosing function does not consume its return register.
      Text.Data.insert(Text.Data.end(), {0xc7, 0x01, 7, 0, 0, 0, 0xc3});
      Text.Size = Text.Data.size();
      addSymbol(Image, Entry + 0x10, 7, "returning_helper");
      Image.KnownCodeRanges.emplace_back(Entry + 0x10, Entry + 0x17);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries.insert(Entry);
      Options.LiftMode = LLVMRoute;
      Options.SourceProjection = LLVMRoute;
      Options.NoOpt = NoOpt;
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      if (LLVMRoute) {
        ASSERT_TRUE(Result.LlvmModule);
        EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, COptions,
                                        nullptr, &Image));
      } else {
        ASSERT_FALSE(Result.HighFuncs.empty());
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COptions));
      }
      EXPECT_NE(Source.find("__debugbreak()"), std::string::npos) << Source;
      EXPECT_NE(Source.find("returning_helper("), std::string::npos) << Source;
      EXPECT_EQ(Source.find("neighbor("), std::string::npos) << Source;
    }
}

struct TrapCase {
  const char *Name;
  Arch Architecture;
  InstructionMode Mode;
  std::vector<uint8_t> Trap;
  std::vector<uint8_t> Continuation;
  bool Resumable;
};

std::vector<TrapCase> siblingTraps() {
  std::vector<TrapCase> Cases;
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    for (const auto &[Name, Bytes] :
         std::vector<std::pair<const char *, std::vector<uint8_t>>>{
             {"int3", {0xcc}},
             {"int 3", {0xcd, 3}},
             {"prefixed int 3", {0x66, 0xcd, 3}},
             {"int1", {0xf1}}})
      Cases.push_back({Name,
                       Architecture,
                       InstructionMode::Default,
                       Bytes,
                       {0xb8, 42, 0, 0, 0, 0xc3},
                       true});
    for (const auto &[Name, Bytes] :
         std::vector<std::pair<const char *, std::vector<uint8_t>>>{
             {"ud0", {0x0f, 0xff}},
             {"ud1", {0x0f, 0xb9, 0xc0}},
             {"ud2", {0x0f, 0x0b}},
             {"fastfail", {0xcd, 0x29}}})
      Cases.push_back({Name,
                       Architecture,
                       InstructionMode::Default,
                       Bytes,
                       {0xb8, 42, 0, 0, 0, 0xc3},
                       false});
  }
  Cases.push_back({"thumb bkpt",
                   Arch::ARM,
                   InstructionMode::Thumb,
                   {1, 0xbe},
                   {42, 0x20, 0x70, 0x47},
                   true});
  Cases.push_back({"thumb udf",
                   Arch::ARM,
                   InstructionMode::Thumb,
                   {0xfe, 0xde},
                   {42, 0x20, 0x70, 0x47},
                   true});
  Cases.push_back({"thumb udf division alias",
                   Arch::ARM,
                   InstructionMode::Thumb,
                   {0xf9, 0xde},
                   {42, 0x20, 0x70, 0x47},
                   true});
  Cases.push_back({"thumb hlt",
                   Arch::ARM,
                   InstructionMode::Thumb,
                   {0x87, 0xba},
                   {42, 0x20, 0x70, 0x47},
                   true});
  Cases.push_back({"arm bkpt",
                   Arch::ARM,
                   InstructionMode::ARM,
                   {0x71, 0, 0x20, 0xe1},
                   {42, 0, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1},
                   true});
  Cases.push_back({"arm udf",
                   Arch::ARM,
                   InstructionMode::ARM,
                   {0xfe, 0xde, 0xff, 0xe7},
                   {42, 0, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1},
                   true});
  Cases.push_back({"arm hlt",
                   Arch::ARM,
                   InstructionMode::ARM,
                   {0x77, 0, 0, 0xe1},
                   {42, 0, 0xa0, 0xe3, 0x1e, 0xff, 0x2f, 0xe1},
                   true});
  Cases.push_back({"a64 debugbreak",
                   Arch::AArch64,
                   InstructionMode::Default,
                   {0, 0, 0x3e, 0xd4},
                   {0x40, 0x05, 0x80, 0x52, 0xc0, 3, 0x5f, 0xd6},
                   true});
  for (const auto &[Name, Bytes] :
       std::vector<std::pair<const char *, std::vector<uint8_t>>>{
           {"a64 brk", {0x20, 0, 0x20, 0xd4}},
           {"a64 hlt", {0, 0, 0x40, 0xd4}},
           {"a64 udf", {0x34, 0x12, 0, 0}}})
    Cases.push_back({Name,
                     Arch::AArch64,
                     InstructionMode::Default,
                     Bytes,
                     {0x40, 0x05, 0x80, 0x52, 0xc0, 3, 0x5f, 0xd6},
                     false});
  return Cases;
}

BinaryImage trapImage(const TrapCase &Case) {
  auto Image = image(Case.Architecture);
  Image.Mode = Case.Mode;
  Image.Bits =
      Case.Architecture == Arch::X64 || Case.Architecture == Arch::AArch64
          ? Bitness::Bits64
          : Bitness::Bits32;
  auto &Text = Image.Segments.front();
  Text.Data = Case.Trap;
  Text.Data.insert(Text.Data.end(), Case.Continuation.begin(),
                   Case.Continuation.end());
  Text.Size = Text.Data.size();
  return Image;
}

std::vector<std::vector<uint8_t>> siblingNops(const TrapCase &Case) {
  if (Case.Architecture == Arch::AArch64)
    return {{0x1f, 0x20, 3, 0xd5}};
  if (Case.Architecture == Arch::ARM) {
    if (Case.Mode == InstructionMode::Thumb)
      return {{0, 0xbf}, {0xc0, 0x46}, {0xaf, 0xf3, 0, 0x80}};
    std::vector<std::vector<uint8_t>> Nops = {{0, 0, 0xa0, 0xe1},
                                              {0, 0xf0, 0x20, 0xe3}};
    for (uint8_t Condition = 0; Condition != 14; ++Condition) {
      Nops.push_back({0, 0, 0xa0, static_cast<uint8_t>((Condition << 4) | 1)});
      Nops.push_back(
          {0, 0xf0, 0x20, static_cast<uint8_t>((Condition << 4) | 3)});
    }
    return Nops;
  }
  if (Case.Architecture == Arch::X64)
    return {{0x90},
            {0x66, 0x90},
            {0x40, 0x90},
            {0x48, 0x90},
            {0x42, 0x90},
            {0x66, 0x48, 0x90},
            {0x67, 0x90},
            {0x2e, 0x48, 0x90},
            {0x48, 0x0f, 0x1f, 0},
            {0x41, 0x0f, 0x1f, 0xc0},
            {0x0f, 0x1a, 0xc0},
            {0x0f, 0x1b, 0xc0}};
  return {{0x90}, {0x66, 0x90}, {0x89, 0xf6}, {0x8d, 0x76, 0}};
}

TEST(ResumableTrapBoundary, SiblingNopPaddingDoesNotCreateATailCall) {
  for (const auto &Case : siblingTraps()) {
    if (!Case.Resumable)
      continue;
    for (const auto &Nop : siblingNops(Case))
      for (BinaryFormat Format :
           {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
        for (unsigned Ownership = 0; Ownership != 3; ++Ownership) {
          SCOPED_TRACE(::testing::Message()
                       << Case.Name << "/" << Nop.size() << "/"
                       << static_cast<int>(Format) << "/" << Ownership);
          auto Image = trapImage(Case);
          Image.Format = Format;
          auto &Text = Image.Segments.front();
          Text.Data.insert(Text.Data.begin() + Case.Trap.size(), Nop.begin(),
                           Nop.end());
          Text.Size = Text.Data.size();
          const va_t AfterNop = Entry + Case.Trap.size() + Nop.size();
          std::set<va_t> Entries;
          if (Ownership == 0)
            Entries.insert(AfterNop);
          else if (Ownership == 1)
            Image.KnownCodeRanges = {{Entry, Entry + Text.Size}};
          const auto Function = build(Image, Entries);
          ASSERT_TRUE(Function.hasCompleteLiftCoverage());
          ASSERT_EQ(Function.Blocks.size(), 1u);
          if (Ownership == 0) {
            EXPECT_EQ(Function.Blocks.front().EndAddr,
                      Entry + Case.Trap.size());
            EXPECT_EQ(Function.DecodedInstructionCount, 1u);
            for (const auto &Op : Function.Blocks.front().Ops)
              EXPECT_NE(Op.Opcode, NdOp::CALL);
          } else {
            EXPECT_EQ(Function.Blocks.front().EndAddr, Entry + Text.Size);
            EXPECT_EQ(Function.DecodedInstructionCount, 4u);
          }
        }
  }
}

TEST(ResumableTrapBoundary, OwnedNopSuffixDoesNotCreateATailCall) {
  for (const auto &Case : siblingTraps()) {
    if (!Case.Resumable)
      continue;
    for (const auto &Nop : siblingNops(Case))
      for (BinaryFormat Format :
           {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
        for (unsigned Ownership = 0; Ownership != 3; ++Ownership)
          for (unsigned Count : {1u, 65u})
            for (bool HasNeighbor : {false, true}) {
              SCOPED_TRACE(::testing::Message()
                           << Case.Name << "/" << Nop.size() << "/"
                           << static_cast<int>(Format) << "/" << Ownership
                           << "/" << Count << "/" << HasNeighbor);
              auto Image = trapImage(Case);
              Image.Format = Format;
              auto &Text = Image.Segments.front();
              Text.Data = Case.Trap;
              for (unsigned I = 0; I < Count; ++I)
                Text.Data.insert(Text.Data.end(), Nop.begin(), Nop.end());
              const va_t End = Entry + Text.Data.size();
              Text.Data.insert(Text.Data.end(), Case.Continuation.begin(),
                               Case.Continuation.end());
              Text.Size = Text.Data.size();
              if (Ownership == 0)
                Image.KnownCodeRanges = {{Entry, End}};
              else if (Ownership == 1)
                addSymbol(Image, Entry, End - Entry, "trap_owner");
              else {
                ExceptionFunction Owner;
                Owner.CodeRange = {Entry, Entry + Case.Trap.size()};
                Owner.FragmentRanges = {{Owner.CodeRange.End, End}};
                Owner.Kind = RuntimeFunctionKind::Primary;
                Owner.Encoding = ExceptionEncoding::DwarfFDE;
                Owner.ParseStatus = ExceptionParseStatus::Complete;
                Image.ExceptionMetadata.Functions.push_back(Owner);
                Image.ExceptionMetadata.rebuildIndex();
              }
              const auto Function = build(
                  Image, HasNeighbor ? std::set<va_t>{End} : std::set<va_t>{});
              ASSERT_TRUE(Function.hasCompleteLiftCoverage());
              ASSERT_EQ(Function.Blocks.size(), 1u);
              EXPECT_EQ(Function.DecodedInstructionCount, 1u);
              EXPECT_EQ(Function.Blocks.front().EndAddr,
                        Entry + Case.Trap.size());
              EXPECT_TRUE(Function.Blocks.front().Succs.empty());
              for (const auto &Op : Function.Blocks.front().Ops)
                EXPECT_NE(Op.Opcode, NdOp::CALL);
            }
  }
}

TEST(ResumableTrapBoundary, LongOwnedNopsRetainRealContinuation) {
  for (const auto &Case : siblingTraps()) {
    if (!Case.Resumable)
      continue;
    auto Image = trapImage(Case);
    auto &Text = Image.Segments.front();
    const auto Nop = siblingNops(Case).front();
    constexpr unsigned Count = 256;
    for (unsigned I = 0; I < Count; ++I)
      Text.Data.insert(Text.Data.begin() + Case.Trap.size(), Nop.begin(),
                       Nop.end());
    Text.Size = Text.Data.size();
    Image.KnownCodeRanges = {{Entry, Entry + Text.Size}};
    const auto Function = build(Image);
    EXPECT_TRUE(Function.hasCompleteLiftCoverage()) << Case.Name;
    EXPECT_EQ(Function.DecodedInstructionCount, Count + 3u) << Case.Name;
    ASSERT_EQ(Function.Blocks.size(), 1u);
    EXPECT_EQ(Function.Blocks.front().EndAddr, Entry + Text.Size);
  }
}

TEST(ResumableTrapBoundary, LongOwnedNopsDoNotHideMalformedContinuation) {
  for (unsigned Count : {1u, 65u, 256u})
    for (bool Missing : {false, true}) {
      auto Image = image();
      auto &Text = Image.Segments.front();
      Text.Data.resize(6);
      Text.Data.insert(Text.Data.end(), Count, 0x90);
      const va_t BadAddress = Entry + Text.Data.size();
      if (!Missing)
        Text.Data.push_back(0x0f);
      Text.Size = BadAddress + 1 - Entry;
      Image.KnownCodeRanges = {{Entry, BadAddress + 1}};
      const auto Function = build(Image);
      EXPECT_FALSE(Function.hasCompleteLiftCoverage());
      EXPECT_EQ(Function.DecodedInstructionCount, Count + 2u);
      EXPECT_EQ(Missing ? Function.TruncatedPathAddresses
                        : Function.DecodeFailureAddresses,
                std::vector<va_t>{BadAddress});
    }
}

TEST(ResumableTrapBoundary, AlignmentNopClassifiersAgreeAcrossDecodeRoutes) {
  for (const auto &Case : siblingTraps())
    for (const auto &Nop : siblingNops(Case))
      for (bool Detail : {false, true})
        for (bool PrintText : {false, true})
          for (unsigned Route = 0; Route != 3; ++Route) {
            SCOPED_TRACE(::testing::Message()
                         << Case.Name << "/" << Nop.size() << "/" << Detail
                         << "/" << PrintText << "/" << Route);
            Decoder Dec;
            ASSERT_TRUE(Dec.init(Case.Architecture, Case.Mode));
            Dec.setDetail(Detail);
            Dec.setText(PrintText);
            DecodedInsn Insn;
            const int Size =
                Route == 0 ? Dec.decodeOne(Nop.data(), Nop.size(), Entry, Insn)
                : Route == 1
                    ? Dec.decodeOneLight(Nop.data(), Nop.size(), Entry, Insn)
                    : Dec.decodeOneForLift(Nop.data(), Nop.size(), Entry, Insn);
            ASSERT_EQ(Size, Nop.size());
            EXPECT_TRUE(Dec.isAlignmentNop(Insn));
          }
}

TEST(ResumableTrapBoundary, OtherArmHintsAreNotAlignmentNops) {
  for (uint8_t Condition = 0; Condition != 16; ++Condition)
    for (uint8_t Hint : {0u, 1u, 2u, 3u, 4u, 5u}) {
      if (Hint == 0 && Condition != 15)
        continue;
      const uint8_t Bytes[] = {Hint, 0xf0, 0x20,
                               static_cast<uint8_t>((Condition << 4) | 3)};
      EXPECT_EQ(alignmentNopLength(Arch::ARM, Bytes, sizeof(Bytes),
                                   InstructionMode::ARM),
                0u);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Arch::ARM, InstructionMode::ARM));
      DecodedInsn Insn;
      if (Dec.decodeOneLight(Bytes, sizeof(Bytes), Entry, Insn))
        EXPECT_FALSE(Dec.isAlignmentNop(Insn));
    }
}

TEST(ResumableTrapBoundary, NopLookalikesRetainObservableContinuation) {
  struct Case {
    Arch Architecture;
    std::vector<uint8_t> Code;
  };
  for (const auto &C :
       std::vector<Case>{{Arch::X64, {0x41, 0x90}}, // xchg eax, r8d
                         {Arch::X64, {0x49, 0x90}}, // xchg rax, r8
                         {Arch::X64, {0x89, 0xc0}}, // clears upper RAX
                         {Arch::X64, {0xf3, 0x90}}, // pause
                         {Arch::X86, {0x40}}}) {    // inc eax, not REX
    auto Image = image(C.Architecture);
    auto &Text = Image.Segments.front();
    Text.Data.insert(Text.Data.begin() + 6, C.Code.begin(), C.Code.end());
    Text.Size = Text.Data.size();
    const va_t Neighbor = Next + C.Code.size();
    Image.KnownCodeRanges = {{Entry, Neighbor}, {Neighbor, Entry + Text.Size}};
    const auto Function = build(Image);
    EXPECT_TRUE(Function.hasCompleteLiftCoverage());
    bool CallsNeighbor = false;
    for (const auto &Block : Function.Blocks)
      for (const auto &Op : Block.Ops)
        CallsNeighbor |= Op.Opcode == NdOp::CALL && Op.Addr == Neighbor;
    EXPECT_TRUE(CallsNeighbor);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    for (bool Detail : {false, true}) {
      Dec.setDetail(Detail);
      DecodedInsn Insn;
      ASSERT_EQ(Dec.decodeOneLight(C.Code.data(), C.Code.size(), Next, Insn),
                C.Code.size());
      EXPECT_FALSE(Dec.isAlignmentNop(Insn));
    }
  }
}

TEST(ResumableTrapBoundary, InstructionMustFitItsSizedOwner) {
  auto Cases = siblingTraps();
  // The extent rule also applies before any trap, to ordinary instructions.
  Cases.push_back({"mov",
                   Arch::X64,
                   InstructionMode::Default,
                   {0xb8, 7, 0, 0, 0},
                   {0xc3},
                   false});
  Cases.push_back({"call",
                   Arch::X64,
                   InstructionMode::Default,
                   {0xe8, 0, 0x20, 0, 0},
                   {0xc3},
                   false});
  for (const auto &Case : Cases)
    for (unsigned Cut = 1; Cut < Case.Trap.size(); ++Cut)
      for (BinaryFormat Format :
           {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
        for (unsigned Ownership = 0; Ownership != 4; ++Ownership) {
          SCOPED_TRACE(::testing::Message()
                       << Case.Name << "/" << Cut << "/"
                       << static_cast<int>(Format) << "/" << Ownership);
          auto Image = trapImage(Case);
          Image.Format = Format;
          va_t Address = Entry;
          if (Ownership == 0)
            Image.KnownCodeRanges = {{Entry, Entry + Cut}};
          else if (Ownership == 1)
            addSymbol(Image, Entry, Cut, "trap_owner");
          else {
            ExceptionFunction Owner;
            Owner.Kind = RuntimeFunctionKind::Primary;
            Owner.Encoding = ExceptionEncoding::DwarfFDE;
            Owner.ParseStatus = ExceptionParseStatus::Complete;
            Owner.CodeRange = {Entry, Entry + Cut};
            if (Ownership == 3) {
              auto &Text = Image.Segments.front();
              auto Body = std::move(Text.Data);
              if (Case.Architecture == Arch::AArch64)
                Text.Data = {4, 0, 0, 0x14}; // b +16
              else if (Case.Architecture == Arch::ARM)
                Text.Data = Case.Mode == InstructionMode::Thumb
                                ? std::vector<uint8_t>{6, 0xe0}
                                : std::vector<uint8_t>{2, 0, 0, 0xea};
              else
                Text.Data = {0xeb, 14};
              Owner.CodeRange.End = Entry + Text.Data.size();
              Text.Data.resize(16);
              Text.Data.insert(Text.Data.end(), Body.begin(), Body.end());
              Text.Size = Text.Data.size();
              Address += 16;
              Owner.FragmentRanges = {{Address, Address + Cut}};
            }
            Image.ExceptionMetadata.Functions.push_back(Owner);
            Image.ExceptionMetadata.rebuildIndex();
          }
          const auto Function = build(Image);
          EXPECT_FALSE(Function.hasCompleteInstructionLift());
          EXPECT_EQ(Function.DecodeFailureAddresses,
                    std::vector<va_t>{Address});
          EXPECT_EQ(Function.DecodedInstructionCount, Ownership == 3 ? 1u : 0u);
        }
}

TEST(ResumableTrapBoundary, ContiguousOwnershipCanCoverOneInstruction) {
  for (bool Peek : {false, true})
    for (bool Gap : {false, true}) {
      auto Image = image();
      auto &Text = Image.Segments.front();
      Text.Data = {0xcd, 3, 0xc3};
      if (Peek)
        Text.Data.insert(Text.Data.begin(), 0xcc);
      Text.Size = Text.Data.size();
      const va_t Address = Entry + (Peek ? 1 : 0);
      ExceptionFunction Owner;
      Owner.CodeRange = {Entry, Address + 1};
      Owner.FragmentRanges = {{Address + 1 + (Gap ? 1 : 0), Entry + Text.Size}};
      Owner.Kind = RuntimeFunctionKind::Primary;
      Owner.Encoding = ExceptionEncoding::X64UnwindV1;
      Owner.ParseStatus = ExceptionParseStatus::Complete;
      Image.ExceptionMetadata.Functions.push_back(Owner);
      Image.ExceptionMetadata.rebuildIndex();
      const auto Function = build(Image);
      EXPECT_EQ(Function.hasCompleteLiftCoverage(), !Gap);
      if (Gap)
        EXPECT_EQ(Function.DecodeFailureAddresses, std::vector<va_t>{Address});
      else
        EXPECT_EQ(Function.DecodedInstructionCount, Peek ? 3u : 2u);
    }
}

TEST(ResumableTrapBoundary, CoarseUnwindExtentDoesNotSplitAnInstruction) {
  for (ExceptionEncoding Encoding :
       {ExceptionEncoding::CompactUnwind, ExceptionEncoding::X86ScopeTableEH3,
        ExceptionEncoding::ARMEHABIInline})
    for (unsigned EndOffset : {1u, 7u, 8u})
      for (bool Nops : {false, true}) {
        // A coarse frame span may cover several functions or be reconstructed.
        // Its end is not a sized function boundary, before or after a trap.
        auto Image = image();
        if (Nops) {
          auto &Text = Image.Segments.front();
          Text.Data.insert(Text.Data.begin() + 6, {0x48, 0x90});
          Text.Size = Text.Data.size();
        }
        ExceptionFunction Owner;
        Owner.CodeRange = {Entry, Entry + EndOffset};
        Owner.Kind = RuntimeFunctionKind::Primary;
        Owner.Encoding = Encoding;
        Owner.ParseStatus = ExceptionParseStatus::Complete;
        Image.ExceptionMetadata.Functions.push_back(Owner);
        Image.ExceptionMetadata.rebuildIndex();
        const auto Function = build(Image);
        EXPECT_TRUE(Function.hasCompleteLiftCoverage());
        EXPECT_EQ(Function.DecodedInstructionCount, Nops ? 5u : 4u);
      }
}

TEST(ResumableTrapBoundary, StrongSizeOutlivesCoarseFrameCoverage) {
  for (bool Peek : {false, true})
    for (bool Complete : {false, true})
      for (bool SymbolSize : {false, true}) {
        auto Image = image(Arch::X86);
        auto &Text = Image.Segments.front();
        if (!Peek)
          Text.Data.erase(Text.Data.begin() + 5);
        Text.Size = Text.Data.size();
        ExceptionFunction Owner;
        Owner.CodeRange = {Entry, Entry + 1};
        Owner.Kind = RuntimeFunctionKind::Primary;
        Owner.Encoding = ExceptionEncoding::X86ScopeTableEH3;
        Owner.ParseStatus = ExceptionParseStatus::Complete;
        if (Complete && Peek)
          Owner.FragmentRanges = {{Entry + 5, Entry + 6}};
        Image.ExceptionMetadata.Functions.push_back(Owner);
        Image.ExceptionMetadata.rebuildIndex();
        const va_t Address = Entry + (Peek ? 6 : 5);
        const va_t End = Complete ? Entry + Text.Size : Address + 1;
        if (SymbolSize)
          addSymbol(Image, Entry, End - Entry, "trap_owner");
        else
          Image.KnownCodeRanges = {{Entry, End}};
        const auto Function = build(Image);
        EXPECT_EQ(Function.hasCompleteLiftCoverage(), Complete);
        if (Complete)
          EXPECT_EQ(Function.DecodedInstructionCount, Peek ? 4u : 3u);
        else
          EXPECT_EQ(Function.DecodeFailureAddresses,
                    std::vector<va_t>{Address});
      }
}

TEST(ResumableTrapBoundary, FragmentMayResumeAtPrimaryEntry) {
  auto Image = image();
  auto &Text = Image.Segments.front();
  Text.VA = Entry - 6;
  Text.Data = {0xb8, 7, 0, 0, 0, 0xcc, 0xeb, 0xf8};
  Text.Size = Text.Data.size();
  ExceptionFunction Owner;
  Owner.CodeRange = {Entry, Entry + 2};
  Owner.FragmentRanges = {{Entry - 6, Entry}};
  Owner.Kind = RuntimeFunctionKind::Primary;
  Owner.Encoding = ExceptionEncoding::X64UnwindV1;
  Owner.ParseStatus = ExceptionParseStatus::Complete;
  Image.ExceptionMetadata.Functions.push_back(Owner);
  Image.ExceptionMetadata.rebuildIndex();
  const auto Function = build(Image);
  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  EXPECT_EQ(Function.DecodedInstructionCount, 3u);
  for (const auto &Block : Function.Blocks)
    EXPECT_FALSE(Block.Succs.empty());
}

TEST(ResumableTrapBoundary, BranchIntoNopSuffixKeepsItsOwnFallthrough) {
  auto Image = image();
  auto &Text = Image.Segments.front();
  // test ecx, ecx; jz nop; int3; nop; neighbor: mov eax, 42; ret.
  Text.Data = {0x85, 0xc9, 0x74, 1, 0xcc, 0x90, 0xb8, 42, 0, 0, 0, 0xc3};
  Text.Size = Text.Data.size();
  Image.KnownCodeRanges = {{Entry, Next}, {Next, Entry + Text.Size}};
  const auto Function = build(Image);
  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  bool TrapStops = false, NopCallsNeighbor = false;
  for (const auto &Block : Function.Blocks) {
    if (Block.EndAddr == Entry + 5)
      TrapStops = Block.Succs.empty();
    for (const auto &Op : Block.Ops)
      NopCallsNeighbor |= Op.Opcode == NdOp::CALL && Op.Addr == Next;
  }
  EXPECT_TRUE(TrapStops);
  EXPECT_TRUE(NopCallsNeighbor);
}

TEST(ResumableTrapBoundary, OwnedInvalidContinuationRetainsFailure) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (unsigned Ownership = 0; Ownership != 4; ++Ownership)
        for (unsigned Failure = 0; Failure != 4; ++Failure) {
          SCOPED_TRACE(::testing::Message()
                       << static_cast<int>(Architecture) << "/"
                       << static_cast<int>(Format) << "/" << Ownership << "/"
                       << Failure);
          auto Image = image(Architecture, Format);
          auto &Text = Image.Segments.front();
          Text.Data.resize(6);
          if (Failure == 0)
            Text.Data.push_back(0x0f); // Truncated instruction.
          if (Failure == 1)
            Text.Data.insert(Text.Data.end(), {0xb8, 42, 0, 0, 0, 0xc3});
          // Failure 1 has enough physical bytes, but only the first opcode
          // byte belongs to this function. 2/3 have missing mapped/unmapped
          // continuation bytes respectively.
          Text.Size = Failure == 2 ? 7 : Text.Data.size();
          if (Ownership == 1)
            Image.KnownCodeRanges = {{Entry, Next + 1}};
          else if (Ownership == 2)
            addSymbol(Image, Entry, 7, "trap_owner");
          else if (Ownership == 3) {
            ExceptionFunction Owner;
            Owner.CodeRange = {Entry, Next - 1};
            Owner.Kind = RuntimeFunctionKind::Primary;
            Owner.Encoding = ExceptionEncoding::X64UnwindV1;
            Owner.ParseStatus = ExceptionParseStatus::Complete;
            Owner.FragmentRanges = {{Next - 1, Next + 1}};
            Image.ExceptionMetadata.Functions.push_back(Owner);
            Image.ExceptionMetadata.rebuildIndex();
          }
          const auto Function = build(Image);
          if (Ownership == 0) {
            // Without ownership, undecodable bytes still bound the heuristic
            // walk; a complete, decodable continuation remains valid.
            EXPECT_TRUE(Function.hasCompleteLiftCoverage());
          } else {
            EXPECT_FALSE(Function.hasCompleteLiftCoverage());
            if (Failure < 2)
              EXPECT_EQ(Function.DecodeFailureAddresses,
                        std::vector<va_t>{Next});
            else
              EXPECT_EQ(Function.TruncatedPathAddresses,
                        std::vector<va_t>{Next});
          }
        }
}

TEST(ResumableTrapBoundary, FragmentOnlyEndStopsTrapsAndCalls) {
  for (bool Call : {false, true})
    for (bool Continues : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Call << "/" << Continues);
      auto Image = image();
      auto &Text = Image.Segments.front();
      Text.Data = {0xe9, 11, 0, 0, 0}; // jmp to the cold fragment.
      Text.Data.resize(16, 0xcc);
      Text.Data.insert(Text.Data.end(), {0xb8, 7, 0, 0, 0});
      if (Call)
        Text.Data.insert(Text.Data.end(), {0xe8, 0, 0x20, 0, 0});
      else
        Text.Data.push_back(0xcc);
      const va_t FragmentEnd = Entry + Text.Data.size();
      Text.Data.insert(Text.Data.end(), {0xb8, 42, 0, 0, 0, 0xc3});
      Text.Size = Text.Data.size();
      ExceptionFunction Owner;
      Owner.CodeRange = {Entry, Entry + 5};
      Owner.Kind = RuntimeFunctionKind::Primary;
      Owner.Encoding = ExceptionEncoding::X64UnwindV1;
      Owner.ParseStatus = ExceptionParseStatus::Complete;
      Owner.FragmentRanges = {{Entry + 16, FragmentEnd}};
      if (Continues)
        Owner.FragmentRanges.push_back({FragmentEnd, Entry + Text.Size});
      Image.ExceptionMetadata.Functions.push_back(Owner);
      Image.ExceptionMetadata.rebuildIndex();
      const auto Function = build(Image);
      EXPECT_TRUE(Function.hasCompleteLiftCoverage());
      EXPECT_EQ(Function.DecodedInstructionCount, Continues ? 5u : 3u);
      for (const auto &Block : Function.Blocks)
        EXPECT_LE(Block.EndAddr, Continues ? Entry + Text.Size : FragmentEnd);
    }
}

TEST(ResumableTrapBoundary, FragmentEndInsideCallDoesNotProveNoReturn) {
  auto Image = image();
  auto &Text = Image.Segments.front();
  Text.Data = {0xe9, 11, 0, 0, 0};
  Text.Data.resize(16, 0xcc);
  Text.Data.insert(Text.Data.end(), {0xe8, 0, 0x20, 0, 0, 0xc3});
  Text.Size = Text.Data.size();
  ExceptionFunction Owner;
  Owner.CodeRange = {Entry, Entry + 5};
  Owner.Kind = RuntimeFunctionKind::Primary;
  Owner.Encoding = ExceptionEncoding::X64UnwindV1;
  Owner.ParseStatus = ExceptionParseStatus::Complete;
  // An inconsistent fragment end in the call's displacement cannot establish
  // the non-returning contract of a call ending exactly at an owned boundary.
  Owner.FragmentRanges = {{Entry + 16, Entry + 20}};
  Image.ExceptionMetadata.Functions.push_back(Owner);
  Image.ExceptionMetadata.rebuildIndex();
  EXPECT_FALSE(lowFunctionNeverReturns(build(Image), Arch::X64));
}

TEST(ResumableTrapBoundary, SiblingTrapClassificationsAgreeWithLightDecode) {
  for (const auto &Case : siblingTraps())
    for (bool Detail : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << Case.Name << "/" << static_cast<int>(Case.Architecture)
                   << "/" << Detail);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Case.Architecture, Case.Mode));
      Dec.setDetail(Detail);
      DecodedInsn Insn;
      ASSERT_EQ(
          Dec.decodeOneLight(Case.Trap.data(), Case.Trap.size(), Entry, Insn),
          Case.Trap.size());
      // ARM control classification requires operand detail for predicates.
      // Trap identity itself is available from the opcode in either mode.
      if (Detail || Case.Architecture != Arch::ARM)
        EXPECT_TRUE(Dec.isFunctionTerminator(Insn));
      EXPECT_EQ(Dec.isResumableTrap(Insn), Case.Resumable);
    }
}

TEST(ResumableTrapBoundary, SiblingTrapsRespectRangeEndsAndAdjacentEntries) {
  for (const auto &Case : siblingTraps())
    for (bool RangeEnd : {false, true}) {
      SCOPED_TRACE(::testing::Message()
                   << Case.Name << "/" << static_cast<int>(Case.Architecture)
                   << "/" << RangeEnd);
      auto Image = trapImage(Case);
      const va_t End = Entry + Case.Trap.size();
      std::set<va_t> Entries;
      if (RangeEnd)
        Image.KnownCodeRanges.emplace_back(Entry, End);
      else
        Entries.insert(End);
      const auto Function = build(Image, Entries);
      EXPECT_TRUE(Function.hasCompleteLiftCoverage());
      if (Case.Resumable)
        EXPECT_FALSE(lowFunctionNeverReturns(Function, Case.Architecture));
      ASSERT_EQ(Function.Blocks.size(), 1u);
      const auto &Block = Function.Blocks.front();
      EXPECT_EQ(Block.EndAddr, End);
      EXPECT_TRUE(Block.Succs.empty());
      ASSERT_EQ(Block.InstructionBoundaries.size(), 1u);
      const auto &Boundary = Block.InstructionBoundaries.front();
      EXPECT_EQ(Boundary.Control, LowInstructionControl::Terminator);
      EXPECT_EQ(
          hasLowInstructionControlFlag(Boundary.ControlFlags,
                                       LowInstructionControlFlag::Resumable),
          Case.Resumable);
      for (const auto &Op : Block.Ops) {
        EXPECT_NE(Op.Opcode, NdOp::CALL);
        EXPECT_NE(Op.Opcode, NdOp::RETURN);
      }
    }
}

TEST(ResumableTrapBoundary,
     SiblingInteriorTrapsKeepOnlyResumableContinuations) {
  for (const auto &Case : siblingTraps()) {
    SCOPED_TRACE(::testing::Message()
                 << Case.Name << "/" << static_cast<int>(Case.Architecture));
    const auto Function = build(trapImage(Case));
    EXPECT_TRUE(Function.hasCompleteLiftCoverage());
    EXPECT_EQ(Function.DecodedInstructionCount, Case.Resumable ? 3u : 1u);
    if (Case.Resumable)
      EXPECT_FALSE(lowFunctionNeverReturns(Function, Case.Architecture));
    ASSERT_EQ(Function.Blocks.size(), 1u);
    EXPECT_EQ(Function.Blocks.front().EndAddr,
              Entry + Case.Trap.size() +
                  (Case.Resumable ? Case.Continuation.size() : 0));
  }
}

TEST(ResumableTrapBoundary, ConditionalARMPathCanSkipTrap) {
  auto Image = image(Arch::ARM);
  Image.Mode = InstructionMode::Thumb;
  auto &Text = Image.Segments.front();
  Text.Data = {0,    0x28,  // cmp r0, #0
               0,    0xd1,  // bne Entry + 6
               0xfe, 0xde,  // udf #254
               42,   0x20,  // movs r0, #42
               0x70, 0x47}; // bx lr
  Text.Size = Text.Data.size();
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  DecodedInsn Insn;
  ASSERT_EQ(Dec.decodeOne(Text.Data.data(), Text.Data.size(), Entry, Insn), 2);
  ASSERT_EQ(Dec.decodeOne(Text.Data.data() + 2, Text.Data.size() - 2, Entry + 2,
                          Insn),
            2);
  EXPECT_FALSE(Dec.isFunctionTerminator(Insn));
  const auto Function = build(Image);
  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  EXPECT_FALSE(lowFunctionNeverReturns(Function, Arch::ARM));
  bool SawReturn = false;
  for (const auto &Block : Function.Blocks)
    for (const auto &Boundary : Block.InstructionBoundaries)
      SawReturn |= Boundary.Control == LowInstructionControl::Return &&
                   Boundary.Address == Entry + 8;
  EXPECT_TRUE(SawReturn);
}

TEST(ResumableTrapBoundary, ThumbContinuationPeekDoesNotConsumeITState) {
  auto Image = image(Arch::ARM);
  Image.Mode = InstructionMode::Thumb;
  auto &Text = Image.Segments.front();
  Text.Data = {1,    0xbe,  // bkpt #1
               0x04, 0xbf,  // itt eq
               42,   0x20,  // moveq r0, #42
               7,    0x21,  // moveq r1, #7
               0x70, 0x47}; // bx lr
  Text.Size = Text.Data.size();
  const auto Function = build(Image);
  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  EXPECT_EQ(Function.DecodedInstructionCount, 5u);
  unsigned ConditionalWrites = 0;
  for (const auto &Block : Function.Blocks)
    for (const auto &Op : Block.Ops)
      ConditionalWrites += Op.Opcode == NdOp::SELECT && Op.Output.isReg() &&
                           (Op.Addr == Entry + 4 || Op.Addr == Entry + 6);
  EXPECT_EQ(ConditionalWrites, 2u);
}

TEST(ResumableTrapBoundary, OrdinaryInterruptKeepsItsContinuation) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    const TrapCase Case{"int 0x80",
                        Architecture,
                        InstructionMode::Default,
                        {0xcd, 0x80},
                        {0xb8, 42, 0, 0, 0, 0xc3},
                        true};
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Architecture));
    DecodedInsn Insn;
    ASSERT_EQ(Dec.decodeOne(Case.Trap.data(), Case.Trap.size(), Entry, Insn),
              2);
    EXPECT_FALSE(Dec.isFunctionTerminator(Insn));
    EXPECT_FALSE(Dec.isResumableTrap(Insn));
    const auto Function = build(trapImage(Case));
    EXPECT_TRUE(Function.hasCompleteLiftCoverage());
    EXPECT_EQ(Function.DecodedInstructionCount, 3u);
    EXPECT_FALSE(lowFunctionNeverReturns(Function, Architecture));
  }
}

TEST(ResumableTrapBoundary, ConsecutiveInteriorBreaksRetainContinuation) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (BinaryFormat Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (unsigned Metadata = 0; Metadata != 4; ++Metadata)
        for (unsigned Count : {2u, 80u}) {
          if (Metadata == 0 && Count > 2)
            continue; // Unbounded padding lookahead deliberately has a budget.
          SCOPED_TRACE(::testing::Message()
                       << int(Architecture) << "/" << int(Format) << "/"
                       << Metadata << "/" << Count);
          auto Image = image(Architecture, Format);
          auto &Text = Image.Segments.front();
          Text.Data.insert(Text.Data.begin() + 6, Count - 1, 0xcc);
          Text.Size = Text.Data.size();
          const va_t End = Entry + Text.Size;
          if (Metadata == 1)
            Image.KnownCodeRanges.emplace_back(Entry, End);
          else if (Metadata == 2)
            addSymbol(Image, Entry, Text.Size, "trap_owner");
          else if (Metadata == 3) {
            ExceptionFunction Owner;
            Owner.CodeRange = {Entry, End};
            Owner.Kind = RuntimeFunctionKind::Primary;
            Owner.Encoding = ExceptionEncoding::X64UnwindV1;
            Owner.ParseStatus = ExceptionParseStatus::Complete;
            Image.ExceptionMetadata.Functions.push_back(std::move(Owner));
            Image.ExceptionMetadata.rebuildIndex();
          }
          const auto Function = build(Image);
          EXPECT_TRUE(Function.hasCompleteLiftCoverage());
          EXPECT_EQ(Function.DecodedInstructionCount, Count + 3);
          ASSERT_EQ(Function.Blocks.size(), 1u);
          EXPECT_EQ(Function.Blocks.front().EndAddr, End);
          EXPECT_EQ(
              Function.Blocks.front().InstructionBoundaries.back().Control,
              LowInstructionControl::Return);
        }
}

TEST(ResumableTrapBoundary, UnownedTrapAndNopPaddingDoesNotReachNeighbor) {
  for (bool NoOps : {false, true}) {
    auto Image = image();
    auto &Text = Image.Segments.front();
    Text.Data = {0xb8, 7,    0,    0,  0, 0xcc, 0xcc, 0xcc,
                 0x90, 0x90, 0xb8, 42, 0, 0,    0,    0xc3};
    if (!NoOps)
      Text.Data[8] = Text.Data[9] = 0xcc;
    Text.Size = Text.Data.size();
    expectStoppedAtTrap(build(Image, {Entry + 10}));
  }
}

TEST(ResumableTrapBoundary, PaddingGuessesDoNotOverrideResumableFallthrough) {
  for (unsigned Evidence = 0; Evidence != 5; ++Evidence)
    for (bool Indexed : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Evidence << "/" << Indexed);
      auto Image = image();
      auto &Text = Image.Segments.front();
      Text.Data.insert(Text.Data.begin() + 6, 0xcc);
      Text.Size = Text.Data.size();
      const va_t Continuation = Next + 1;
      auto Guess = Symbol::makeFunc(Continuation);
      Guess.IsBoundaryGuess = true;
      Image.Symbols.push_back(Guess);
      std::set<va_t> Entries;
      switch (Evidence) {
      case 1:
        Image.Symbols.back().Name = "confirmed_by_debug_info";
        break;
      case 2:
        addSymbol(Image, Continuation, 0, "independent_alias");
        break;
      case 3:
        Image.KnownCodeRanges.emplace_back(Continuation, Entry + Text.Size);
        break;
      case 4:
        Entries.insert(Continuation); // Independently discovered call target.
        break;
      }
      const ExecutableCodeOwnerIndex Index(Image);
      const auto *Owners = Indexed ? &Index : nullptr;
      EXPECT_EQ(Image.hasFunctionSymbolAt(Continuation, Owners,
                                          /*IncludeBoundaryGuesses=*/false),
                Evidence != 0 && Evidence != 4);
      EXPECT_TRUE(Image.hasFunctionSymbolAt(Continuation, Owners));
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Image));
      CFGBuilder Builder;
      Builder.setKnownFuncEntries(&Entries);
      Builder.setExecutableCodeOwnerIndex(Owners);
      const auto Function = Builder.build(Image, Dec, Entry, "trap_owner");
      if (Evidence != 0) {
        expectStoppedAtTrap(Function);
      } else {
        EXPECT_TRUE(Function.hasCompleteLiftCoverage());
        EXPECT_EQ(Function.DecodedInstructionCount, 5u);
        ASSERT_EQ(Function.Blocks.size(), 1u);
        EXPECT_EQ(Function.Blocks.front().EndAddr, Entry + Text.Size);
      }
    }
}

TEST(ResumableTrapBoundary, ReachedColdBlockCanResumeBeyondPrimaryRange) {
  auto Image = image();
  auto &Text = Image.Segments.front();
  Text.Data = {0xeb, 6,    0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc,
               0xcc, 0xcc, 0xb8, 42,   0,    0,    0,    0xc3};
  Text.Size = Text.Data.size();
  Image.KnownCodeRanges.emplace_back(Entry, Entry + 2);
  const auto Function = build(Image);
  EXPECT_TRUE(Function.hasCompleteLiftCoverage());
  EXPECT_EQ(Function.DecodedInstructionCount, 5u);
  ASSERT_EQ(Function.Blocks.size(), 2u);
  EXPECT_EQ(Function.Blocks.back().EndAddr, Entry + Text.Size);
  EXPECT_EQ(Function.Blocks.back().InstructionBoundaries.back().Control,
            LowInstructionControl::Return);
}

TEST(ResumableTrapBoundary, FullAndSelectedPipelinesAbsorbResumedPaddingGuess) {
  for (bool Selected : {false, true})
    for (bool LLVMRoute : {false, true}) {
      SCOPED_TRACE(::testing::Message() << Selected << "/" << LLVMRoute);
      auto Image = image();
      Image.IsRelocatable = true;
      auto &Text = Image.Segments.front();
      Text.Data = {0xcc, 0xcc, 0xb8, 42, 0, 0, 0, 0xc3};
      Text.Size = Text.Data.size();
      addSymbol(Image, Entry, 0, "double_debugbreak");
      auto Guess = Symbol::makeFunc(Entry + 2);
      Guess.IsBoundaryGuess = true;
      Image.Symbols.push_back(Guess);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      if (Selected)
        Options.OnlyFunctionEntries.insert(Entry);
      Options.LiftMode = LLVMRoute;
      Options.SourceProjection = LLVMRoute;
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.LowFuncs.size(), 1u);
      const auto &Function = Result.LowFuncs.front();
      EXPECT_EQ(Function.Entry, Entry);
      EXPECT_TRUE(Function.hasCompleteLiftCoverage());
      EXPECT_EQ(Function.DecodedInstructionCount, 4u);
      EXPECT_EQ(Function.Blocks.back().EndAddr, Entry + Text.Size);
      if (!Selected) {
        const auto Audit =
            llvm::find_if(Result.FunctionAudits,
                          [](const auto &A) { return A.Entry == Entry + 2; });
        ASSERT_NE(Audit, Result.FunctionAudits.end());
        EXPECT_EQ(Audit->Disposition,
                  PipelineFunctionDisposition::AbsorbedFunctionChunk);
      }
    }
}

TEST(ResumableTrapBoundary, ResumableTrapsDoNotDefineReturnRegisters) {
  for (const auto &Case : siblingTraps()) {
    if (!Case.Resumable)
      continue;
    SCOPED_TRACE(::testing::Message()
                 << Case.Name << "/" << int(Case.Architecture));
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Case.Architecture, Case.Mode));
    DecodedInsn Insn;
    ASSERT_EQ(Dec.decodeOne(Case.Trap.data(), Case.Trap.size(), Entry, Insn),
              Case.Trap.size());
    std::vector<LowOp> Ops;
    Dec.liftToLow(Insn, Ops);
    unsigned Intrinsics = 0;
    for (const auto &Op : Ops) {
      if (Op.Opcode != NdOp::INTRINSIC)
        continue;
      ++Intrinsics;
      EXPECT_EQ(Op.Output.Size, 0u);
      EXPECT_FALSE(lowIntrinsicIsUnconditionalTrap(Op));
      if (Op.Inputs[0].Offset == static_cast<uint64_t>(Intrinsic::Int3))
        EXPECT_EQ(Op.NumInputs,
                  1u); // All x86 encodings have one void contract.
    }
    EXPECT_EQ(Intrinsics, 1u);
  }
}

TEST(ResumableTrapBoundary,
     BothSourceRoutesPreserveLiveValuesAndRepeatedBreaks) {
  for (const auto &Case : siblingTraps()) {
    if (!Case.Resumable)
      continue;
    for (bool Repeated : {false, true})
      for (bool LLVMRoute : {false, true})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << Case.Name << "/" << int(Case.Architecture) << "/"
                       << Repeated << "/" << LLVMRoute << "/" << NoOpt);
          auto Image = trapImage(Case);
          auto &Text = Image.Segments.front();
          if (Repeated) {
            Text.Data.insert(Text.Data.begin(), Case.Trap.begin(),
                             Case.Trap.end());
          } else {
            // The value is defined before the break and remains live across it.
            switch (Case.Architecture) {
            case Arch::X86:
            case Arch::X64:
              Text.Data = {0xb8, 7, 0, 0, 0};
              break;
            case Arch::ARM:
              Text.Data = Case.Mode == InstructionMode::Thumb
                              ? std::vector<uint8_t>{7, 0x20}
                              : std::vector<uint8_t>{7, 0, 0xa0, 0xe3};
              break;
            case Arch::AArch64:
              Text.Data = {0xe0, 0, 0x80, 0x52}; // mov w0, #7
              break;
            default:
              FAIL() << "unexpected debugger architecture";
            }
            Text.Data.insert(Text.Data.end(), Case.Trap.begin(),
                             Case.Trap.end());
            const size_t ReturnSize =
                Case.Architecture == Arch::ARM
                    ? (Case.Mode == InstructionMode::Thumb ? 2 : 4)
                : Case.Architecture == Arch::AArch64 ? 4
                                                     : 1;
            Text.Data.insert(Text.Data.end(),
                             Case.Continuation.end() - ReturnSize,
                             Case.Continuation.end());
          }
          Text.Size = Text.Data.size();
          Image.KnownCodeRanges = {{Entry, Entry + Text.Size}};
          addSymbol(Image, Entry, Text.Size, "trap_owner");
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries.insert(Entry);
          Options.LiftMode = LLVMRoute;
          Options.SourceProjection = LLVMRoute;
          Options.NoOpt = NoOpt;
          auto Result = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Result.Success) << Result.Error;
          std::string Source;
          llvm::raw_string_ostream OS(Source);
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          if (LLVMRoute) {
            ASSERT_TRUE(Result.LlvmModule);
            EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, COptions,
                                            nullptr, &Image));
          } else {
            ASSERT_FALSE(Result.HighFuncs.empty());
            ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COptions));
          }
          EXPECT_TRUE(
              Source.find(Repeated ? "return 42;" : "return 7;") !=
                  std::string::npos ||
              (Repeated && Source.find("return 0x2A;") != std::string::npos))
              << Source;
          EXPECT_EQ(Source.find("__builtin_trap("), std::string::npos)
              << Source;
          EXPECT_EQ(Source.find("__debugbreak(3)"), std::string::npos)
              << Source;
          const std::string Trap =
              std::string(Case.Name) == "int1"
                  ? (LLVMRoute ? ".byte 0xf1" : "_emit 0xf1")
              : std::string(Case.Name).find("bkpt") != std::string::npos
                  ? "bkpt #"
              : std::string(Case.Name).find("udf") != std::string::npos
                  ? "__asm__ volatile(\"udf"
              : std::string(Case.Name).find("hlt") != std::string::npos
                  ? "__asm__ volatile(\"hlt #"
              : Case.Architecture == Arch::X86 || Case.Architecture == Arch::X64
                  ? "__debugbreak()"
                  : "__builtin_debugtrap()";
          unsigned Breaks = 0;
          for (size_t Position = 0;
               (Position = Source.find(Trap, Position)) != std::string::npos;
               Position += Trap.size())
            ++Breaks;
          EXPECT_EQ(Breaks, Repeated ? 2u : 1u) << Source;
        }
  }
}

} // namespace
