//===- FunctionDiscoveryAlignmentTests.cpp - Candidate address checks -----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/intrinsics/AArch64/AArch64PAuth.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/loader/ExceptionInfo.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/MachO/MachOLoaderUtils.h"
#include "neverd/support/BinaryEncoding.h"
#include "neverd/support/ISAEncoding.h"
#include "neverd/support/Parallel.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <utility>
#include <vector>

using namespace neverd;

namespace {

Segment executableSegment(va_t Address, llvm::ArrayRef<uint8_t> Bytes) {
  Segment Seg;
  Seg.VA = Address;
  Seg.Size = Bytes.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data.assign(Bytes.begin(), Bytes.end());
  return Seg;
}

BinaryImage x64UnwindTailAdapter(bool Large, va_t Target) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.Entry = 0x1040;
  std::vector<uint8_t> Code(256, 0xcc);
  std::vector<uint8_t> Body =
      Large ? std::vector<uint8_t>{0x48, 0x81, 0xec, 0x28, 0x01, 0, 0}
            : std::vector<uint8_t>{0x48, 0x83, 0xec, 0x28};
  const size_t Prologue = Body.size();
  Body.push_back(0xe8);
  Body.resize(Body.size() + 4);
  writeLE<int32_t>(Body.data() + Prologue + 1,
                   0x10c0 - (Img.Entry + Body.size()));
  if (Large)
    Body.insert(Body.end(), {0x4d, 0x8b, 0xc8}); // mov r9,r8
  else
    Body.insert(Body.end(), {0x8b, 0xc8}); // mov ecx,eax
  const size_t Restore = Body.size();
  const std::vector<uint8_t> Frame(Body.begin(), Body.begin() + Prologue);
  Body.insert(Body.end(), Frame.begin(), Frame.end());
  Body[Restore + 2] = 0xc4; // add rsp, same allocation
  Body.push_back(0xe9);
  Body.resize(Body.size() + 4);
  writeLE<int32_t>(Body.data() + Body.size() - 4,
                   Target - (Img.Entry + Body.size()));
  const uint8_t Leaf[] = {0x81, 0xe1, 0, 3, 0, 0, 0x8b, 0xc1, 0xc3};
  std::copy(std::begin(Leaf), std::end(Leaf), Code.begin() + Target - 0x1000);
  std::copy(Body.begin(), Body.end(), Code.begin() + 0x40);
  Code[0xc0] = 0xc3;
  Img.Segments.push_back(executableSegment(0x1000, Code));
  Img.Symbols.push_back(Symbol::makeFunc(0x10c0, 1));
  ExceptionFunction F;
  F.CodeRange = {Img.Entry, Img.Entry + Body.size()};
  F.Encoding = ExceptionEncoding::X64UnwindV1;
  F.UnwindVersion = 1;
  F.PrologueSize = Prologue;
  UnwindOperation Allocation;
  Allocation.Kind = Large ? UnwindOperationKind::AllocateLarge
                          : UnwindOperationKind::AllocateSmall;
  Allocation.CodeOffset = Prologue;
  Allocation.StackOffset = Large ? 0x128 : 0x28;
  F.UnwindOperations.push_back(Allocation);
  Img.KnownCodeRanges.emplace_back(F.CodeRange.Begin, F.CodeRange.End);
  Img.ExceptionMetadata.PrimaryFunctionEntries.push_back(Img.Entry);
  Img.ExceptionMetadata.Functions.push_back(F);
  return Img;
}

TEST(FunctionDiscoveryAlignment,
     RestoredUnwindAdaptersDiscoverLeafTailEntries) {
  const auto PreviousWorkers = workerThreadOverride().load();
  auto RestoreWorkers =
      llvm::scope_exit([&] { setWorkerThreadCount(PreviousWorkers); });
  for (unsigned Workers : {1u, 4u})
    for (bool Large : {false, true})
      for (va_t Target : {0x1020, 0x1080}) {
        SCOPED_TRACE(::testing::Message()
                     << Workers << ':' << Large << ':' << Target);
        setWorkerThreadCount(Workers);
        auto Img = x64UnwindTailAdapter(Large, Target);
        if (Large) {
          auto &F = Img.ExceptionMetadata.Functions.front();
          F.Encoding = ExceptionEncoding::X64UnwindV2;
          F.UnwindVersion = 2;
        }
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Img));
        const auto Found = FuncDetector().detect(Img, Dec);
        EXPECT_TRUE(llvm::any_of(
            Found, [=](const auto &F) { return F.first == Target; }));
        EXPECT_TRUE(Img.VerifiedFunctionEntries.count(Target));
        // Entry publication must remain stable on the next discovery pass.
        EXPECT_EQ(FuncDetector().detect(Img, Dec), Found);
      }
}

TEST(FunctionDiscoveryAlignment, UnprovedUnwindTailAdaptersStayUnsplit) {
  for (unsigned Mutation = 0; Mutation != 22; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Img = x64UnwindTailAdapter(false, 0x1080);
    auto &F = Img.ExceptionMetadata.Functions.front();
    auto &Code = Img.Segments.front().Data;
    const size_t Last = F.CodeRange.End - 0x1000 - 5;
    switch (Mutation) {
    case 0:
      F.Encoding = ExceptionEncoding::Unknown;
      break;
    case 1:
      F.ParseStatus = ExceptionParseStatus::Malformed;
      break;
    case 2:
      F.ParseStatus = ExceptionParseStatus::Partial;
      break;
    case 3:
      F.Kind = RuntimeFunctionKind::Fragment;
      break;
    case 4:
      F.UnwindFlags = 4;
      break;
    case 5:
      F.FrameRegister = 5;
      break;
    case 6:
      F.UnwindOperations.front().StackOffset = 32;
      break;
    case 7:
      F.UnwindOperations.front().CodeOffset = 3;
      break;
    case 8:
      F.UnwindOperations.push_back(F.UnwindOperations.front());
      break;
    case 9:
      Code[0x4e] = 0x20;
      break; // restore a different stack size
    case 10:
      Code[0x4a] = 0xe0;
      break; // mov esp,eax
    case 11:
      Code[0x4a] = 0xd8;
      break; // mov ebx,eax
    case 12:
      Code[0x49] = 0x74;
      Code[0x4a] = 0;
      break; // conditional branch
    case 13:
      Code[0x49] = 0x89;
      Code[0x4a] = 1;
      break; // mov [rcx],eax
    case 14:
      Img.Symbols.front().Name = "abort";
      break;
    case 15:
      writeLE<int32_t>(Code.data() + Last + 1, 0x1049 - (0x1000 + Last + 5));
      break;
    case 16:
      Img.KnownCodeRanges.push_back({0x1070, 0x1090});
      break;
    case 17:
      Img.ContinuationCodeStarts.insert(0x1080);
      break;
    case 18:
      Code[0x80] = 0xeb;
      Code[0x81] = 0xfe;
      break; // no callable exit
    case 19:
      F.FragmentRanges.push_back({0x1080, 0x1089});
      break;
    case 20:
      Code[0x43] = Code[0x4e] = 8;
      F.UnwindOperations.front().StackOffset = 8;
      break;
    case 21:
      Img.ExceptionMetadata.Functions.clear();
      break;
    }
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    const auto Found = FuncDetector().detect(Img, Dec);
    EXPECT_FALSE(llvm::any_of(
        Found, [](const auto &Entry) { return Entry.first == 0x1080; }));
    EXPECT_FALSE(Img.VerifiedFunctionEntries.count(0x1080));
  }
}

TEST(FunctionDiscoveryAlignment, UnwindTailRequiresAnExecutableCompleteBody) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Img = x64UnwindTailAdapter(false, 0x1080);
    Section Code;
    Code.VA = 0x1000;
    Code.Size = 0x100;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Section Data;
    Data.VA = Mutation == 0 ? 0x1049 : 0x1080;
    Data.Size = Mutation == 0 ? 2 : 9;
    Data.Flags = SegmentFlags::Readable;
    if (Mutation < 2) {
      Img.Sections.push_back(Data);
      Img.Sections.push_back(Code);
    } else if (Mutation == 2)
      Img.Segments.front().Data.resize(0x50); // truncated terminal displacement
    else if (Mutation == 3)
      Img.ExceptionMetadata.Functions.front().CodeRange.End -= 1;
    else
      Img.IsRelocatable = true;
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    const auto Found = FuncDetector().detect(Img, Dec);
    EXPECT_FALSE(llvm::any_of(
        Found, [](const auto &Entry) { return Entry.first == 0x1080; }));
  }
}

// A long `.text` is swept for call targets in pieces on the worker threads
// and the pieces joined as one sweep would have gone.  Random bytes decode to
// every length of instruction and to bytes that do not decode at all, so the
// pieces start in the middle of instructions; the symbols start wherever
// they like, some where the long sweep never lands and some ending inside an
// instruction.  What the detector finds must not depend on any of that.
TEST(FuncDetectorCoverage, SweepsInPiecesExactlyAsInOnePiece) {
  const auto PreviousWorkers = workerThreadOverride().load();
  auto RestoreWorkers =
      llvm::scope_exit([&] { setWorkerThreadCount(PreviousWorkers); });
  constexpr va_t TextVA = 0x401000;
  constexpr size_t TextSize = 512 * 1024;
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Base = 0x400000;
  Img.Entry = TextVA;

  Segment Mapping;
  Mapping.VA = TextVA;
  Mapping.Size = TextSize;
  Mapping.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Mapping.Data.resize(TextSize);
  uint64_t State = 0x9E3779B97F4A7C15ull;
  auto Next = [&] {
    State ^= State << 13, State ^= State >> 7, State ^= State << 17;
    return State;
  };
  for (uint8_t &Byte : Mapping.Data)
    Byte = static_cast<uint8_t>(Next());
  // Real calls among the noise: `push rbp; call rel32` into the text.
  for (unsigned Call = 0; Call < 4000; ++Call) {
    const size_t At = Next() % (TextSize - 16);
    const size_t Target = Next() % (TextSize - 16) & ~size_t(15);
    Mapping.Data[At] = 0x55;
    Mapping.Data[At + 1] = 0xE8;
    const int32_t Disp = static_cast<int32_t>(static_cast<int64_t>(Target) -
                                              static_cast<int64_t>(At + 6));
    std::memcpy(Mapping.Data.data() + At + 2, &Disp, sizeof(Disp));
    std::memcpy(Mapping.Data.data() + Target, "\x55\x48\x89\xE5\x5D\xC3",
                6); // a small function there
  }
  Img.Segments.push_back(std::move(Mapping));
  Section Code;
  Code.Name = ".text";
  Code.VA = TextVA;
  Code.Size = TextSize;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(Code));
  for (unsigned Sym = 0; Sym < 500; ++Sym) {
    const va_t Start = TextVA + Next() % (TextSize - 64);
    Img.Symbols.push_back(Symbol::makeFunc(Start, 1 + Next() % 60));
  }

  // Repeated nested ranges may begin/end inside canonical instructions.
  Img.KnownCodeRanges = {{TextVA + 3, TextVA + 65539},
                         {TextVA + 3, TextVA + 65539},
                         {TextVA + 7, TextVA + 16391}};
  auto DetectWith = [&](unsigned Threads) {
    setWorkerThreadCount(Threads);
    Decoder Dec;
    EXPECT_TRUE(Dec.init(Arch::X64));
    FuncDetector Detector;
    auto Functions = Detector.detect(Img, Dec);
    setWorkerThreadCount(0);
    return Functions;
  };
  const auto OnePiece = DetectWith(1);
  EXPECT_GT(OnePiece.size(), 1000u);
  for (unsigned Workers : {2u, 4u, 16u})
    EXPECT_EQ(DetectWith(Workers), OnePiece);
}

TEST(FunctionDiscoveryAlignment, LongCalleesNeedReachableCallableExits) {
  enum class Shape {
    PrefixBoundary,
    Straight,
    Conditional,
    Trap,
    Indirect,
    Cycle,
    Truncated,
    CFGBoundary,
    Exhausted,
    EscapingBranch,
    TruncatedArm,
    ExhaustedArm,
    MixedIndirect,
    UnsupportedArm,
    PACMArm,
    ScalableCPAArm
  };
  // PTRUES remains refused because its NZCV effects are not modelled. PACM
  // and scalable ADDPT now have checked semantics and are positive cases.
  constexpr uint32_t UnsupportedPtrues = 0x2519e3e0;
  constexpr uint32_t ScalableAddpt = 0x04e20820;
  const std::pair<Arch, InstructionMode> Targets[] = {
      {Arch::X86, InstructionMode::Default},
      {Arch::X64, InstructionMode::Default},
      {Arch::ARM, InstructionMode::ARM},
      {Arch::ARM, InstructionMode::Thumb},
      {Arch::AArch64, InstructionMode::Default}};
  for (auto [Architecture, Mode] : Targets)
    for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::MachO})
      for (Shape Form :
           {Shape::PrefixBoundary, Shape::Straight, Shape::Conditional,
            Shape::Trap, Shape::Indirect, Shape::Cycle, Shape::Truncated,
            Shape::CFGBoundary, Shape::Exhausted, Shape::EscapingBranch,
            Shape::TruncatedArm, Shape::ExhaustedArm, Shape::MixedIndirect,
            Shape::UnsupportedArm, Shape::PACMArm, Shape::ScalableCPAArm}) {
        SCOPED_TRACE(testing::Message()
                     << unsigned(Architecture) << ':' << unsigned(Mode) << ':'
                     << unsigned(Format) << ':' << unsigned(Form));
        constexpr va_t Base = 0x1000, Callee = Base + 32;
        const bool X86 = Architecture == Arch::X86 || Architecture == Arch::X64;
        const bool Thumb = Mode == InstructionMode::Thumb;
        const bool A64 = Architecture == Arch::AArch64;
        if ((Form == Shape::UnsupportedArm || Form == Shape::PACMArm ||
             Form == Shape::ScalableCPAArm) &&
            !A64)
          continue;
        // Pin both the 64/65 linear-prefix and 256/257 reachable-probe edges.
        size_t Prefix = limits::kMaxVerifyInsns;
        if (Form == Shape::PrefixBoundary)
          --Prefix;
        if (Form == Shape::CFGBoundary || Form == Shape::Exhausted)
          Prefix = 256 - (Form == Shape::CFGBoundary ? 1 : 0);
        const bool Conditional =
            Form == Shape::Conditional || Form == Shape::EscapingBranch ||
            Form == Shape::TruncatedArm || Form == Shape::ExhaustedArm ||
            Form == Shape::MixedIndirect || Form == Shape::UnsupportedArm ||
            Form == Shape::PACMArm || Form == Shape::ScalableCPAArm;
        std::vector<uint8_t> Body;
        const auto Word = [&](uint32_t Value) {
          const size_t At = Body.size();
          Body.resize(At + 4);
          writeLE<uint32_t>(Body.data() + At, Value);
        };
        const auto Half = [&](uint16_t Value) {
          const size_t At = Body.size();
          Body.resize(At + 2);
          writeLE<uint16_t>(Body.data() + At, Value);
        };
        const auto Arithmetic = [&] {
          if (X86)
            Body.push_back(0x90);
          else if (Thumb)
            Half(0x3001); // adds r0, #1
          else
            Word(A64 ? 0x11000400 : 0xe2800001); // add w0/r0, #1
        };
        const auto Return = [&] {
          if (X86)
            Body.push_back(0xc3);
          else if (Thumb)
            Half(0x4770);
          else
            Word(A64 ? 0xd65f03c0 : 0xe12fff1e); // ret / bx lr
        };
        const auto Indirect = [&] {
          if (X86)
            Body.insert(Body.end(), {0xff, 0xe0}); // jmp rax/eax
          else if (Thumb)
            Half(0x4718); // bx r3
          else
            Word(A64 ? 0xd61f0060 : 0xe12fff13); // br x3 / bx r3
        };
        for (size_t I = 0; I != Prefix; ++I)
          Arithmetic();
        if (Conditional) {
          // The conditional transfer is outside the initial linear prefix on
          // every ISA. Fallthrough returns; the other arm must still be
          // checked.
          if (X86) {
            Body.insert(Body.end(), {0x0f, 0x85});
            Word(Form == Shape::EscapingBranch ? 0x10000 : 1);
          } else if (Thumb)
            Half(Form == Shape::EscapingBranch ? 0xd17f : 0xd100);
          else if (A64)
            Word(Form == Shape::EscapingBranch ? 0x54080001 : 0x54000041);
          else
            Word(Form == Shape::EscapingBranch ? 0x1a003ffe : 0x1a000000);
          Return();
        }
        if (Form == Shape::Truncated || Form == Shape::TruncatedArm)
          Body.push_back(X86 ? 0x0f : 0);
        else if (Form == Shape::Indirect || Form == Shape::MixedIndirect)
          Indirect();
        else if (Form == Shape::UnsupportedArm) {
          Word(UnsupportedPtrues);
          Return();
        } else if (Form == Shape::ScalableCPAArm) {
          Word(ScalableAddpt);
          Return();
        } else if (Form == Shape::PACMArm) {
          Word(kAArch64PACMEncoding);
          Return();
        } else if (Form == Shape::Trap) {
          if (X86)
            Body.push_back(0xcc);
          else if (Thumb)
            Half(0xde00);
          else
            Word(A64 ? 0xd4200000 : 0xe7f000f0); // brk / udf
        } else if (Form == Shape::Cycle) {
          if (X86)
            Body.insert(Body.end(), {0xeb, 0xfe});
          else if (Thumb)
            Half(0xe7fe);
          else
            Word(A64 ? 0x14000000 : 0xeafffffe); // b self
        } else {
          if (Form == Shape::ExhaustedArm)
            for (size_t I = 0; I != 256; ++I)
              Arithmetic();
          Return();
        }
        std::vector<uint8_t> Bytes(Callee - Base, 0);
        if (X86) {
          Bytes[0] = 0xe8; // call callee; ret
          writeLE<uint32_t>(Bytes.data() + 1, Callee - (Base + 5));
          Bytes[5] = 0xc3;
        } else if (Thumb) {
          writeLE<uint16_t>(Bytes.data(), 0xf000); // bl callee
          writeLE<uint16_t>(Bytes.data() + 2, 0xf80e);
          writeLE<uint16_t>(Bytes.data() + 4, 0x4770);
        } else if (A64) {
          writeLE<uint32_t>(Bytes.data(), 0x94000008); // bl callee
          writeLE<uint32_t>(Bytes.data() + 4, 0xd65f03c0);
        } else {
          writeLE<uint32_t>(Bytes.data(), 0xeb000006); // bl callee
          writeLE<uint32_t>(Bytes.data() + 4, 0xe12fff1e);
        }
        llvm::append_range(Bytes, Body);
        BinaryImage Img;
        Img.Arch = Architecture;
        Img.Bits = Architecture == Arch::X64 || A64 ? Bitness::Bits64
                                                    : Bitness::Bits32;
        Img.Mode = Mode;
        Img.Format = Format;
        Img.Entry = Base;
        Img.Segments.push_back(executableSegment(Base, Bytes));
        Img.Symbols.push_back(Symbol::makeFunc(Base, X86 || Thumb ? 6 : 8));
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Img));
        if (Form == Shape::UnsupportedArm || Form == Shape::ScalableCPAArm) {
          uint8_t Bytes[4];
          writeLE<uint32_t>(Bytes, Form == Shape::UnsupportedArm
                                       ? UnsupportedPtrues
                                       : ScalableAddpt);
          DecodedInsn I;
          ASSERT_EQ(Dec.decodeOneForLift(Bytes, sizeof(Bytes), Callee, I), 4);
          ASSERT_FALSE(I.Raw->illegal);
          std::vector<LowOp> Ops;
          if (Form == Shape::UnsupportedArm)
            EXPECT_THROW(Dec.liftToLow(I, Ops), UnliftedInstruction);
          else {
            EXPECT_NO_THROW(Dec.liftToLow(I, Ops));
            EXPECT_FALSE(Ops.empty());
          }
        }
        FuncDetector Detector;
        const auto Functions = Detector.detect(Img, Dec);
        // Callable identity needs an observed exit, not proof that all paths
        // return. An indirect frontier alongside that exit keeps its old role.
        const bool Expected =
            Form == Shape::PrefixBoundary || Form == Shape::Straight ||
            Form == Shape::Conditional || Form == Shape::CFGBoundary ||
            Form == Shape::MixedIndirect || Form == Shape::PACMArm ||
            Form == Shape::ScalableCPAArm;
        EXPECT_EQ(
            llvm::any_of(Functions,
                         [&](const auto &F) { return F.first == Callee; }),
            Expected);
        EXPECT_EQ(Img.VerifiedFunctionEntries.count(Callee) != 0, Expected);
      }
}

TEST(FunctionDiscoveryAlignment, LongCalleesVerifyUnprovenTailTargets) {
  for (bool Authenticated : {false, true})
    for (unsigned Ending = 0; Ending != 3; ++Ending) {
      SCOPED_TRACE(testing::Message() << Authenticated << ':' << Ending);
      constexpr va_t Base = 0x1000, Callee = Base + 32, Tail = Base + 512;
      std::vector<uint8_t> Bytes(Tail - Base + 2, 0xcc);
      const auto Transfer = [&](size_t At, uint8_t Opcode, va_t Target) {
        Bytes[At] = Opcode;
        writeLE<uint32_t>(Bytes.data() + At + 1, Target - (Base + At + 5));
      };
      Transfer(0, 0xe8, Callee);
      Transfer(5, 0xe8, Tail); // The tail's address is also a scan candidate.
      Bytes[10] = 0xc3;
      std::fill_n(Bytes.begin() + (Callee - Base), limits::kMaxVerifyInsns,
                  0x90);
      Transfer(Callee - Base + limits::kMaxVerifyInsns, 0xe9, Tail);
      Bytes[Tail - Base] = Ending == 0 ? 0xc3 : Ending == 1 ? 0xcc : 0xeb;
      Bytes[Tail - Base + 1] = Ending == 2 ? 0xfe : 0xcc;
      BinaryImage Img;
      Img.Arch = Arch::X64;
      Img.Bits = Bitness::Bits64;
      Img.Format = BinaryFormat::ELF;
      Img.Entry = Base;
      Img.Segments.push_back(executableSegment(Base, Bytes));
      Img.Symbols.push_back(Symbol::makeFunc(Base, 11));
      if (Authenticated)
        Img.Symbols.push_back(Symbol::makeFunc(Tail, 2));
      EXPECT_EQ(Img.hasAuthenticatedFunctionEntryAt(Tail), Authenticated);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Img));
      FuncDetector Detector;
      const auto Functions = Detector.detect(Img, Dec);
      const bool Expected = Authenticated || Ending == 0;
      EXPECT_EQ(llvm::any_of(Functions,
                             [&](const auto &F) { return F.first == Callee; }),
                Expected);
    }
}

TEST(FunctionDiscoveryAlignment, UsesVirtualAddressForEveryArchitecture) {
  struct Case {
    Arch Architecture;
    uint64_t Alignment;
    std::array<uint8_t, 4> Prologue;
  };
  const Case Cases[] = {
      {Arch::AArch64, 4, {0xff, 0x83, 0x00, 0xd1}}, // sub sp, sp, #32
      {Arch::ARM, 2, {0x10, 0xb5, 0x70, 0x47}},     // push; bx lr (Thumb)
      {Arch::X86, 1, {0x55, 0x89, 0xe5, 0xc3}},
      {Arch::X64, 1, {0x55, 0x48, 0x89, 0xe5}},
  };
  for (const Case &C : Cases) {
    for (va_t BaseOffset = 0; BaseOffset != 4; ++BaseOffset) {
      for (size_t Off = 0; Off != 8; ++Off) {
        SCOPED_TRACE(::testing::Message()
                     << "arch=" << static_cast<unsigned>(C.Architecture)
                     << " base offset=" << BaseOffset << " offset=" << Off);
        std::vector<uint8_t> Bytes(12, 0);
        std::copy(C.Prologue.begin(), C.Prologue.end(), Bytes.begin() + Off);
        const Segment Seg = executableSegment(0x1000 + BaseOffset, Bytes);
        EXPECT_EQ(checkPrologueAtOffset(Seg, Off, C.Architecture),
                  (Seg.VA + Off) % C.Alignment == 0);
      }
    }
  }
}

TEST(FunctionDiscoveryAlignment, StartsIBTImportThunksAtTheirEndbr) {
  constexpr va_t StubVA = 0xCA90;
  constexpr va_t FreeSlot = 0x31C70, MallocSlot = 0x31C78, ExitSlot = 0x31C80;
  // jmp [rip+disp32] whose opcode is at Offset, to Slot.
  const auto Jump = [&](std::vector<uint8_t> &Bytes, size_t Offset, va_t Slot) {
    const auto Disp = static_cast<int32_t>(Slot - (StubVA + Offset + 6));
    Bytes[Offset] = 0xff;
    Bytes[Offset + 1] = 0x25;
    writeLE<int32_t>(Bytes.data() + Offset + 2, Disp);
  };
  std::vector<uint8_t> Bytes(48, 0x90);
  // endbr64; bnd jmp [rip+free]
  const uint8_t Endbr64[] = {0xf3, 0x0f, 0x1e, 0xfa};
  std::copy(std::begin(Endbr64), std::end(Endbr64), Bytes.begin());
  Bytes[4] = 0xf2;
  Jump(Bytes, 5, FreeSlot);
  // endbr64; jmp [rip+malloc]
  std::copy(std::begin(Endbr64), std::end(Endbr64), Bytes.begin() + 16);
  Jump(Bytes, 20, MallocSlot);
  // mov al, 0xf2; jmp [rip+exit]: the F2 ends the previous instruction.
  Bytes[32] = 0xb0;
  Bytes[33] = 0xf2;
  Jump(Bytes, 34, ExitSlot);

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Segments.push_back(executableSegment(StubVA, Bytes));
  Img.Imports.push_back({"extern", "free", 0, FreeSlot});
  Img.Imports.push_back({"extern", "malloc", 0, MallocSlot});
  Img.Imports.push_back({"extern", "exit", 0, ExitSlot});
  scanImportThunks(Img);

  const struct {
    va_t Entry;
    uint64_t Size;
    const char *Import;
  } Thunks[] = {{StubVA, 11, "free"},
                {StubVA + 16, 10, "malloc"},
                {StubVA + 34, 6, "exit"}};
  for (const auto &Thunk : Thunks) {
    SCOPED_TRACE(Thunk.Import);
    const Import *Resolved = Img.findImportAt(Thunk.Entry);
    ASSERT_NE(Resolved, nullptr);
    EXPECT_EQ(Resolved->Name, Thunk.Import);
    const Symbol *Stub = Img.findSymbolAt(Thunk.Entry);
    ASSERT_NE(Stub, nullptr);
    EXPECT_TRUE(Stub->IsFunc);
    EXPECT_EQ(Stub->Size, Thunk.Size);
  }
  // Nothing is registered inside an instruction.
  for (va_t Inside : {StubVA + 4, StubVA + 5, StubVA + 20, StubVA + 33}) {
    EXPECT_FALSE(Img.ImportStubIndices.count(Inside)) << std::hex << Inside;
    EXPECT_EQ(Img.findSymbolAt(Inside), nullptr) << std::hex << Inside;
  }
}

TEST(FunctionDiscoveryAlignment, ImportTailJumpsDoNotSplitKnownBodies) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    for (bool RawUnwind : {false, true}) {
      SCOPED_TRACE(::testing::Message() << int(Architecture) << RawUnwind);
      constexpr va_t Base = 0x1000, Slot = 0x4000;
      std::vector<uint8_t> Bytes(64, 0xcc);
      // A branch skips an import tail jump to the same function's epilogue.
      const uint8_t Head[] = {0x48, 0x83, 0xec, 0x28, 0x74, 0x07, 0x48};
      std::copy(std::begin(Head), std::end(Head), Bytes.begin());
      const auto Jump = [&](size_t Offset) {
        Bytes[Offset] = 0xff;
        Bytes[Offset + 1] = 0x25;
        writeLE<uint32_t>(Bytes.data() + Offset + 2,
                          Architecture == Arch::X64 ? Slot - (Base + Offset + 6)
                                                    : Slot);
      };
      Jump(7);
      const uint8_t Tail[] = {0x90, 0x48, 0x83, 0xc4, 0x28, 0xc3};
      std::copy(std::begin(Tail), std::end(Tail), Bytes.begin() + 13);
      Jump(32); // A real, separate import thunk still gets discovered.
      BinaryImage Img;
      Img.Base = Base;
      Img.Arch = Architecture;
      Img.Bits = Architecture == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
      Img.Format = BinaryFormat::COFF;
      Img.Segments.push_back(executableSegment(Base, Bytes));
      Img.Imports.push_back({"crt", "free", 0, Slot});
      if (RawUnwind)
        Img.COFFPDataRecords.push_back({0, 19, 0});
      else
        Img.Symbols.push_back(Symbol::makeFunc(Base, 19));
      scanImportThunks(Img);
      EXPECT_FALSE(Img.ImportStubIndices.count(Base + 7));
      EXPECT_EQ(Img.findSymbolAt(Base + 7), nullptr);
      ASSERT_TRUE(Img.ImportStubIndices.count(Base + 32));
      ASSERT_NE(Img.findSymbolAt(Base + 32), nullptr);
    }
  }
}

TEST(FunctionDiscoveryAlignment, ArmImportPatternsRespectFunctionBodies) {
  for (Arch Architecture : {Arch::ARM, Arch::AArch64}) {
    for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::COFF}) {
      SCOPED_TRACE(::testing::Message() << int(Architecture) << int(Format));
      constexpr va_t Base = 0x1000, Slot = 0x4000;
      std::vector<uint8_t> Bytes(64, 0);
      const auto Veneer = [&](size_t Offset) {
        if (Architecture == Arch::ARM) {
          writeLE<uint32_t>(Bytes.data() + Offset,
                            0xe51ff004); // ldr pc,[pc,#-4]
          writeLE<uint32_t>(Bytes.data() + Offset + 4, Slot);
        } else {
          writeLE<uint32_t>(Bytes.data() + Offset,
                            0xf0000010); // adrp x16,0x4000
          writeLE<uint32_t>(Bytes.data() + Offset + 4,
                            0xf9400210); // ldr x16,[x16]
          writeLE<uint32_t>(Bytes.data() + Offset + 8, 0xd61f0200); // br x16
        }
      };
      Veneer(4);
      Veneer(32);
      BinaryImage Img;
      Img.Arch = Architecture;
      Img.Mode = Architecture == Arch::ARM ? InstructionMode::ARM
                                           : InstructionMode::Default;
      Img.Format = Format;
      Img.Segments.push_back(executableSegment(Base, Bytes));
      Img.Symbols.push_back(Symbol::makeFunc(Base, 24));
      Img.Imports.push_back({"extern", "free", 0, Slot});
      scanImportThunks(Img);
      EXPECT_FALSE(Img.ImportStubIndices.count(Base + 4));
      EXPECT_EQ(Img.findSymbolAt(Base + 4), nullptr);
      EXPECT_TRUE(Img.ImportStubIndices.count(Base + 32));
    }
  }
}

TEST(FunctionDiscoveryAlignment, PLTUnwindRangesDoNotHideIndividualVeneers) {
  for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
    for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
      SCOPED_TRACE(testing::Message() << int(Architecture) << ':' << Mutation);
      constexpr va_t Base = 0x1000, Slot = 0x4000, Veneer = Base + 16;
      std::vector<uint8_t> Bytes(64, 0);
      if (Architecture == Arch::X86 || Architecture == Arch::X64) {
        Bytes[16] = 0xff;
        Bytes[17] = 0x25;
        writeLE<uint32_t>(Bytes.data() + 18, Architecture == Arch::X64
                                                 ? Slot - (Veneer + 6)
                                                 : Slot);
      } else if (Architecture == Arch::ARM) {
        writeLE<uint32_t>(Bytes.data() + 16, 0xe51ff004); // ldr pc,[pc,#-4]
        writeLE<uint32_t>(Bytes.data() + 20, Slot);
      } else {
        writeLE<uint32_t>(Bytes.data() + 16, 0xf0000010); // adrp x16,0x4000
        writeLE<uint32_t>(Bytes.data() + 20, 0xf9400210); // ldr x16,[x16]
        writeLE<uint32_t>(Bytes.data() + 24, 0xd61f0200); // br x16
      }
      BinaryImage Img;
      Img.Arch = Architecture;
      Img.Bits = Architecture == Arch::X64 || Architecture == Arch::AArch64
                     ? Bitness::Bits64
                     : Bitness::Bits32;
      Img.Mode = Architecture == Arch::ARM ? InstructionMode::ARM
                                           : InstructionMode::Default;
      Img.Format = BinaryFormat::ELF;
      Img.Segments.push_back(executableSegment(Base, Bytes));
      Img.Imports.push_back({"extern", "printf", 0, Slot});
      Img.KnownCodeRanges.emplace_back(Base, Base + 64);
      if (Mutation != 2)
        Img.recordImportStubRange(Base, Mutation == 3 ? 20 : 64);
      if (Mutation == 1 || Mutation == 4) {
        auto Function = Symbol::makeFunc(Base, 64);
        if (Mutation == 4) {
          Function.Name = "explicit_function";
          Function.Origin = NameOrigin::Stated;
        }
        Img.Symbols.push_back(std::move(Function));
      }
      scanImportThunks(Img);
      EXPECT_EQ(Img.ImportStubIndices.count(Veneer), Mutation <= 1);
      EXPECT_EQ(Img.findSymbolAt(Veneer) != nullptr, Mutation <= 1);
    }
}

TEST(FunctionDiscoveryAlignment, KnownImportEntriesRemainCallable) {
  const uint8_t Code[] = {0xff, 0x25, 0xfa, 0x2f, 0, 0};
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Segments.push_back(executableSegment(0x1000, Code));
  Img.Symbols.push_back(Symbol::makeFunc(0x1000, 6));
  Img.Imports.push_back({"crt", "free", 0, 0x4000});
  scanImportThunks(Img);
  EXPECT_TRUE(Img.ImportStubIndices.count(0x1000));
  EXPECT_EQ(Img.Symbols.size(), 1u);
}

TEST(FunctionDiscoveryAlignment, CalledImportVeneerSurvivesCoarseUnwindRange) {
  constexpr va_t Base = 0x1000, Stub = Base + 16, Entry = Base + 64;
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    BinaryImage Img;
    Img.Format = BinaryFormat::ELF;
    Img.Arch = Arch::X64;
    Img.Bits = Bitness::Bits64;
    Img.Entry = Entry;
    std::vector<uint8_t> Code(80, 0xcc);
    Code[0] = 0xc3;
    Code[16] = 0xff;
    Code[17] = 0x25;
    writeLE<int32_t>(Code.data() + 18,
                     (Mutation == 4 ? 0x4008 : 0x4000) - (Stub + 6));
    Code[64] = 0xe8;
    writeLE<int32_t>(Code.data() + 65, Stub - (Entry + 5));
    Code[69] = 0xc3;
    if (Mutation == 3)
      Code[64] = 0xc3;
    Img.Segments.push_back(executableSegment(Base, Code));
    Img.KnownCodeRanges = {{Base, Base + 32}};
    Img.Symbols.push_back(Symbol::makeFunc(Base, 32));
    ExceptionFunction EH;
    EH.Encoding = ExceptionEncoding::DwarfFDE;
    EH.CodeRange = {Base, Base + 32};
    EH.Dwarf.emplace();
    Img.ExceptionMetadata.Functions.push_back(EH);
    if (Mutation != 1)
      ASSERT_TRUE(Img.recordImportStubRange(Base, 32));
    if (Mutation != 2)
      Img.Imports.push_back({"crt", "printf", 0, 0x4000});
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    const auto Found = FuncDetector().detect(Img, Dec);
    EXPECT_EQ(std::any_of(Found.begin(), Found.end(),
                          [&](const auto &F) { return F.first == Stub; }),
              Mutation == 0);
  }
}

TEST(FunctionDiscoveryAlignment, RegistersLoaderRunFunctions) {
  // sub rsp, 8; add rsp, 8; ret; ret; ret: initializers nothing in the image
  // calls, and the entry point.
  const uint8_t Code[] = {0x48, 0x83, 0xec, 0x08, 0x48, 0x83,
                          0xc4, 0x08, 0xc3, 0xc3, 0xc3};
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Segments.push_back(executableSegment(0x1000, Code));
  Img.Symbols.push_back(Symbol::makeFunc(0x1008, 1));
  Img.Entry = 0x100A;
  for (va_t Addr : {0x1000, 0x1008, 0x1009, 0x100A})
    ASSERT_TRUE(Img.recordRuntimeFunction(Addr));
  Img.DynInfo.InitAddr = 0x1000;
  Img.DynInfo.FiniArray = {0x1008, 0x1009};
  registerLoaderRunFunctions(Img);

  for (va_t Addr : {0x1000, 0x1009}) {
    const Symbol *Function = Img.findSymbolAt(Addr);
    ASSERT_NE(Function, nullptr) << std::hex << Addr;
    EXPECT_TRUE(Function->IsFunc);
    EXPECT_EQ(Function->Size, 0u);
  }
  // A function symbol already there is kept as the only one.
  EXPECT_EQ(std::count_if(Img.Symbols.begin(), Img.Symbols.end(),
                          [](const Symbol &Sym) { return Sym.Addr == 0x1008; }),
            1);
  // The entry point is the function detector's, which recovers its extent.
  EXPECT_EQ(Img.findSymbolAt(0x100A), nullptr);
}

TEST(FunctionDiscoveryAlignment, RejectsWrappedAndOutOfBufferAddresses) {
  const std::array<uint8_t, 8> Bytes = {0xff, 0x83, 0x00, 0xd1,
                                        0xff, 0x83, 0x00, 0xd1};
  const Segment Seg = executableSegment(InvalidVA - 3, Bytes);
  EXPECT_TRUE(checkPrologueAtOffset(Seg, 0, Arch::AArch64));
  EXPECT_FALSE(checkPrologueAtOffset(Seg, 4, Arch::AArch64));
  EXPECT_FALSE(checkPrologueAtOffset(Seg, Bytes.size(), Arch::AArch64));
}

TEST(FunctionDiscoveryAlignment,
     PaddingDoesNotCreateEntriesInsideAArch64Instructions) {
  // Actual bytes at hello's 0x1000004e0: str wzr; adrp x8; add x8.
  // The zero run at 4e5/4e6 previously made 4e7's 90 08 01 15 look like B.
  constexpr va_t MainVA = 0x1000004c4;
  const std::array<uint8_t, 12> Instructions = {
      0xff, 0x0f, 0x00, 0xb9, 0x08, 0x00, 0x00, 0x90, 0x08, 0x01, 0x15, 0x91};
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    BinaryImage Img;
    Img.Arch = Arch::AArch64;
    Img.Bits = Bitness::Bits64;
    Img.Format = Format;
    std::vector<uint8_t> Bytes(0x40, 0xff);
    std::copy(Instructions.begin(), Instructions.end(), Bytes.begin() + 0x1c);
    Img.Segments.push_back(executableSegment(MainVA, Bytes));
    Img.Symbols.push_back(Symbol::makeFunc(MainVA));

    scanPaddingBoundaries(Img);

    ASSERT_EQ(Img.Symbols.size(), 1u);
    EXPECT_EQ(Img.Symbols.front().Addr, MainVA);
  }
}

TEST(FunctionDiscoveryAlignment, PreservesAlignedEntriesAfterPadding) {
  struct Case {
    Arch Architecture;
    size_t Offset;
    std::array<uint8_t, 4> Prologue;
  };
  const Case Cases[] = {
      {Arch::AArch64, 8, {0xff, 0x83, 0x00, 0xd1}},
      {Arch::ARM, 6, {0x10, 0xb5, 0x70, 0x47}},
      {Arch::X86, 5, {0x55, 0x89, 0xe5, 0xc3}},
      {Arch::X64, 5, {0x55, 0x48, 0x89, 0xe5}},
  };
  for (const Case &C : Cases) {
    SCOPED_TRACE(static_cast<unsigned>(C.Architecture));
    BinaryImage Img;
    Img.Arch = C.Architecture;
    std::vector<uint8_t> Bytes(C.Offset + 4, codePaddingByte(Img.Arch));
    // The scanner deliberately skips a segment's initial padding run.
    Bytes[0] = 0x12;
    std::copy(C.Prologue.begin(), C.Prologue.end(), Bytes.begin() + C.Offset);
    Img.Segments.push_back(executableSegment(0x1000, Bytes));

    scanPaddingBoundaries(Img);

    ASSERT_EQ(Img.Symbols.size(), 1u);
    EXPECT_EQ(Img.Symbols.front().Addr, 0x1000 + C.Offset);
  }
}

// 0x40-0x4F are REX prefixes only in 64-bit code; IA-32 decodes them as
// INC/DEC.  There the byte after an int3 is more often the rest of an
// operand holding 0xcc, as in `mov [0x4048cc], eax` (a3 cc 48 40 00), than a
// function start.
TEST(FunctionDiscoveryAlignment, RexBytesBeginFunctionsOnlyIn64BitCode) {
  for (Arch Architecture : {Arch::X64, Arch::X86}) {
    SCOPED_TRACE(static_cast<unsigned>(Architecture));
    BinaryImage Img;
    Img.Arch = Architecture;
    Img.Bits = Architecture == Arch::X64 ? Bitness::Bits64 : Bitness::Bits32;
    // The scanner deliberately skips a segment's initial padding run.  Then
    // x64 `sub rsp, 0x28; ret` and IA-32 `dec eax; sub esp, 0x28; ret`.
    const uint8_t Bytes[] = {0x12, x86::kInt3, 0x48, 0x83, 0xec, 0x28, 0xc3};
    Img.Segments.push_back(executableSegment(0x1000, Bytes));

    scanPaddingBoundaries(Img);

    if (Architecture == Arch::X64) {
      ASSERT_EQ(Img.Symbols.size(), 1u);
      EXPECT_EQ(Img.Symbols.front().Addr, 0x1002u);
    } else {
      EXPECT_TRUE(Img.Symbols.empty());
    }
  }
}

// MSVC keeps string literals in the executable sections of drivers and the
// kernel, padded with int3 like the functions between them.  Their first
// bytes read as prologues -- `W` is `push rdi`, `H` a REX prefix -- but text
// holds no return, call or unconditional jump: execution entered there can
// only trap on the padding, so no function begins there.
TEST(FunctionDiscoveryAlignment, StringLiteralsInCodeBeginNoFunctions) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  std::vector<uint8_t> Bytes(0x60, x86::kInt3);
  // The scanner deliberately skips a segment's initial padding run.
  Bytes[0] = 0x12;
  const auto Put = [&](size_t Offset, std::initializer_list<uint8_t> Code) {
    std::copy(Code.begin(), Code.end(), Bytes.begin() + Offset);
  };
  // push rbp; mov rbp, rsp; pop rbp; ret
  Put(0x10, {0x55, 0x48, 0x89, 0xe5, 0x5d, 0xc3});
  // L"WIN"
  Put(0x20, {'W', 0, 'I', 0, 'N', 0, 0, 0});
  // "HYPERVISOR=": pop rcx, push rax, push r10, ... in code.
  Put(0x30, {'H', 'Y', 'P', 'E', 'R', 'V', 'I', 'S', 'O', 'R', '=', 0});
  // Text and a zero byte that are code: `imul rax, [rax], -0x34; nop; ret`,
  // whose immediate is the int3 after them.
  Put(0x40, {'H', 'k', 0, x86::kInt3, 0x90, 0xc3});
  // `jz` out of the text: execution leaves the literals.
  Put(0x50, {'H', 't', 0x7e, 0});
  Img.Segments.push_back(executableSegment(0x1000, Bytes));

  scanPaddingBoundaries(Img);

  std::vector<va_t> Guessed;
  for (const Symbol &Sym : Img.Symbols) {
    EXPECT_TRUE(Sym.IsBoundaryGuess);
    Guessed.push_back(Sym.Addr);
  }
  llvm::sort(Guessed);
  EXPECT_EQ(Guessed, (std::vector<va_t>{0x1010, 0x1040, 0x1050}));
}

// A table of pointers to such literals points at data.
TEST(FunctionDiscoveryAlignment, PointersToStringLiteralsInCodeAreNoFunctions) {
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  std::vector<uint8_t> Code(0x20, x86::kInt3);
  // L"WIN", then `push rbp; mov rbp, rsp; pop rbp; ret`.
  const std::array<uint8_t, 8> Literal = {'W', 0, 'I', 0, 'N', 0, 0, 0};
  const std::array<uint8_t, 6> Function = {0x55, 0x48, 0x89, 0xe5, 0x5d, 0xc3};
  std::copy(Literal.begin(), Literal.end(), Code.begin());
  std::copy(Function.begin(), Function.end(), Code.begin() + 0x10);
  Img.Segments.push_back(executableSegment(0x1000, Code));
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 16;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(16);
  writeLE<uint64_t>(Data.Data.data(), 0x1000);
  writeLE<uint64_t>(Data.Data.data() + 8, 0x1010);
  Img.Segments.push_back(std::move(Data));

  scanDataFuncPointers(Img);

  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_EQ(Img.Symbols.front().Addr, 0x1010u);
}

TEST(FunctionDiscoveryAlignment, DataPointersRejectMisalignedAArch64Entries) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  std::vector<uint8_t> Bytes(16, 0xff);
  writeLE<uint32_t>(Bytes.data() + 1, 0xd10083ffu);
  writeLE<uint32_t>(Bytes.data() + 8, 0xd10083ffu);
  Img.Segments.push_back(executableSegment(0x1000, Bytes));
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 16;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(16);
  writeLE<uint64_t>(Data.Data.data(), 0x1001);
  writeLE<uint64_t>(Data.Data.data() + 8, 0x1008);
  Img.Segments.push_back(std::move(Data));

  scanDataFuncPointers(Img);

  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_EQ(Img.Symbols.front().Addr, 0x1008u);
}

TEST(FunctionDiscoveryAlignment, RawDataPointerCandidatesRemainGuesses) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
    for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64})
      for (unsigned Existing = 0; Existing != 4; ++Existing)
        for (unsigned RelocatedSlot = 0; RelocatedSlot != 3; ++RelocatedSlot) {
          SCOPED_TRACE(::testing::Message()
                       << unsigned(Format) << ' ' << unsigned(Target) << ' '
                       << Existing << ' ' << RelocatedSlot);
          BinaryImage Img;
          Img.Format = Format;
          Img.Arch = Target;
          Img.Bits = Target == Arch::X86 || Target == Arch::ARM
                         ? Bitness::Bits32
                         : Bitness::Bits64;
          std::vector<uint8_t> Code(16, 0x90);
          if (Target == Arch::AArch64)
            writeLE<uint32_t>(Code.data(), 0xd10083ff);
          else if (Target == Arch::ARM) {
            Img.Mode = InstructionMode::Thumb;
            Code = {0x10, 0xb5, 0x70, 0x47};
          } else if (Target == Arch::X64) {
            Code[0] = 0x55;
            Code[1] = 0x48;
            Code[2] = 0x89;
            Code[3] = 0xe5;
          } else {
            Code[0] = 0x55;
            Code[1] = 0x89;
            Code[2] = 0xe5;
          }
          Img.Segments.push_back(executableSegment(0x1000, Code));
          Segment Data;
          Data.VA = 0x2000;
          Data.Flags = SegmentFlags::Readable;
          const unsigned Width = Img.getPointerSize();
          Data.Size = 2 * Width;
          Data.Data.resize(Data.Size);
          for (unsigned Offset : {0u, Width}) {
            const va_t Value = Target == Arch::ARM ? 0x1001 : 0x1000;
            if (Width == 4)
              writeLE<uint32_t>(Data.Data.data() + Offset, Value);
            else
              writeLE<uint64_t>(Data.Data.data() + Offset, Value);
          }
          Img.Segments.push_back(std::move(Data));
          if (Existing) {
            auto Candidate = Symbol::makeFunc(0x1000);
            Candidate.IsBoundaryGuess = Existing == 1;
            if (Existing == 2) {
              Candidate.Name = "stated_function";
              Candidate.Origin = NameOrigin::Stated;
            }
            Img.Symbols.push_back(Candidate);
          }
          if (RelocatedSlot)
            Img.CodePtrRelocSlots.insert(0x2000 + (RelocatedSlot - 1) * Width);

          scanDataFuncPointers(Img);

          ASSERT_EQ(Img.Symbols.size(), 1u);
          EXPECT_EQ(Img.Symbols.front().Addr, 0x1000u);
          EXPECT_EQ(Img.Symbols.front().IsBoundaryGuess,
                    Existing < 2 && !RelocatedSlot);
          // A later scan can learn the slot's relocation independently of
          // whether its earlier untyped value or padding created the guess.
          Img.CodePtrRelocSlots.insert(0x2000);
          scanDataFuncPointers(Img);
          EXPECT_FALSE(Img.Symbols.front().IsBoundaryGuess);
        }
}

TEST(FunctionDiscoveryAlignment, DebugCodeReferencesAreNotFunctionEntries) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    for (Arch Target : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
      for (const char *Name : {".debug_info", ".debug$S", ".zdebug_info",
                               "__debug_info", ".stab", ".rdata"}) {
        for (bool Sections : {false, true}) {
          SCOPED_TRACE(Name);
          SCOPED_TRACE(static_cast<unsigned>(Target));
          SCOPED_TRACE(Sections);
          SCOPED_TRACE(static_cast<unsigned>(Format));
          BinaryImage Img;
          Img.Arch = Target;
          Img.Bits = Target == Arch::X86 || Target == Arch::ARM
                         ? Bitness::Bits32
                         : Bitness::Bits64;
          Img.Format = Format;
          if (Target == Arch::ARM)
            Img.Mode = InstructionMode::Thumb;
          // MOV at a call-return PC passes the x86 prologue heuristic. AArch64
          // uses a plausible SP adjustment for the same false candidate.
          std::vector<uint8_t> Code(16, 0x90);
          if (Target == Arch::AArch64)
            writeLE<uint32_t>(Code.data(), 0xd10083ffu);
          else if (Target == Arch::ARM)
            Code = {0x10, 0xb5, 0x70, 0x47};
          else {
            Code[0] = 0x8b;
            Code[1] = 0xc1;
            Code[2] = 0xc3;
          }
          Img.Segments.push_back(executableSegment(0x1000, Code));
          Segment Data;
          Data.Name = Name;
          Data.VA = 0x2000;
          Data.Size = 8;
          Data.Flags = SegmentFlags::Readable;
          Data.Data.resize(8);
          writeLE<uint64_t>(Data.Data.data(),
                            Target == Arch::ARM ? 0x1001 : 0x1000);
          Img.Segments.push_back(Data);
          if (Sections) {
            Section Sec;
            Sec.Name = Name;
            Sec.VA = Data.VA;
            Sec.Size = Data.Size;
            Sec.Flags = Data.Flags;
            Img.Sections.push_back(Sec);
          }
          scanDataFuncPointers(Img);
          EXPECT_EQ(Img.Symbols.size(),
                    std::string(Name) == ".rdata" ? 1u : 0u);
        }
      }
    }
  }
}

TEST(FunctionDiscoveryAlignment,
     PreservesTaggedThumbPointersAndOddDataSymbols) {
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Mode = InstructionMode::Thumb;
  Img.Bits = Bitness::Bits32;
  const std::array<uint8_t, 4> Code = {0x10, 0xb5, 0x70, 0x47};
  Img.Segments.push_back(executableSegment(0x1002, Code));
  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 4;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(4);
  writeLE<uint32_t>(Data.Data.data(), 0x1003);
  Img.Segments.push_back(std::move(Data));
  Symbol Label;
  Label.Name = "odd_data";
  Label.Addr = 0x2001;
  Img.Symbols.push_back(Label);

  scanDataFuncPointers(Img);

  ASSERT_EQ(Img.Symbols.size(), 2u);
  EXPECT_EQ(Img.Symbols[0].Addr, 0x2001u);
  EXPECT_EQ(Img.Symbols[0].Name, "odd_data");
  EXPECT_FALSE(Img.Symbols[0].IsFunc);
  EXPECT_EQ(Img.Symbols[1].Addr, 0x1002u);
  EXPECT_TRUE(Img.Symbols[1].IsFunc);
}

TEST(FunctionDiscoveryAlignment,
     DataPrologueCandidatesKeepExactOwnershipAndExistingSymbols) {
  for (BinaryFormat Format :
       {BinaryFormat::MachO, BinaryFormat::ELF, BinaryFormat::COFF}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    BinaryImage Img;
    Img.Arch = Arch::AArch64;
    Img.Bits = Bitness::Bits64;
    Img.Format = Format;
    std::vector<uint8_t> Bytes(0x60, 0);
    for (size_t Offset : {4u, 8u, 12u, 16u, 32u})
      writeLE<uint32_t>(Bytes.data() + Offset, 0xd10083ffu);
    Img.Segments.push_back(executableSegment(0x1000, Bytes));

    Section Code;
    Code.VA = 0x1000;
    Code.Size = 14;
    Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
    Code.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
    Img.Sections.push_back(Code);
    Section Constants;
    Constants.VA = Code.VA + Code.Size;
    Constants.Size = Bytes.size() - Code.Size;
    Constants.Flags = SegmentFlags::Readable;
    Img.Sections.push_back(Constants);

    Symbol Existing;
    Existing.Name = "existing_data_label";
    Existing.Addr = 0x1004;
    Img.Symbols.push_back(Existing);
    Img.KnownCodeRanges.push_back({0x1010, 0x1014});
    // Includes a duplicate, an instruction crossing a code/data boundary,
    // a claimed body, non-code prologue bytes, and an unmapped target.
    const uint64_t Targets[] = {0x1004, 0x1008, 0x1008, 0x100c,
                                0x1010, 0x1020, 0x2000};
    Segment Data;
    Data.VA = 0x3000;
    Data.Size = sizeof(Targets);
    Data.Flags = SegmentFlags::Readable;
    Data.Data.resize(sizeof(Targets));
    for (size_t I = 0; I < std::size(Targets); ++I)
      writeLE<uint64_t>(Data.Data.data() + I * sizeof(uint64_t), Targets[I]);
    Img.Segments.push_back(std::move(Data));

    scanDataFuncPointers(Img);

    ASSERT_EQ(Img.Symbols.size(), 2u);
    EXPECT_EQ(Img.Symbols[0].Addr, Existing.Addr);
    EXPECT_EQ(Img.Symbols[0].Name, Existing.Name);
    EXPECT_FALSE(Img.Symbols[0].IsFunc);
    EXPECT_EQ(Img.Symbols[1].Addr, 0x1008u);
    EXPECT_TRUE(Img.Symbols[1].IsFunc);
    scanDataFuncPointers(Img);
    EXPECT_EQ(Img.Symbols.size(), 2u);
  }
}

TEST(FunctionDiscoveryAlignment,
     MachOFunctionStartsSuppressRawMetadataPointerGuessing) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.MachOHasFunctionStarts = true;

  std::vector<uint8_t> Bytes(16, 0);
  writeLE<uint32_t>(Bytes.data(), 0xd10083ffu);
  writeLE<uint32_t>(Bytes.data() + 8, 0xd10083ffu);
  Img.Segments.push_back(executableSegment(0x1000, Bytes));
  Img.Symbols.push_back(Symbol::makeFunc(0x1000));

  // Models two adjacent 32-bit relative metadata fields which, when read as
  // one untyped 64-bit word, happen to spell an interior code address.
  Segment Metadata;
  Metadata.VA = 0x2000;
  Metadata.Size = 8;
  Metadata.Flags = SegmentFlags::Readable;
  Metadata.Data.resize(8);
  writeLE<uint64_t>(Metadata.Data.data(), 0x1008);
  Img.Segments.push_back(std::move(Metadata));

  scanDataFuncPointers(Img);

  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_EQ(Img.Symbols.front().Addr, 0x1000u);
}

TEST(FunctionDiscoveryAlignment,
     MachOFunctionStartsSuppressPaddingBoundaryGuesses) {
  // Rust's x86-64 macOS dylib: `call` with an int3 byte in its displacement,
  // then `mov rax, [rbp-0x40]`.  The byte after the int3 is the next one of
  // the displacement, and it reads as a prologue (`ff`).
  const std::array<uint8_t, 14> Body = {0x55, 0x48, 0x89, 0xe5, 0xe8,
                                        0x9a, 0xcc, 0xff, 0xff, 0x48,
                                        0x8b, 0x45, 0xc0, 0xc3};
  for (const bool FunctionStarts : {false, true}) {
    SCOPED_TRACE(FunctionStarts);
    BinaryImage Img;
    Img.Arch = Arch::X64;
    Img.Bits = Bitness::Bits64;
    Img.Format = BinaryFormat::MachO;
    Img.MachOHasFunctionStarts = FunctionStarts;
    Img.Segments.push_back(executableSegment(0x1000, Body));
    Img.Symbols.push_back(Symbol::makeFunc(0x1000));

    scanPaddingBoundaries(Img);

    // Without the command nothing bounds the function, and the guess is what
    // the scan has; with it, the bytes belong to the function it lists.
    ASSERT_EQ(Img.Symbols.size(), FunctionStarts ? 1u : 2u);
    if (!FunctionStarts)
      EXPECT_EQ(Img.Symbols.back().Addr, 0x1007u);
  }
}

TEST(FunctionDiscoveryAlignment,
     MachOFunctionStartsAcceptZeroBasedLinkedImages) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::MachO;
  Img.Segments.push_back(executableSegment(0, std::vector<uint8_t>(32)));

  // LC_FUNCTION_STARTS deltas are relative to the Mach header VA. A linked
  // dylib may map that header at VA zero; zero is an address here, not a
  // missing-base sentinel.
  std::vector<uint8_t> File(16, 0);
  File[8] = 4;
  File[9] = 0;
  macho_loader::FunctionStartsInfo Info;
  Info.DataOff = 8;
  Info.DataSize = 2;

  macho_loader::parseFunctionStarts(File.data(), File.size(), Info, 0, Img);

  ASSERT_TRUE(Img.MachOHasFunctionStarts);
  ASSERT_EQ(Img.Symbols.size(), 1u);
  EXPECT_EQ(Img.Symbols.front().Addr, 4u);
  EXPECT_TRUE(Img.Symbols.front().IsFunc);
}

TEST(FunctionDiscoveryAlignment, CoffFuncLoadSkipsPaddingAndDataScans) {
  std::vector<uint8_t> Text(16, 0xcc);
  Text[8] = 0x55;
  Text[9] = 0x48;
  Text[10] = 0x89;
  Text[11] = 0xe5;
  Text[12] = 0xc3;

  BinaryImage WithPdata;
  WithPdata.Arch = Arch::X64;
  WithPdata.Bits = Bitness::Bits64;
  WithPdata.Format = BinaryFormat::COFF;
  WithPdata.Segments.push_back(executableSegment(0x1000, Text));
  WithPdata.KnownCodeRanges.push_back({0x1000, 0x1008});
  WithPdata.LoadOnlyFunctionEntries.insert(0x1000);
  ExceptionFunction Owned;
  Owned.CodeRange = ExceptionAddressRange{0x1000, 0x1008};
  WithPdata.ExceptionMetadata.Functions.push_back(Owned);
  WithPdata.Symbols.push_back(Symbol::makeFunc(0x1000, 8));
  const size_t Before = WithPdata.Symbols.size();
  runPostLoadDiscovery(WithPdata, "coff-pdata-func");
  EXPECT_EQ(WithPdata.Symbols.size(), Before)
      << "--func pdata load must not walk padding or data pointers";
}

TEST(FunctionDiscoveryAlignment, CoffFullLoadStillScansDataFuncPointers) {
  std::vector<uint8_t> Text(16, 0xcc);
  Text[0] = 0x55;
  Text[1] = 0x48;
  Text[2] = 0x89;
  Text[3] = 0xe5;
  Text[4] = 0xc3;
  Text[8] = 0x55;
  Text[9] = 0x48;
  Text[10] = 0x89;
  Text[11] = 0xe5;
  Text[12] = 0xc3;

  BinaryImage WithPdata;
  WithPdata.Arch = Arch::X64;
  WithPdata.Bits = Bitness::Bits64;
  WithPdata.Format = BinaryFormat::COFF;
  WithPdata.Segments.push_back(executableSegment(0x1000, Text));
  WithPdata.KnownCodeRanges.push_back({0x1000, 0x1008});
  ExceptionFunction Owned;
  Owned.CodeRange = ExceptionAddressRange{0x1000, 0x1008};
  WithPdata.ExceptionMetadata.Functions.push_back(Owned);
  WithPdata.Symbols.push_back(Symbol::makeFunc(0x1000, 8));

  Segment Data;
  Data.VA = 0x2000;
  Data.Size = 8;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(8);
  writeLE<uint64_t>(Data.Data.data(), 0x1008);
  WithPdata.Segments.push_back(std::move(Data));

  runPostLoadDiscovery(WithPdata, "coff-pdata-full");
  EXPECT_NE(std::find_if(WithPdata.Symbols.begin(), WithPdata.Symbols.end(),
                         [](const Symbol &Sym) {
                           return Sym.IsFunc && Sym.Addr == 0x1008;
                         }),
            WithPdata.Symbols.end())
      << "full-image PE still discovers data-pointer callees outside pdata";
}

TEST(FunctionDiscoveryAlignment, FuncLoadNamesImportThunkWithoutScan) {
  // `--func` skips scanImportThunks.  A call to an IAT veneer must still
  // resolve `_CxxThrowException` so HighC can print `throw`.
  constexpr va_t Thunk = 0x140001020;
  constexpr va_t IAT = 0x140003000;
  const int32_t Disp = static_cast<int32_t>(IAT - (Thunk + 6));
  std::vector<uint8_t> Text(6, 0xcc);
  Text[0] = 0xff;
  Text[1] = 0x25;
  Text[2] = static_cast<uint8_t>(Disp);
  Text[3] = static_cast<uint8_t>(Disp >> 8);
  Text[4] = static_cast<uint8_t>(Disp >> 16);
  Text[5] = static_cast<uint8_t>(Disp >> 24);

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::COFF;
  Img.LoadOnlyFunctionEntries.insert(0x140001000);
  Img.Segments.push_back(executableSegment(Thunk, Text));
  Import Imp;
  Imp.Name = "_CxxThrowException";
  Imp.IATAddr = IAT;
  Img.Imports.push_back(std::move(Imp));

  runPostLoadDiscovery(Img, "coff-func-thunk");
  EXPECT_TRUE(Img.ImportStubIndices.empty())
      << "--func must not walk every executable byte for IAT veneers";
  const Import *Named = Img.findImportAt(Thunk);
  ASSERT_NE(Named, nullptr);
  EXPECT_EQ(Named->Name, "_CxxThrowException");
}

} // namespace

TEST(FunctionDiscoveryAlignment, FDEExtentOwnsInteriorRelocationLabels) {
  for (BinaryFormat Format :
       {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO}) {
    for (Arch Architecture : {Arch::X86, Arch::X64, Arch::ARM, Arch::AArch64}) {
      for (unsigned Mutation = 0; Mutation < 4; ++Mutation) {
        SCOPED_TRACE(static_cast<unsigned>(Format));
        SCOPED_TRACE(static_cast<unsigned>(Architecture));
        SCOPED_TRACE(Mutation);
        constexpr va_t Entry = 0x401000, Interior = Entry + 0x20;
        BinaryImage Img;
        Img.Arch = Architecture;
        const unsigned Width =
            Architecture == Arch::X86 || Architecture == Arch::ARM ? 4 : 8;
        Img.Bits = Width == 4 ? Bitness::Bits32 : Bitness::Bits64;
        Img.Format = Format;
        if (Architecture == Arch::ARM)
          Img.Mode = InstructionMode::Thumb;
        Img.Base = 0x400000;
        Img.Entry = Entry;
        std::vector<uint8_t> Bytes(0x50, 0x90);
        Bytes[0] = 0x55;
        Bytes[1] = 0x89;
        Bytes[2] = 0xe5;
        Bytes[0x20] = 0xc3;
        Bytes[0x40] = 0xc3;
        if (Architecture == Arch::AArch64) {
          for (unsigned I = 0; I < Bytes.size(); I += 4)
            writeLE<uint32_t>(Bytes.data() + I, 0xd503201fu); // nop
          writeLE<uint32_t>(Bytes.data(), 0xd10083ffu);       // sub sp, sp, #32
          for (unsigned I : {0x20u, 0x40u})
            writeLE<uint32_t>(Bytes.data() + I, 0xd65f03c0u); // ret
        } else if (Architecture == Arch::ARM) {
          for (unsigned I = 0; I < Bytes.size(); I += 2)
            writeLE<uint16_t>(Bytes.data() + I, 0xbf00u); // nop
          writeLE<uint16_t>(Bytes.data(), 0xb510u);       // push {r4, lr}
          for (unsigned I : {0x20u, 0x40u})
            writeLE<uint16_t>(Bytes.data() + I, 0x4770u); // bx lr
        } else if (Architecture == Arch::X64) {
          Bytes[1] = 0x48;
          Bytes[2] = 0x89;
          Bytes[3] = 0xe5;
        }
        Img.Segments.push_back(executableSegment(Entry, Bytes));
        Segment Table;
        Table.Name = ".rdata";
        Table.VA = 0x402000;
        Table.Size = Width * 2;
        Table.Flags = SegmentFlags::Readable;
        Table.Data.resize(Table.Size);
        const unsigned Tag = Architecture == Arch::ARM ? 1 : 0;
        if (Width == 4) {
          writeLE<uint32_t>(Table.Data.data(), Interior | Tag);
          writeLE<uint32_t>(Table.Data.data() + Width, (Entry + 0x40) | Tag);
        } else {
          writeLE<uint64_t>(Table.Data.data(), Interior);
          writeLE<uint64_t>(Table.Data.data() + Width, Entry + 0x40);
        }
        Img.CodePtrRelocSlots = {Table.VA, Table.VA + Width};
        Img.Segments.push_back(Table);
        Img.Symbols.push_back(Symbol::makeFunc(Entry)); // COFF has no size.
        ExceptionFunction EH;
        EH.Encoding = ExceptionEncoding::DwarfFDE;
        EH.CodeRange = {Entry, Entry + 0x30};
        EH.Dwarf.emplace();
        if (Mutation == 1)
          EH.ParseStatus = ExceptionParseStatus::Malformed;
        if (Mutation == 2)
          EH.Dwarf.reset();
        if (Mutation == 3)
          Img.Symbols.push_back(Symbol::makeFunc(Interior));
        Img.ExceptionMetadata.Functions.push_back(EH);
        // Linked i386 PE deliberately probes pointers inside coarse CRT
        // ranges. Other targets already reject those weaker candidates, so
        // leave coarse coverage absent there to test the FDE's own authority.
        if (Format == BinaryFormat::COFF && Architecture == Arch::X86)
          Img.KnownCodeRanges = {{Entry, Entry + 0x30}};
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Img));
        auto Found = FuncDetector().detect(Img, Dec);
        auto Has = [&](va_t A) {
          return std::any_of(Found.begin(), Found.end(),
                             [&](const auto &F) { return F.first == A; });
        };
        EXPECT_EQ(Has(Interior), Mutation != 0);
        EXPECT_TRUE(Has(Entry));
        EXPECT_TRUE(Has(Entry + 0x40));
      }
    }
  }
}
