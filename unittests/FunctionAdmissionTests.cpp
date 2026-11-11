//===- FunctionAdmissionTests.cpp - Decoded function admission
//-------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Function discovery, unwind metadata and strict instruction admission.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/decode/Decoder.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

namespace neverd {
namespace {

BinaryImage makeCodeFixture(va_t Entry, std::vector<uint8_t> Bytes) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x140000000;
  Segment Seg;
  Seg.VA = Entry;
  Seg.Size = Bytes.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Bytes);
  Img.Segments.push_back(std::move(Seg));
  return Img;
}

TEST(FunctionAdmission, CompactCallQueryUsesOnlyTheExactInstruction) {
  const uint8_t Call[] = {0xe8, 0xfb, 0xff, 0xff, 0xff, 0x90};
  const uint8_t Indirect[] = {0xff, 0xd0};
  constexpr va_t Address = 0xfffffffd;
  Decoder Dec;
  for (Arch A : {Arch::X86, Arch::X64}) {
    ASSERT_TRUE(Dec.init(A));
    DecodedInsn Original;
    ASSERT_EQ(Dec.decodeOneLight(Call, sizeof(Call), Address, Original), 5);
    const va_t Target = Dec.directCallTarget(Original);
    ASSERT_EQ(Target, Address);
    for (bool Detail : {false, true}) {
      Dec.setDetail(Detail);
      for (bool Text : {false, true}) {
        Dec.setText(Text);
        for (size_t Size = 0; Size <= sizeof(Call); ++Size)
          EXPECT_EQ(Dec.directX86CallTarget({Call, Size}, Address),
                    Size == 5 ? Target : InvalidVA);
        EXPECT_EQ(Dec.directX86CallTarget(Indirect, Address), InvalidVA);
        EXPECT_EQ(Dec.detailEnabled(), Detail);
        EXPECT_EQ(Dec.textEnabled(), Text);
        EXPECT_EQ(Dec.directCallTarget(Original), Target);
      }
    }
  }
}

TEST(FunctionAdmission, UnwindlessNonLeafCandidateRemainsAFunction) {
  // An exception directory need not describe every executable entry. Neither
  // a stack adjustment nor a call disproves a candidate outside its records.
  constexpr va_t Primary = 0x140001000;
  constexpr va_t NonLeaf = 0x140001010;
  constexpr va_t Leaf = 0x140001020;
  std::vector<uint8_t> Code = {0xc3};
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0x53,                         // push rbx
                           0x48, 0x83, 0xec, 0x20,       // sub rsp, 20h
                           0xe8, 0x06, 0x00, 0x00, 0x00, // call Leaf
                           0x48, 0x83, 0xc4, 0x20,       // add rsp, 20h
                           0x5b, 0xc3});                 // pop rbx; ret
  ASSERT_EQ(Code.size(), 0x20u);
  Code.insert(Code.end(), {0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3}); // mov eax,1
  BinaryImage Img = makeCodeFixture(Primary, Code);
  Img.KnownCodeRanges.push_back({Primary, Primary + 1});
  for (va_t Entry : {Primary, NonLeaf, Leaf})
    Img.Symbols.push_back(Symbol::makeFunc(Entry));

  for (bool Selected : {false, true})
    for (bool Cached : {false, true}) {
      SCOPED_TRACE(Selected);
      SCOPED_TRACE(Cached);
      PipelineAnalysisCache Cache;
      for (va_t Entry : {NonLeaf, Leaf, NonLeaf}) {
        llvm::LLVMContext Ctx;
        PipelineOptions Opts;
        Opts.EmitDumpOutput = false;
        if (Selected)
          Opts.OnlyFunctionEntries.insert(Entry);
        if (Cached)
          Opts.AnalysisCache = &Cache;
        auto Result = Pipeline().run(Img, Ctx, Opts);
        ASSERT_TRUE(Result.Error.empty()) << Result.Error;
        const auto Audit = llvm::find_if(
            Result.FunctionAudits,
            [&](const PipelineFunctionAudit &A) { return A.Entry == Entry; });
        ASSERT_NE(Audit, Result.FunctionAudits.end());
        EXPECT_EQ(Audit->Disposition, PipelineFunctionDisposition::Accepted);
        EXPECT_TRUE(Audit->DecodeFailures.empty());
        EXPECT_TRUE(Audit->UnsupportedInstructions.empty());
        EXPECT_EQ(Audit->DecodedInstructions, Audit->LiftedInstructions);
        EXPECT_TRUE(llvm::any_of(Result.HighFuncs, [&](const HighFunc &F) {
          return F.Entry == Entry;
        }));
      }
    }
}

TEST(FunctionAdmission, UnwindRecordOrderDoesNotVetoSelectedCode) {
  constexpr va_t Primary = 0x140001000;
  constexpr va_t NonLeaf = 0x140001010;
  std::vector<uint8_t> Code = {0xc3};
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0x53, 0x5b, 0xc3}); // push rbx; pop rbx; ret
  const auto DispositionOf = [&](std::vector<std::pair<va_t, va_t>> Ranges,
                                 std::set<va_t> Continuations) {
    BinaryImage Img = makeCodeFixture(Primary, Code);
    Img.KnownCodeRanges = std::move(Ranges);
    Img.ContinuationCodeStarts = std::move(Continuations);
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.OnlyFunctionEntries.insert(NonLeaf);
    auto Result = Pipeline().run(Img, Ctx, Opts);
    for (const auto &Audit : Result.FunctionAudits)
      if (Audit.Entry == NonLeaf)
        return Audit.Disposition;
    return PipelineFunctionDisposition::Candidate;
  };
  const std::pair<va_t, va_t> Other{Primary, Primary + 1};
  const std::pair<va_t, va_t> Own{NonLeaf, NonLeaf + 3};
  // Primary and continuation records retain their ownership roles, but none
  // is required merely to decode an explicitly selected entry.
  EXPECT_EQ(DispositionOf({}, {}), PipelineFunctionDisposition::Accepted);
  EXPECT_EQ(DispositionOf({Other}, {}), PipelineFunctionDisposition::Accepted);
  EXPECT_EQ(DispositionOf({Other, Own}, {}),
            PipelineFunctionDisposition::Accepted);
  EXPECT_EQ(DispositionOf({Own, Other}, {}),
            PipelineFunctionDisposition::Accepted);
  EXPECT_EQ(DispositionOf({Other, Own}, {NonLeaf}),
            PipelineFunctionDisposition::Accepted);
  EXPECT_EQ(DispositionOf({Own, Other}, {NonLeaf}),
            PipelineFunctionDisposition::Accepted);
}

TEST(FunctionAdmission, UnwindlessJumpTargetsUseActualChunkOwnership) {
  constexpr va_t Primary = 0x140001000;
  constexpr va_t Target = Primary + 0x10;
  std::vector<uint8_t> Code = {0xe9, 0x0b, 0x00, 0x00, 0x00}; // jmp Target
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0x53, 0xb8, 7, 0, 0, 0, 0x5b, 0xc3});
  Code.resize(0x30, 0xcc);
  Code.push_back(0xc3);

  for (bool Owned : {false, true})
    for (bool Guessed : {false, true}) {
      SCOPED_TRACE(Owned);
      SCOPED_TRACE(Guessed);
      BinaryImage Img = makeCodeFixture(Primary, Code);
      Img.Entry = Primary;
      Img.KnownCodeRanges.emplace_back(Primary, Primary + 5);
      Img.KnownCodeRanges.emplace_back(Primary + 0x30, Primary + 0x31);
      if (Owned) {
        ExceptionFunction Owner;
        Owner.Kind = RuntimeFunctionKind::Primary;
        Owner.ParseStatus = ExceptionParseStatus::Complete;
        Owner.CodeRange = {Primary, Primary + 5};
        Owner.FragmentRanges.push_back({Target, Target + 8});
        Img.ExceptionMetadata.Functions.push_back(std::move(Owner));
        Img.ExceptionMetadata.rebuildIndex();
      }
      Img.Symbols.push_back(Symbol::makeFunc(Primary));
      Img.Symbols.push_back(Symbol::makeFunc(Primary + 0x30));
      Symbol TargetSymbol = Symbol::makeFunc(Target);
      TargetSymbol.IsBoundaryGuess = Guessed;
      Img.Symbols.push_back(TargetSymbol);
      llvm::LLVMContext Ctx;
      PipelineOptions Opts;
      Opts.EmitDumpOutput = false;
      auto Result = Pipeline().run(Img, Ctx, Opts);
      ASSERT_TRUE(Result.Error.empty()) << Result.Error;
      const auto Audit = llvm::find_if(
          Result.FunctionAudits,
          [&](const PipelineFunctionAudit &A) { return A.Entry == Target; });
      ASSERT_NE(Audit, Result.FunctionAudits.end());
      const bool Absorbed = Guessed && Owned;
      EXPECT_EQ(Audit->Disposition,
                Absorbed ? PipelineFunctionDisposition::AbsorbedFunctionChunk
                         : PipelineFunctionDisposition::Accepted);
      // Absorption needs the target's actual decoded instructions in its owner.
      // A padding guess alone is no proof that it belongs to the caller.
      const va_t Owner = Absorbed ? Primary : Target;
      const auto Low = llvm::find_if(
          Result.LowFuncs, [&](const LowFunc &F) { return F.Entry == Owner; });
      ASSERT_NE(Low, Result.LowFuncs.end());
      EXPECT_TRUE(Low->hasCompleteInstructionLift());
      EXPECT_TRUE(llvm::any_of(Low->Blocks, [&](const LowBlock &B) {
        return llvm::any_of(B.InstructionBoundaries,
                            [&](const LowInstructionBoundary &I) {
                              return I.Address == Target;
                            });
      }));
      EXPECT_TRUE(llvm::any_of(Result.HighFuncs, [&](const HighFunc &F) {
        return F.Entry == Owner;
      }));
    }
}

TEST(FunctionAdmission, UnwindlessEntriesStillRequireCompleteDecoding) {
  constexpr va_t Primary = 0x140001000;
  constexpr va_t Broken = Primary + 0x10;
  std::vector<uint8_t> Code = {0xc3};
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0x53, 0x0f}); // push rbx; truncated opcode
  BinaryImage Img = makeCodeFixture(Primary, Code);
  Img.KnownCodeRanges.emplace_back(Primary, Primary + 1);
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(Broken);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_EQ(Result.FunctionAudits.size(), 1u);
  const auto &Audit = Result.FunctionAudits.front();
  EXPECT_EQ(Audit.Disposition, PipelineFunctionDisposition::RejectedIncomplete);
  EXPECT_EQ(Audit.DecodeFailures, std::vector<va_t>{Broken + 1});
  EXPECT_TRUE(Result.LowFuncs.empty());
  EXPECT_TRUE(Result.HighFuncs.empty());
}

TEST(FunctionAdmission, NativeOutputRequiresEverySelectedEntry) {
  constexpr va_t Primary = 0x140001000;
  constexpr va_t Broken = Primary + 0x10;
  constexpr va_t Unmapped = Primary + 0x100;
  std::vector<uint8_t> Code = {0xb8, 7, 0, 0, 0, 0xc3};
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0x53, 0x0f});
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool Patch : {false, true})
      for (bool Cached : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << unsigned(Format) << ' ' << Patch << ' ' << Cached);
        const auto Image = [&] {
          auto Img = makeCodeFixture(Primary, Code);
          Img.Format = Format;
          return Img;
        }();
        PipelineAnalysisCache Cache;
        for (va_t Extra : {Primary, Broken, Unmapped, Primary}) {
          SCOPED_TRACE(Extra);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.NoOpt = true;
          Options.PatchMode = Patch;
          Options.LiftMode = !Patch;
          Options.OnlyFunctionEntries = {Primary, Extra};
          if (Cached)
            Options.AnalysisCache = &Cache;
          auto Result = Pipeline().run(Image, Context, Options);
          if (Extra == Primary) {
            EXPECT_TRUE(Result.Success) << Result.Error;
            EXPECT_TRUE(Result.Error.empty());
            ASSERT_TRUE(Result.LlvmModule);
          } else {
            EXPECT_FALSE(Result.Success);
            EXPECT_FALSE(Result.Error.empty());
            EXPECT_FALSE(Result.LlvmModule);
            EXPECT_NE(Result.Error.find("selected"), std::string::npos);
            if (Extra == Broken)
              EXPECT_NE(Result.Error.find("rejected-incomplete"),
                        std::string::npos);
          }
        }
      }
}

TEST(FunctionAdmission, ASelectedRejectedParentCannotExportOnlyItsCleanup) {
  constexpr va_t Cleanup = 0x401000, Parent = Cleanup + 0x10;
  std::vector<uint8_t> Code = {0xb8, 7, 0, 0, 0, 0xc3};
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0x53, 0x0f});
  auto Image = makeCodeFixture(Cleanup, Code);
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Base = 0x400000;
  ExceptionFunction EH;
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.CodeRange = {Parent, Parent + 2};
  EH.Encoding = ExceptionEncoding::X86CxxFuncInfo;
  EH.Cxx.emplace().MaxState = 1;
  EH.Cxx->UnwindMap.push_back({-1, Cleanup});
  Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  Image.ExceptionMetadata.rebuildIndex();
  for (bool Lift : {false, true}) {
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.NoOpt = true;
    Options.LiftMode = Lift;
    Options.OnlyFunctionEntries = {Parent};
    auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(llvm::any_of(
        Result.MedFuncs, [&](const MedFunc &F) { return F.Entry == Cleanup; }));
    EXPECT_FALSE(llvm::any_of(
        Result.MedFuncs, [&](const MedFunc &F) { return F.Entry == Parent; }));
    EXPECT_EQ(Result.Success, !Lift) << Result.Error;
    if (Lift) {
      EXPECT_FALSE(Result.LlvmModule);
      EXPECT_NE(Result.Error.find("401010"), std::string::npos);
      EXPECT_NE(Result.Error.find("401011"), std::string::npos);
      EXPECT_NE(Result.Error.find("rejected-incomplete"), std::string::npos);
    }
  }
}

TEST(FunctionAdmission, SelectedLiftReportsUnsupportedInstructionSemantics) {
  constexpr va_t Entry = 0x401000;
  // FCHS reads an incoming x87 payload/tag that the typed PE32 projection
  // cannot prove. Complete decoding alone must not admit its instruction.
  auto Image = makeCodeFixture(Entry, {0xd9, 0xe0, 0xc3});
  Image.Arch = Arch::X86;
  Image.Bits = Bitness::Bits32;
  Image.Base = 0x400000;
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.NoOpt = true;
  Options.LiftMode = true;
  Options.OnlyFunctionEntries = {Entry};
  auto Result = Pipeline().run(Image, Context, Options);
  ASSERT_EQ(Result.FunctionAudits.size(), 1u);
  const auto &Audit = Result.FunctionAudits.front();
  EXPECT_EQ(Audit.DecodedInstructions, Audit.LiftedInstructions);
  EXPECT_TRUE(Audit.DecodeFailures.empty());
  EXPECT_EQ(Audit.UnsupportedInstructions, std::vector<va_t>{Entry});
  EXPECT_FALSE(Result.Success);
  EXPECT_FALSE(Result.LlvmModule);
  EXPECT_NE(Result.Error.find("unsupported instruction at 0x401000"),
            std::string::npos);
}

} // namespace
} // namespace neverd
