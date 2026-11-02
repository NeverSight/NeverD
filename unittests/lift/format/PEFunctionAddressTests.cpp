//===- PEFunctionAddressTests.cpp - Address-taken PE leaf entries
//----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Instruction boundaries and callable ownership for x64 PE/ELF code addresses.
//===----------------------------------------------------------------------===//

#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

BinaryImage peImage(size_t Size = 512) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x401000;
  Img.Entry = Img.Base + 0x100;
  Segment Text;
  Text.VA = Img.Base;
  Text.Size = Size;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data.assign(Size, 0xcc);
  Img.Segments.push_back(std::move(Text));
  Section Code;
  Code.Name = ".text";
  Code.VA = Img.Base;
  Code.Size = Size;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Code));
  return Img;
}

void addressInsn(BinaryImage &Img, va_t Source, va_t Target) {
  auto *Bytes = Img.Segments[0].Data.data() + (Source - Img.Base);
  Bytes[0] = 0x48;
  Bytes[1] = 0x8d;
  Bytes[2] = 0x05; // lea rax, [rip+disp32]
  writeLE<int32_t>(Bytes + 3, static_cast<int32_t>(Target - (Source + 7)));
  Bytes[7] = 0xc3;
}

bool hasEntry(const std::vector<std::pair<va_t, std::string>> &Functions,
              va_t Address) {
  return std::any_of(Functions.begin(), Functions.end(),
                     [&](const auto &F) { return F.first == Address; });
}

TEST(X64FunctionAddresses, RequiresPointerInstructionOwnershipAndCallableExit) {
  enum class Shape {
    Before,
    After,
    ExtendedRegister,
    ExtraPrefix,
    OwnLabel,
    KnownInterior,
    Continuation,
    DataTarget,
    NonExecutableTarget,
    NonExecutableSource,
    TruncatedSource,
    EmbeddedInstruction,
    NarrowDestination,
    NarrowAddress,
    InvalidBody,
    TrapBody,
    CycleBody,
    IndirectOnlyBody,
    ELF,
    MachO,
    Raw,
    Relocatable
  };
  const auto PreviousWorkers = workerThreadOverride().load();
  setWorkerThreadCount(1);
  auto RestoreWorkers =
      llvm::scope_exit([&] { setWorkerThreadCount(PreviousWorkers); });
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF})
    for (Shape Form : {Shape::Before,
                       Shape::After,
                       Shape::ExtendedRegister,
                       Shape::ExtraPrefix,
                       Shape::OwnLabel,
                       Shape::KnownInterior,
                       Shape::Continuation,
                       Shape::DataTarget,
                       Shape::NonExecutableTarget,
                       Shape::NonExecutableSource,
                       Shape::TruncatedSource,
                       Shape::EmbeddedInstruction,
                       Shape::NarrowDestination,
                       Shape::NarrowAddress,
                       Shape::InvalidBody,
                       Shape::TrapBody,
                       Shape::CycleBody,
                       Shape::IndirectOnlyBody,
                       Shape::ELF,
                       Shape::MachO,
                       Shape::Raw,
                       Shape::Relocatable}) {
      SCOPED_TRACE(static_cast<unsigned>(Form));
      SCOPED_TRACE(static_cast<unsigned>(Format));
      BinaryImage Img = peImage();
      Img.Format = Format;
      const va_t Source = Img.Entry;
      va_t Target = Img.Base + 0x20;
      if (Form == Shape::After) {
        Target = Img.Base + 0x180;
        Img.Symbols.push_back(Symbol::makeFunc(Img.Base + 0x140, 1));
        Img.Segments[0].Data[0x140] = 0xc3;
      } else if (Form == Shape::OwnLabel) {
        Target = Source + 8;
      }
      addressInsn(Img, Source, Target);
      auto *Body = Img.Segments[0].Data.data() + (Target - Img.Base);
      Body[0] = 0x31; // xor eax,eax; ret: a stackless leaf with no prologue
      Body[1] = 0xc0;
      Body[2] = 0xc3;
      auto *Code = Img.Segments[0].Data.data() + (Source - Img.Base);
      switch (Form) {
      case Shape::ExtendedRegister:
        Code[0] = 0x4c; // lea r8,[rip+disp32]
        break;
      case Shape::ExtraPrefix:
        addressInsn(Img, Source + 1, Target);
        Code[0] = 0x66;
        break;
      case Shape::KnownInterior:
        Img.KnownCodeRanges = {{Target - 16, Target + 16}};
        break;
      case Shape::Continuation:
        Img.ContinuationCodeStarts.insert(Target);
        break;
      case Shape::DataTarget:
        Img.DataInCode.insert(Target);
        break;
      case Shape::NonExecutableTarget:
      case Shape::NonExecutableSource: {
        Section Data;
        Data.Name = ".data";
        Data.VA = Form == Shape::NonExecutableTarget ? Target : Source;
        Data.Size = 8;
        Data.Flags = SegmentFlags::Readable;
        Img.Sections.insert(Img.Sections.begin(), std::move(Data));
        break;
      }
      case Shape::TruncatedSource:
        Img.Sections[0].Size = Source - Img.Base + 6;
        break;
      case Shape::EmbeddedInstruction:
        // The apparent LEA is inside movabs's immediate, not an instruction.
        addressInsn(Img, Source + 2, Target);
        Code[0] = 0x48;
        Code[1] = 0xb8;
        Code[10] = 0xc3;
        break;
      case Shape::NarrowDestination:
        Code[0] = 0x8d; // lea eax, [rip+disp32]
        Code[1] = 0x05;
        writeLE<int32_t>(Code + 2, static_cast<int32_t>(Target - (Source + 6)));
        Code[6] = 0xc3;
        break;
      case Shape::NarrowAddress:
        addressInsn(Img, Source + 1, Target);
        Code[0] = 0x67; // lea rax, [eip+disp32]
        break;
      case Shape::InvalidBody:
        Body[0] = 0x0f;
        Body[1] = 0x04;
        break;
      case Shape::TrapBody:
        Body[0] = 0xcc;
        break;
      case Shape::CycleBody:
        Body[0] = 0xeb;
        Body[1] = 0xfe;
        break;
      case Shape::IndirectOnlyBody:
        Body[0] = 0xff;
        Body[1] = 0xe0; // jmp rax: no independently known callable exit
        break;
      case Shape::ELF:
        Img.Format = BinaryFormat::ELF;
        break;
      case Shape::MachO:
        Img.Format = BinaryFormat::MachO;
        break;
      case Shape::Raw:
        Img.Format = BinaryFormat::Raw;
        break;
      case Shape::Relocatable:
        Img.IsRelocatable = true;
        break;
      default:
        break;
      }
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Img));
      const auto Functions = FuncDetector().detect(Img, Dec);
      EXPECT_EQ(hasEntry(Functions, Target),
                Form == Shape::Before || Form == Shape::After ||
                    Form == Shape::ExtendedRegister || Form == Shape::Raw ||
                    Form == Shape::ELF);
    }
}

TEST(ELFFunctionAddresses, AddressTakenBodyOwnsPaddingAndDataPointerGuesses) {
  const auto PreviousWorkers = workerThreadOverride().load();
  auto RestoreWorkers =
      llvm::scope_exit([&] { setWorkerThreadCount(PreviousWorkers); });
  for (unsigned Workers : {1u, 4u}) {
    SCOPED_TRACE(Workers);
    setWorkerThreadCount(Workers);
    auto Img = peImage();
    Img.Format = BinaryFormat::ELF;
    const va_t Target = Img.Base + 0x20;
    addressInsn(Img, Img.Entry, Target);
    // An original assembly fixture: the callable entry has no conventional
    // prologue or unwind row. Operand bytes are not INT3 padding.
    const uint8_t Body[] = {0xf3, 0x0f, 0x1e, 0xfa, // endbr64
                            0x50,                   // push rax
                            0x48, 0xb8, 0xcd, 0xcc, 0xcc, 0xcc, 0xcc,
                            0xcc, 0xcc, 0xcc, 0x48, 0xf7, 0xe2, // mul rdx
                            0x48, 0x83, 0xe0, 7,                // and rax,7
                            0x58, 0xc3};                        // pop rax; ret
    std::copy(std::begin(Body), std::end(Body),
              Img.Segments[0].Data.begin() + 0x20);
    scanPaddingBoundaries(Img);
    auto Guess = Symbol::makeFunc(Target + 18);
    Guess.IsBoundaryGuess = true; // An unrelocated data word names this row.
    Img.Symbols.push_back(Guess);
    const auto Guesses = Img.boundaryGuessFunctionStarts();
    ASSERT_TRUE(Guesses.count(Target + 15));
    ASSERT_TRUE(Guesses.count(Target + 18));
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    const auto Functions = FuncDetector().detect(Img, Dec);
    EXPECT_TRUE(hasEntry(Functions, Target));
    EXPECT_FALSE(hasEntry(Functions, Target + 15));
    EXPECT_FALSE(hasEntry(Functions, Target + 18));
    EXPECT_EQ(FuncDetector().detect(Img, Dec), Functions);
  }
}

TEST(X64FunctionAddresses, ReferencedIBTEntryCanOutgrowTheCandidateProbe) {
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF})
    for (unsigned Shape = 0; Shape != 9; ++Shape) {
      SCOPED_TRACE(Shape);
      SCOPED_TRACE(static_cast<unsigned>(Format));
      auto Img = peImage(1024);
      Img.Format = Format;
      Img.Entry = Img.Base + 0x300;
      const va_t Target = Img.Base + 0x20;
      addressInsn(Img, Img.Entry, Target);
      auto &Code = Img.Segments[0].Data;
      const uint8_t Marker[] = {0xf3, 0x0f, 0x1e, 0xfa};
      std::copy(std::begin(Marker), std::end(Marker), Code.begin() + 0x20);
      // A complete body beyond the bounded candidate probe. Discovery must
      // retain the external reference plus entry marker for the formal audit.
      std::fill(Code.begin() + 0x24, Code.begin() + 0x180, 0x90);
      Code[0x180] = 0xc3;
      switch (Shape) {
      case 1:
        Code[0x20] = 0x90; // no marker
        Code[0x21] = Code[0x22] = Code[0x23] = 0x90;
        break;
      case 2:
        Code[0x23] = 0xfb; // ENDBR32 does not mark a 64-bit entry
        break;
      case 3:
        Code[0x24] = 0x0f;
        Code[0x25] = 0x04; // marker cannot override a definite decode failure
        break;
      case 4:
        Img.KnownCodeRanges = {{Target - 8, Target + 8}};
        break;
      case 5:
        Img.DataInCode.insert(Target);
        break;
      case 6:
        Img.ContinuationCodeStarts.insert(Target);
        break;
      case 7:
      case 8:
        if (Shape == 8) {
          auto Guess = Symbol::makeFunc(Target);
          Guess.IsBoundaryGuess = true;
          Img.Symbols.push_back(Guess);
        }
        Img.Entry = Img.Base + 8;
        addressInsn(Img, Img.Entry, Target); // the namer's own possible label
        addressInsn(Img, Img.Base + 0x300, Img.Base + 8);
        break;
      }
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Img));
      EXPECT_EQ(hasEntry(FuncDetector().detect(Img, Dec), Target), Shape == 0);
    }
}

TEST(X64FunctionAddresses,
     InteriorRefutationPreservesIndependentEntryEvidence) {
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF})
    for (unsigned Evidence = 0; Evidence != 5; ++Evidence) {
      SCOPED_TRACE(static_cast<unsigned>(Format));
      SCOPED_TRACE(Evidence);
      auto Img = peImage();
      Img.Format = Format;
      auto &Code = Img.Segments[0].Data;
      const va_t Interior = Img.Entry + 10;
      // movabs rax,imm64; xor eax,eax; ret. A padding guess starts exactly
      // where the image entry falls through, without being a separate call.
      Code[0x100] = 0x48;
      Code[0x101] = 0xb8;
      Code[0x102] = 0xcd;
      Code[0x10a] = 0x31;
      Code[0x10b] = 0xc0;
      Code[0x10c] = 0xc3;
      scanPaddingBoundaries(Img);
      ASSERT_TRUE(Img.boundaryGuessFunctionStarts().count(Interior));
      switch (Evidence) {
      case 1:
        Img.Symbols.push_back(Symbol::makeFunc(Interior, 3));
        break;
      case 2:
        Code[0x20] = 0xe8;
        writeLE<int32_t>(Code.data() + 0x21, Interior - (Img.Base + 0x25));
        Code[0x25] = 0xc3;
        break;
      case 3:
        addressInsn(Img, Img.Base + 0x20, Interior);
        Img.Symbols.push_back(Symbol::makeFunc(Img.Base + 0x20, 8));
        break;
      case 4:
        Code[0x20] = 0xc3;
        writeLE<uint64_t>(Code.data() + 0x40, Interior);
        Img.CodePtrRelocSlots.insert(Img.Base + 0x40);
        Img.DataInCode.insert(Img.Base + 0x40);
        break;
      }
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Img));
      const auto Found = FuncDetector().detect(Img, Dec);
      EXPECT_EQ(hasEntry(Found, Interior), Evidence != 0);
      EXPECT_EQ(FuncDetector().detect(Img, Dec), Found);
    }
}

TEST(X64FunctionAddresses, UnprovedControlFlowCannotRefuteAnEntry) {
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF})
    for (unsigned Transfer = 0; Transfer != 4; ++Transfer) {
      SCOPED_TRACE(static_cast<unsigned>(Format));
      SCOPED_TRACE(Transfer);
      auto Img = peImage();
      Img.Format = Format;
      auto &Code = Img.Segments[0].Data;
      const va_t Target = Img.Entry + 10;
      Code[0x10a] = 0x31;
      Code[0x10b] = 0xc0;
      Code[0x10c] = 0xc3;
      auto Guess = Symbol::makeFunc(Target);
      Guess.IsBoundaryGuess = true;
      Img.Symbols.push_back(Guess);
      switch (Transfer) {
      case 0:
        Code[0x100] = 0xeb;
        Code[0x101] = 8; // a possible tail jump
        break;
      case 1:
        std::fill(Code.begin() + 0x100, Code.begin() + 0x105, 0x90);
        Code[0x105] = 0xe8;
        writeLE<int32_t>(Code.data() + 0x106, (Img.Base + 0x20) - Target);
        Code[0x20] = 0xc3; // the caller's return behavior is not a proof
        break;
      case 2:
        Code[0x100] = 0xc3; // the guess is unreachable from this entry
        break;
      case 3:
        Code[0x100] = 0xff;
        Code[0x101] = 0xe0; // unresolved computed branch
        break;
      }
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Img));
      EXPECT_TRUE(hasEntry(FuncDetector().detect(Img, Dec), Target));
    }
}

TEST(X64FunctionAddresses, PieceBoundariesAndCancellationPreserveOneSweep) {
  const auto PreviousWorkers = workerThreadOverride().load();
  auto RestoreWorkers =
      llvm::scope_exit([&] { setWorkerThreadCount(PreviousWorkers); });
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    constexpr size_t TextSize = 512 * 1024;
    BinaryImage Original = peImage(TextSize);
    Original.Format = Format;
    Original.Segments[0].Data[0x100] = 0xc3;
    std::vector<va_t> Expected, Embedded;
    for (size_t Boundary = 16 * 1024; Boundary < TextSize;
         Boundary += 16 * 1024) {
      const va_t Source = Original.Base + Boundary;
      const va_t Fake = Original.Base + 0x200 + Embedded.size() * 8;
      const va_t Real = Original.Base + 0x400 + Expected.size() * 8;
      Embedded.push_back(Fake);
      Expected.push_back(Real);
      Original.Segments[0].Data[Fake - Original.Base] = 0xc3;
      Original.Segments[0].Data[Real - Original.Base] = 0xc3;
      // Every nominal piece boundary lands in a movabs immediate spelling LEA.
      // Joining must discard that independent piece's false instruction.
      addressInsn(Original, Source, Fake);
      auto *At = Original.Segments[0].Data.data() + Boundary;
      At[-2] = 0x48;
      At[-1] = 0xb8;
      At[8] = 0xc3;
      addressInsn(Original, Source + 16, Real);
      Original.Symbols.push_back(Symbol::makeFunc(Source - 2, 11));
      Original.Symbols.push_back(Symbol::makeFunc(Source + 16, 8));
      // A nested sweep ending inside an instruction may not fabricate a LEA.
      Original.KnownCodeRanges.emplace_back(Source - 2, Source + 5);
    }
    std::vector<std::pair<va_t, std::string>> Baseline;
    for (unsigned Workers : {1u, 2u, 4u, 16u}) {
      SCOPED_TRACE(Workers);
      setWorkerThreadCount(Workers);
      auto Img = Original;
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Img));
      Dec.setDetail(false);
      Dec.setText(false);
      FuncDetector Detector;
      std::atomic<unsigned> Checks{0};
      EXPECT_FALSE(Detector.detect(Img, Dec,
                                   [&] { return Checks.fetch_add(1) >= 512; }));
      EXPECT_TRUE(Img.VerifiedFunctionEntries.empty());
      EXPECT_FALSE(Dec.detailEnabled());
      EXPECT_FALSE(Dec.textEnabled());
      const auto Functions = Detector.detect(Img, Dec);
      for (va_t Target : Expected)
        EXPECT_TRUE(hasEntry(Functions, Target));
      for (va_t Target : Embedded)
        EXPECT_FALSE(hasEntry(Functions, Target));
      if (Workers == 1)
        Baseline = Functions;
      else
        EXPECT_EQ(Functions, Baseline);
    }
  }
}

} // namespace
