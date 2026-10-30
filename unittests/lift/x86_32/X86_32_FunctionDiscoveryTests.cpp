//===- X86_32_FunctionDiscoveryTests.cpp - PE gap function recovery ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/debug/DebugContext.h"
#include "neverd/decode/Decoder.h"
#include "neverd/decode/InstructionRelocations.h"
#include "neverd/ir/high/MedToHigh.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/low/FuncDetector.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/ir/med/MedIR.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/loader/COFF/COFFLoaderUtils.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/loader/PointerRelocation.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryEncoding.h"

#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>

using namespace neverd;

namespace {

BinaryImage makePeText(std::vector<uint8_t> Bytes, va_t Base = 0x401000) {
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = Base;

  Segment Text;
  Text.Name = ".text";
  Text.VA = Base;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = std::move(Bytes);
  Img.Segments.push_back(Text);

  Section Sec;
  Sec.Name = ".text";
  Sec.VA = Base;
  Sec.Size = Img.Segments[0].Size;
  Sec.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Sec.Type = 0x60000020;
  Img.Sections.push_back(Sec);
  Img.KnownCodeRanges.emplace_back(Base, Base + Img.Segments[0].Size);
  return Img;
}

DecodedInsn decodeAt(const BinaryImage &Img, Decoder &Dec, va_t Addr) {
  const Segment *Seg = Img.getSegmentFor(Addr);
  DecodedInsn DI{};
  if (Seg) {
    const size_t Off = static_cast<size_t>(Addr - Seg->VA);
    Dec.decodeOne(Seg->Data.data() + Off, Seg->Data.size() - Off, Addr, DI);
  }
  return DI;
}

} // namespace

TEST(X86_32_FunctionDiscovery, RecoversAlignedUnsymbolizedStackPrologue) {
  // Entry at 0x401000 is a tiny EBP frame.  An unsymbolized callee sits at
  // the next 16-byte boundary with `sub esp, imm32` and is not called.
  std::vector<uint8_t> Bytes(0x30, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Hidden[] = {0x81, 0xEC, 0x20, 0x00, 0x00,
                            0x00, 0x83, 0xC4, 0x20, 0xC3};
  std::copy(Hidden, Hidden + sizeof(Hidden), Bytes.begin() + 0x20);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);

  const bool FoundEntry =
      std::any_of(Functions.begin(), Functions.end(),
                  [](const auto &F) { return F.first == 0x401000; });
  const bool FoundHidden =
      std::any_of(Functions.begin(), Functions.end(),
                  [](const auto &F) { return F.first == 0x401020; });
  EXPECT_TRUE(FoundEntry);
  EXPECT_TRUE(FoundHidden) << "expected 16-byte-aligned sub-esp prologue";
}

TEST(X86_32_FunctionDiscovery, DoesNotPromoteInt3PaddingAsAFunction) {
  std::vector<uint8_t> Bytes(0x30, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_EQ(std::count_if(Functions.begin(), Functions.end(),
                          [](const auto &F) { return F.first == 0x401010; }),
            0);
}

static bool hasEntry(const std::vector<std::pair<va_t, std::string>> &Functions,
                     va_t Addr) {
  return std::any_of(Functions.begin(), Functions.end(),
                     [Addr](const auto &F) { return F.first == Addr; });
}

TEST(X86_32_FunctionDiscovery, PrologueMatchesInsideAFunctionAreItsCode) {
  // The entry's own code falls into a 16-byte-aligned `push esi; push edi`
  // in its prologue and branches back to an aligned loop head `mov esi,
  // [esp+38h]` that it otherwise jumps over: both match the unsymbolized
  // prologue scan, yet both are the entry's code.  A padded function after
  // it, and one only the entry's tail jump reaches, remain entries.
  std::vector<uint8_t> Bytes(0x70, 0xCC);
  const uint8_t Entry[] = {0x55,                   // 401000 push ebp
                           0x8B, 0xEC,             // 401001 mov ebp, esp
                           0x83, 0xEC, 0x40,       // 401003 sub esp, 40h
                           0x53,                   // 401006 push ebx
                           0x8B, 0x5D, 0x08,       // 401007 mov ebx, [ebp+8]
                           0x8B, 0x45, 0x0C,       // 40100A mov eax, [ebp+0Ch]
                           0x8B, 0x4D, 0x10,       // 40100D mov ecx, [ebp+10h]
                           0x56,                   // 401010 push esi
                           0x57,                   // 401011 push edi
                           0x33, 0xF6,             // 401012 xor esi, esi
                           0xEB, 0x0E,             // 401014 jmp 401024h
                           0x90, 0x90, 0x90, 0x90, // 401016 loop-head alignment
                           0x90, 0x90, 0x90, 0x90, 0x90, 0x90,
                           0x8B, 0x74, 0x24, 0x38, // 401020 mov esi, [esp+38h]
                           0x46,                   // 401024 inc esi
                           0x83, 0xFE, 0x10,       // 401025 cmp esi, 10h
                           0x72, 0xF6,             // 401028 jb 401020h
                           0x5F,                   // 40102A pop edi
                           0x5E,                   // 40102B pop esi
                           0x5B,                   // 40102C pop ebx
                           0xE9, 0x2E, 0x00, 0x00, 0x00}; // 40102D jmp 401060h
  std::copy(std::begin(Entry), std::end(Entry), Bytes.begin());
  const uint8_t Padded[] = {0x8B, 0x44, 0x24, 0x04, 0xC3};
  std::copy(std::begin(Padded), std::end(Padded), Bytes.begin() + 0x40);
  const uint8_t TailTarget[] = {0x56, 0x57, 0x8B, 0x44, 0x24,
                                0x0C, 0x5F, 0x5E, 0xC3};
  std::copy(std::begin(TailTarget), std::end(TailTarget), Bytes.begin() + 0x60);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_FALSE(hasEntry(Functions, 0x401010)) << "fallen into";
  EXPECT_FALSE(hasEntry(Functions, 0x401020)) << "a loop's back edge";
  EXPECT_TRUE(hasEntry(Functions, 0x401040)) << "a padded function";
  EXPECT_TRUE(hasEntry(Functions, 0x401060)) << "a tail jump's target";
}

TEST(X86_32_FunctionDiscovery, AFramePrologueAfterACallStaysAnEntry) {
  // A call the detector cannot prove returns may be the last instruction of
  // its function, so a frame prologue right after it is the next function.
  std::vector<uint8_t> Bytes(0x50, 0xCC);
  Bytes[0] = 0x55; // 401000 push ebp
  Bytes[1] = 0x8B; // 401001 mov ebp, esp
  Bytes[2] = 0xEC;
  std::fill(Bytes.begin() + 3, Bytes.begin() + 0x1B, 0x90);
  const uint8_t Call[] = {0xE8, 0x20, 0x00, 0x00, 0x00}; // 40101B call 401040h
  std::copy(std::begin(Call), std::end(Call), Bytes.begin() + 0x1B);
  const uint8_t Frame[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(std::begin(Frame), std::end(Frame), Bytes.begin() + 0x20);
  std::copy(std::begin(Frame), std::end(Frame), Bytes.begin() + 0x40);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401020));
  EXPECT_TRUE(hasEntry(Functions, 0x401040));
}

TEST(X86_32_FunctionDiscovery, RecoversUnalignedFramePrologue) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Hidden[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Hidden, Hidden + sizeof(Hidden), Bytes.begin() + 0x09);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401009))
      << "expected unaligned push-ebp frame prologue";
}

TEST(X86_32_FunctionDiscovery,
     APushOfEbpWithoutTheFrameMoveIsNoUnalignedEntry) {
  // `push 55h; mov eax, esi` holds the bytes 55 8B C6 one byte into its
  // first instruction.  A push of EBP followed by just any move is no frame
  // setup, so the unaligned scan takes it for none.
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, // 401000 push ebp; mov ebp, esp
                           0x6A, 0x55,       // 401003 push 55h
                           0x8B, 0xC6,       // 401005 mov eax, esi
                           0x83, 0xC4, 0x04, // 401007 add esp, 4
                           0x5D, 0xC3};      // 40100A pop ebp; ret
  std::copy(std::begin(Entry), std::end(Entry), Bytes.begin());

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_FALSE(hasEntry(Functions, 0x401004))
      << "55 8B C6 inside an instruction is no frame prologue";
}

TEST(X86_32_FunctionDiscovery, AnInt3ByteInsideAnInstructionIsNoPadding) {
  // A loop's `jne` back to its head has the displacement CC, which the
  // loader's padding scan reads as an int3 before a boundary.  The entry
  // falls through the `jne` into that guessed start, so it is the entry's
  // code.  A start after real padding, past the entry's return, stays one.
  std::vector<uint8_t> Bytes(0x60, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, // 401000 push ebp; mov ebp, esp
                           0x8B, 0x45, 0x08, // 401003 mov eax, [ebp+8]
                           0x85, 0xC0,       // 401006 test eax, eax
                           0x48};            // 401008 dec eax
  std::copy(std::begin(Entry), std::end(Entry), Bytes.begin());
  std::fill(Bytes.begin() + 0x09, Bytes.begin() + 0x38, 0x90);
  const uint8_t Tail[] = {0x75, 0xCC,       // 401038 jne 401006h
                          0x8B, 0x4D, 0x0C, // 40103A mov ecx, [ebp+0Ch]
                          0x5D, 0xC3};      // 40103D pop ebp; ret
  std::copy(std::begin(Tail), std::end(Tail), Bytes.begin() + 0x38);
  const uint8_t Padded[] = {0x56,                   // 401041 push esi
                            0x8B, 0x74, 0x24, 0x08, // mov esi, [esp+8]
                            0x8B, 0xC6,             // mov eax, esi
                            0x5E, 0xC3};            // pop esi; ret
  std::copy(std::begin(Padded), std::end(Padded), Bytes.begin() + 0x41);

  BinaryImage Img = makePeText(std::move(Bytes));
  Img.KnownCodeRanges.clear();
  runPostLoadDiscovery(Img, "test");
  const std::set<va_t> Guesses = Img.boundaryGuessFunctionStarts();
  ASSERT_TRUE(Guesses.count(0x40103A)) << "the loader guesses after the CC";
  ASSERT_TRUE(Guesses.count(0x401041));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_FALSE(hasEntry(Functions, 0x40103A)) << "fallen into past a jne";
  EXPECT_TRUE(hasEntry(Functions, 0x401041)) << "a start after real padding";
}

namespace {

BinaryImage operandEntryImage() {
  std::vector<uint8_t> Bytes(0x60, 0xCC);
  Bytes[0] = 0xEB;
  Bytes[1] = 0x0C;                                      // jmp 40100E
  const uint8_t Body[] = {0x68, 0x90, 0x56, 0x53, 0x90, // push 90535690h
                          0x58, 0xC3};                  // pop eax; ret
  std::copy(std::begin(Body), std::end(Body), Bytes.begin() + 0x0E);
  return makePeText(std::move(Bytes));
}

} // namespace

TEST(X86_32_FunctionDiscovery, OperandBytesCannotSupplyHeuristicEntries) {
  // 401010 looks like an aligned push-register prologue. The loader can also
  // guess an unaligned boundary at 401011. Both are bytes of a reachable
  // PUSH immediate, not instructions at those addresses.
  BinaryImage Img = operandEntryImage();
  Symbol Guess = Symbol::makeFunc(0x401011, 0);
  Guess.IsBoundaryGuess = true;
  Img.Symbols.push_back(Guess);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_FALSE(hasEntry(Functions, 0x401010));
  EXPECT_FALSE(hasEntry(Functions, 0x401011));
}

TEST(X86_32_FunctionDiscovery, OperandEntriesKeepIndependentEvidence) {
  for (unsigned Evidence = 0; Evidence != 4; ++Evidence) {
    SCOPED_TRACE(Evidence);
    BinaryImage Img = operandEntryImage();
    switch (Evidence) {
    case 0:
      Img.Symbols.push_back(Symbol::makeFunc(0x401010, 0));
      break;
    case 1: {
      Export Exp;
      Exp.Name = "overlapping_entry";
      Exp.Addr = 0x401010;
      Img.Exports.push_back(Exp);
      break;
    }
    case 2: {
      auto &Bytes = Img.Segments[0].Data;
      Bytes[0x30] = 0xE8; // call 401010
      writeLE<uint32_t>(Bytes.data() + 0x31, uint32_t(-0x25));
      Bytes[0x35] = 0xC3;
      break;
    }
    case 3: {
      Segment Data;
      Data.Name = ".rdata";
      Data.VA = 0x402000;
      Data.Size = 4;
      Data.Flags = SegmentFlags::Readable;
      Data.Data = {0x10, 0x10, 0x40, 0x00};
      Img.Segments.push_back(std::move(Data));
      Img.CodePtrRelocSlots.insert(0x402000);
      break;
    }
    }
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X86));
    FuncDetector Detector;
    EXPECT_TRUE(hasEntry(Detector.detect(Img, Dec), 0x401010));
  }
}

TEST(X86_32_FunctionDiscovery, UnreachedOperandsDoNotDisproveAnEntry) {
  BinaryImage Img = operandEntryImage();
  Img.Segments[0].Data[0] = 0xC3;
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  EXPECT_TRUE(hasEntry(Detector.detect(Img, Dec), 0x401010));
}

TEST(X86_32_FunctionDiscovery, OperandProofRequiresOneCompleteCodeOwner) {
  for (bool DataMarker : {false, true}) {
    SCOPED_TRACE(DataMarker);
    BinaryImage Img = operandEntryImage();
    if (DataMarker) {
      Img.DataInCode.insert(0x40100F);
    } else {
      Img.KnownCodeRanges.clear();
      Img.Sections[0].Size = 0x0F;
      Section Data = Img.Sections[0];
      Data.VA = 0x40100F;
      Data.Size = 1;
      Data.Flags = SegmentFlags::Readable;
      Data.Type = 0x40000040;
      Img.Sections.push_back(Data);
      Section Tail = Img.Sections[0];
      Tail.VA = 0x401010;
      Tail.Size = 0x50;
      Img.Sections.push_back(Tail);
    }
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Arch::X86));
    FuncDetector Detector;
    EXPECT_TRUE(hasEntry(Detector.detect(Img, Dec), 0x401010));
  }
}

TEST(X86_32_FunctionDiscovery, AJumpIntoAnOperandPreservesItsEntry) {
  BinaryImage Img = operandEntryImage();
  auto &Bytes = Img.Segments[0].Data;
  const uint8_t Entry[] = {0x75, 0x06,  // jne 401008
                           0xEB, 0x0A}; // jmp 40100E
  std::copy(std::begin(Entry), std::end(Entry), Bytes.begin());
  Bytes[8] = 0xEB;
  Bytes[9] = 0x06; // jmp 401010
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  EXPECT_TRUE(hasEntry(Detector.detect(Img, Dec), 0x401010));
}

TEST(X86_32_FunctionDiscovery, AnUnresolvedJumpMayEnterAnOperand) {
  BinaryImage Img = operandEntryImage();
  auto &Bytes = Img.Segments[0].Data;
  const uint8_t Entry[] = {0x75, 0x1E,  // jne 401020
                           0xEB, 0x0A}; // jmp 40100E
  std::copy(std::begin(Entry), std::end(Entry), Bytes.begin());
  Bytes[0x20] = 0xFF;
  Bytes[0x21] = 0xE0; // jmp eax
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  EXPECT_TRUE(hasEntry(Detector.detect(Img, Dec), 0x401010));
}

TEST(X86_32_FunctionDiscovery, OverlappingDecodesDoNotDisproveAnOperandEntry) {
  BinaryImage Img = operandEntryImage();
  auto &Bytes = Img.Segments[0].Data;
  const uint8_t Entry[] = {0x75, 0x0B,  // jne 40100D
                           0xEB, 0x0A}; // jmp 40100E
  std::copy(std::begin(Entry), std::end(Entry), Bytes.begin());
  Bytes[0x0D] = 0xB8; // mov eax, 53569068h overlaps the PUSH.
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  EXPECT_TRUE(hasEntry(Detector.detect(Img, Dec), 0x401010));
}

TEST(X86_32_FunctionDiscovery, RecoversAlignedPushRegAndSehPrologues) {
  std::vector<uint8_t> Bytes(0x50, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t PushReg[] = {0x53, 0x56, 0x57, 0x8B, 0x7C, 0x24,
                             0x10, 0x5F, 0x5E, 0x5B, 0xC3};
  std::copy(PushReg, PushReg + sizeof(PushReg), Bytes.begin() + 0x20);
  const uint8_t Seh[] = {0x6A, 0xFF, 0x58, 0xC3};
  std::copy(Seh, Seh + sizeof(Seh), Bytes.begin() + 0x40);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401020))
      << "expected 16-byte-aligned push-ebx/esi/edi prologue";
  EXPECT_TRUE(hasEntry(Functions, 0x401040))
      << "expected 16-byte-aligned SEH push -1 prologue";
}

TEST(X86_32_FunctionDiscovery,
     RecoversLargeDirectCallWhenVerifyBudgetExhausts) {
  const size_t NopCount = static_cast<size_t>(limits::kMaxVerifyInsns) + 8;
  std::vector<uint8_t> Bytes(0x10 + NopCount + 1, 0xCC);
  Bytes[0] = 0xE8;
  writeLE<uint32_t>(Bytes.data() + 1, 3); // call 0x401008
  Bytes[5] = 0xC3;
  std::fill(Bytes.begin() + 8, Bytes.begin() + 8 + NopCount, 0x90);
  Bytes[8 + NopCount] = 0xC3;

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401008))
      << "direct CALL target must survive a verify walk that exhausts "
         "kMaxVerifyInsns";
}

TEST(X86_32_FunctionDiscovery,
     RecoversRelocBackedLeafWhenVerifyBudgetExhausts) {
  const size_t NopCount = static_cast<size_t>(limits::kMaxVerifyInsns) + 8;
  std::vector<uint8_t> Bytes(0x10 + NopCount + 1, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  std::fill(Bytes.begin() + 8, Bytes.begin() + 8 + NopCount, 0x90);
  Bytes[8 + NopCount] = 0xC3;

  BinaryImage Img = makePeText(std::move(Bytes));
  Segment Data;
  Data.Name = ".rdata";
  Data.VA = 0x402000;
  Data.Size = 4;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(4);
  writeLE<uint32_t>(Data.Data.data(), 0x401008);
  Img.Segments.push_back(std::move(Data));
  Img.CodePtrRelocSlots.insert(0x402000);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401008))
      << "PE HIGHLOW code-pointer target must survive verify budget exhaustion";
}

TEST(X86_32_FunctionDiscovery, RecoversUnalignedStackAdjustAfterStdcallRet) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0xC2, 0x08, 0x00};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Hidden[] = {0x83, 0xEC, 0x08, 0x33, 0xC0, 0x83,
                            0xC4, 0x08, 0xC2, 0x08, 0x00};
  std::copy(Hidden, Hidden + sizeof(Hidden), Bytes.begin() + 0x06);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401006))
      << "expected packed unaligned sub-esp prologue after ret imm16";
}

TEST(X86_32_FunctionDiscovery, StartsUnalignedHotpatchFrameAtItsNoOp) {
  // MSVC's hotpatchable entry, `mov edi, edi; push ebp; mov ebp, esp`, packed
  // after another function's ret.  The frame scan finds `push ebp` two bytes
  // in, but the function begins at the no-op.
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Hotpatch[] = {0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Hotpatch, Hotpatch + sizeof(Hotpatch), Bytes.begin() + 0x05);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401005));
  EXPECT_FALSE(hasEntry(Functions, 0x401007))
      << "the push after the hotpatch no-op is not an entry of its own";
}

TEST(X86_32_FunctionDiscovery, LoaderRegistersPackedHotpatchEntries) {
  std::vector<uint8_t> Bytes = {
      0x55, 0x8B, 0xEC, 0x5D, 0xC2, 0x08, 0x00, // 00: frame; ret 8
      0x8B, 0xFF, 0x56, 0x5E, 0xC3,             // 07: no-op; push esi
      0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x5D, 0xC3, // 0C: no-op; EBP frame
      0xB8, 0x8B, 0xFF, 0x56, 0x00, 0xC3,       // 13: mov eax, 0056FF8Bh
      0x8B, 0xFF, 0x55, 0x29, 0x40, 0x00,       // 19: no-op, jump table
  };
  BinaryImage Img = makePeText(std::move(Bytes));
  // x86 PE has no function table to claim ranges from.
  Img.KnownCodeRanges.clear();

  scanX86HotpatchEntries(Img);

  std::vector<va_t> Found;
  for (const Symbol &Sym : Img.Symbols) {
    EXPECT_TRUE(Sym.IsFunc);
    EXPECT_FALSE(Sym.IsBoundaryGuess);
    Found.push_back(Sym.Addr);
  }
  std::sort(Found.begin(), Found.end());
  // A push after the no-op begins a function only after another one ends:
  // inside an immediate, or where MSVC pads a jump table, it does not.
  EXPECT_EQ(Found, (std::vector<va_t>{0x401007, 0x40100C}));
}

TEST(X86_32_FunctionDiscovery, BoundaryGuessesAreThoseNothingElseConfirms) {
  BinaryImage Img;
  const auto Add = [&](va_t Addr, bool Guess, const char *Name = nullptr) {
    Symbol Sym = Symbol::makeFunc(Addr, 4);
    if (Name)
      Sym.Name = Name;
    Sym.IsBoundaryGuess = Guess;
    Img.Symbols.push_back(Sym);
  };
  Add(0x401100, false);
  EXPECT_TRUE(Img.boundaryGuessFunctionStarts().empty());
  // A guess alone, one another symbol confirms, one debug information
  // named, one a known code range starts, and one whose range continues
  // another function's, which confirms nothing.
  Add(0x401000, true);
  Add(0x401010, true);
  Add(0x401010, false);
  Add(0x401020, true, "named_by_debug_info");
  Add(0x401030, true);
  Add(0x401040, true);
  Img.KnownCodeRanges = {{0x401030, 0x401038}, {0x401040, 0x401048}};
  Img.ContinuationCodeStarts = {0x401040};
  EXPECT_EQ(Img.boundaryGuessFunctionStarts(),
            (std::set<va_t>{0x401000, 0x401040}));
}

TEST(X86_32_FunctionDiscovery, HotpatchEntryAfterPaddingIsNotAGuess) {
  std::vector<uint8_t> Bytes = {0x55, 0x8B, 0xEC, 0x5D, 0xC3, 0xCC, 0xCC,
                                0x8B, 0xFF, 0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  BinaryImage Img = makePeText(std::move(Bytes));
  Img.KnownCodeRanges.clear();

  runPostLoadDiscovery(Img, "test");

  const auto It =
      std::find_if(Img.Symbols.begin(), Img.Symbols.end(),
                   [](const Symbol &Sym) { return Sym.Addr == 0x401007; });
  ASSERT_NE(It, Img.Symbols.end());
  EXPECT_FALSE(It->IsBoundaryGuess)
      << "a direct jump to a hotpatch entry is a tail call";
  EXPECT_TRUE(Img.boundaryGuessFunctionStarts().empty());
}

TEST(X86_32_FunctionDiscovery,
     SeedsPrimaryExceptionFunctionInsideCoarseKnownRange) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  Bytes[0] = 0xC3;
  Bytes[0x10] = 0xC3;
  BinaryImage Img = makePeText(std::move(Bytes));
  Img.KnownCodeRanges = {{0x401000, 0x401020}};
  ExceptionFunction EH;
  EH.CodeRange = {0x401010, 0x401012};
  EH.Kind = RuntimeFunctionKind::Primary;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401010))
      << "pdata/registration primary starts are functions even inside a coarse "
         "KnownCodeRange";
}

class SizedFuncDebugContext : public DebugContext {
public:
  FunctionSym Function;
  std::optional<FunctionSym> resolveFunction(va_t Addr) const override {
    if (Addr == Function.Addr)
      return Function;
    return std::nullopt;
  }
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override {
    return std::nullopt;
  }
  std::optional<TypeSym> resolveType(uint64_t) const override {
    return std::nullopt;
  }
  std::optional<SourceLoc> sourceLocation(va_t) const override {
    return std::nullopt;
  }
  std::vector<FunctionSym> allFunctions() const override { return {Function}; }
  bool hasInfo() const override { return true; }
};

TEST(X86_32_FunctionDiscovery, PipelineDropsInteriorsInsideDebugExtents) {
  std::vector<uint8_t> Bytes(0x40, 0xCC);
  const uint8_t Leaf[] = {0x68, 0x00, 0x00, 0x00, 0x00, 0x55,
                          0x8B, 0xEC, 0x5D, 0xC3, 0x90};
  std::copy(Leaf, Leaf + sizeof(Leaf), Bytes.begin());
  const uint8_t Caller[] = {0x55, 0x8B, 0xEC, 0xE8, 0xDD,
                            0xFF, 0xFF, 0xFF, 0x5D, 0xC3};
  std::copy(Caller, Caller + sizeof(Caller), Bytes.begin() + 0x20);

  BinaryImage Img = makePeText(std::move(Bytes));
  SizedFuncDebugContext Dbg;
  Dbg.Function.Name = "covering";
  Dbg.Function.Addr = 0x401000;
  Dbg.Function.Size = sizeof(Leaf);

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  PipelineResult Result = Pipeline().run(Img, Ctx, Opts, &Dbg);
  EXPECT_TRUE(
      std::any_of(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                  [](const HighFunc &F) { return F.Entry == 0x401000; }));
  EXPECT_FALSE(
      std::any_of(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                  [](const HighFunc &F) { return F.Entry == 0x401005; }))
      << "debug-sized function must keep interior CALL targets as blocks";
  const auto Covering =
      std::find_if(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                   [](const HighFunc &F) { return F.Entry == 0x401000; });
  ASSERT_NE(Covering, Result.HighFuncs.end());
  EXPECT_FALSE(Covering->Body.empty());
}

TEST(X86_32_FunctionDiscovery, DropsCallTargetsInsideSizedFunctionSymbol) {
  std::vector<uint8_t> Bytes(0x40, 0xCC);
  // 11-byte leaf: push imm32 then a nested-looking EBP frame.  A later caller
  // targets the interior frame; the sized symbol still owns the bytes.
  const uint8_t Leaf[] = {0x68, 0x00, 0x00, 0x00, 0x00, 0x55,
                          0x8B, 0xEC, 0x5D, 0xC3, 0x90};
  std::copy(Leaf, Leaf + sizeof(Leaf), Bytes.begin());
  // push ebp; mov ebp, esp; call 0x401005; pop ebp; ret
  const uint8_t Caller[] = {0x55, 0x8B, 0xEC, 0xE8, 0xDD,
                            0xFF, 0xFF, 0xFF, 0x5D, 0xC3};
  std::copy(Caller, Caller + sizeof(Caller), Bytes.begin() + 0x20);

  BinaryImage Img = makePeText(std::move(Bytes));
  Symbol Covering = Symbol::makeFunc(0x401000, sizeof(Leaf));
  Covering.Name = "covering";
  Img.Symbols.push_back(std::move(Covering));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401020));
  EXPECT_FALSE(hasEntry(Functions, 0x401005))
      << "direct-call target inside a sized function symbol is an interior "
         "block";

  std::set<va_t> Entries;
  for (const auto &F : Functions)
    Entries.insert(F.first);
  CFGBuilder CFG;
  CFG.setKnownFuncEntries(&Entries);
  auto LeafIR = CFG.build(Img, Dec, 0x401000, "covering");
  EXPECT_GE(LeafIR.DecodedInstructionCount, 2u)
      << "CFG must not stop at a dropped interior start 5 bytes in";
}

TEST(X86_32_FunctionDiscovery, RecoversAlignedThiscallVtableStoreThunk) {
  std::vector<uint8_t> Bytes(0x30, 0x90);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  const uint8_t Thunk[] = {0xC7, 0x01, 0x00, 0x20, 0x40, 0x00, 0xC3};
  std::copy(Thunk, Thunk + sizeof(Thunk), Bytes.begin() + 0x20);

  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401020))
      << "expected 16-byte-aligned mov [ecx], imm32 destructor thunk";
}

TEST(X86_32_FunctionDiscovery, KeepsAnExportThatEndsAtANoReturnImport) {
  // The entry returns.  Finish, which only its export names, ends at the
  // last bytes of .text calling ExitProcess through its import slot:
  //   mov eax, [esp+4]; cmp eax, 4; jge exit; inc eax; ret
  //   exit: push eax; call dword ptr [0x402000]
  std::vector<uint8_t> Bytes(0x10, 0xCC);
  Bytes[0] = 0xC3;
  const uint8_t Finish[] = {0x8B, 0x44, 0x24, 0x04, 0x83, 0xF8,
                            0x04, 0x7D, 0x02, 0x40, 0xC3, 0x50,
                            0xFF, 0x15, 0x00, 0x20, 0x40, 0x00};
  Bytes.insert(Bytes.end(), std::begin(Finish), std::end(Finish));
  BinaryImage Img = makePeText(std::move(Bytes));
  Import Exit;
  Exit.Name = "ExitProcess";
  Exit.IATAddr = 0x402000;
  Img.Imports.push_back(Exit);
  Export Exp;
  Exp.Name = "Finish";
  Exp.Addr = 0x401010;
  Img.Exports.push_back(Exp);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  EXPECT_TRUE(hasEntry(Detector.detect(Img, Dec), 0x401010))
      << "the call through ExitProcess's slot does not return to run past "
         ".text";
}

TEST(X86_32_FunctionDiscovery, ImportScanIncludesFinalCompleteVeneer) {
  for (bool Is64 : {false, true}) {
    for (bool PIC : {false, true}) {
      if (Is64 && PIC)
        continue;
      for (size_t Offset : {size_t(0), size_t(1), size_t(128)}) {
        SCOPED_TRACE(Is64);
        SCOPED_TRACE(PIC);
        SCOPED_TRACE(Offset);
        const va_t Target = 0x402000;
        std::vector<uint8_t> Bytes(Offset + 6, 0xFF);
        Bytes[Offset] = 0xFF;
        Bytes[Offset + 1] = PIC ? 0xA3 : 0x25;
        const uint32_t Field =
            static_cast<uint32_t>(Is64  ? Target - (0x401000 + Offset + 6)
                                  : PIC ? Target - 0x403000
                                        : Target);
        writeLE(Bytes.data() + Offset + 2, Field);
        auto Image = makePeText(std::move(Bytes));
        Image.KnownCodeRanges.clear();
        Image.Arch = Is64 ? Arch::X64 : Arch::X86;
        Image.Bits = Is64 ? Bitness::Bits64 : Bitness::Bits32;
        Image.DynInfo.PltGotAddr = PIC ? 0x403000 : 0;
        Import Imported;
        Imported.IATAddr = Target;
        Imported.Name = "imported";
        Image.Imports.push_back(Imported);
        auto Truncated = Image;
        Truncated.Segments[0].Data.pop_back();

        scanImportThunks(Image);
        EXPECT_TRUE(Image.isImportStubAt(0x401000 + Offset));
        ASSERT_EQ(Image.Symbols.size(), 1u);
        EXPECT_EQ(Image.Symbols[0].Addr, 0x401000 + Offset);
        scanImportThunks(Truncated);
        EXPECT_TRUE(Truncated.Symbols.empty());
      }
    }
  }
}

TEST(X86_32_FunctionDiscovery, DecompileIncludesImportStubs) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  // jmp dword ptr [0x402000]
  const uint8_t Thunk[] = {0xFF, 0x25, 0x00, 0x20, 0x40, 0x00};
  std::copy(Thunk, Thunk + sizeof(Thunk), Bytes.begin());
  BinaryImage Img = makePeText(std::move(Bytes));
  Import Imp;
  Imp.Name = "imported";
  Imp.IATAddr = 0x402000;
  Img.Imports.push_back(Imp);
  ASSERT_TRUE(Img.recordImportStub(0x401000, 0));
  ASSERT_TRUE(Img.isImportStubAt(0x401000));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Pipeline Pipe;
  PipelineResult Result = Pipe.run(Img, Ctx, Opts);
  const bool Found =
      std::any_of(Result.HighFuncs.begin(), Result.HighFuncs.end(),
                  [](const HighFunc &F) { return F.Entry == 0x401000; });
  EXPECT_TRUE(Found) << "decompile must emit import thunks, not skip them";
}

TEST(X86_32_FunctionDiscovery, SkipsSsaWhenInstructionCountExceedsCap) {
  LowFunc Low;
  Low.Name = "wide_ssa";
  Low.Entry = 0x401000;
  Low.DecodedInstructionCount =
      static_cast<uint64_t>(limits::kMaxSSAFunctionOps) + 1;
  LowBlock B0;
  B0.Id = 0;
  B0.StartAddr = 0x401000;
  B0.Succs = {1};
  LowOp Nop;
  Nop.Opcode = NdOp::NOP;
  Nop.Addr = 0x401000;
  B0.Ops.push_back(Nop);
  LowBlock B1;
  B1.Id = 1;
  B1.StartAddr = 0x401010;
  B1.Preds = {0};
  LowOp Ret;
  Ret.Opcode = NdOp::RETURN;
  Ret.Addr = 0x401010;
  B1.Ops.push_back(Ret);
  Low.Blocks = {B0, B1};

  MedFunc Med = LowToMedConverter().convert(Low, Arch::X86, BinaryFormat::COFF);
  EXPECT_TRUE(Med.SkippedSSA);
  ASSERT_EQ(Med.Blocks.size(), 2u);
  EXPECT_TRUE(Med.Blocks[0].Phis.empty());
  EXPECT_TRUE(Med.Blocks[1].Phis.empty());
  EXPECT_EQ(Med.Entry, 0x401000u);
}

static MedFunc makeWideChainMed() {
  MedFunc Med;
  Med.Name = "wide_cfg";
  Med.Entry = 0x401000;
  for (int I = 0; I < 1025; ++I) {
    MedBlock Block;
    Block.Id = I;
    Block.StartAddr = 0x401000 + static_cast<va_t>(I);
    if (I + 1 < 1025)
      Block.Succs = {I + 1};
    Med.Blocks.push_back(Block);
  }
  return Med;
}

TEST(X86_32_FunctionDiscovery, LargeIncompleteMutableMedIRFailsExplicitly) {
  MedFunc Med = makeWideChainMed();
  Med.SkippedSSA = true;
  MedToHighConverter Conv;
  EXPECT_THROW(Conv.convert(Med, Arch::X86), std::runtime_error);
}

TEST(X86_32_FunctionDiscovery, LargeSSAMedIRIsLoweredBlockByBlock) {
  // Past the structuring limits an SSA function still gets its statements;
  // a straight chain needs no goto at all.
  MedFunc Med = makeWideChainMed();
  MedToHighConverter Conv;
  HighFunc HF = Conv.convert(Med, Arch::X86);
  ASSERT_FALSE(HF.Body.empty());
  const bool HasGoto =
      std::any_of(HF.Body.begin(), HF.Body.end(),
                  [](const HighStmt &S) { return S.Kind == StmtKind::Goto; });
  EXPECT_FALSE(HasGoto);
  EXPECT_EQ(HF.Body.back().Kind, StmtKind::Return);
}

TEST(X86_32_FunctionDiscovery, FillUnstructuredSkeletonFromTwoBlocks) {
  MedFunc Med;
  Med.Name = "two_block";
  Med.Entry = 0x401000;
  MedBlock First;
  First.Id = 0;
  First.StartAddr = 0x401000;
  First.Succs = {1};
  MedBlock Second;
  Second.Id = 1;
  Second.StartAddr = 0x401010;
  Med.Blocks = {First, Second};

  HighFunc HF;
  HF.Name = Med.Name;
  HF.Entry = Med.Entry;
  fillUnstructuredGotoSkeleton(HF, Med);
  ASSERT_EQ(HF.Body.size(), 4u);
  EXPECT_EQ(HF.Body[0].Kind, StmtKind::Nop);
  EXPECT_EQ(HF.Body[1].Kind, StmtKind::Goto);
  EXPECT_EQ(HF.Body[1].GotoTarget, 0x401010u);
  EXPECT_EQ(HF.Body[3].Kind, StmtKind::Return);
}

TEST(X86_32_FunctionDiscovery, CfgDoesNotFollowPastNextFunctionStart) {
  std::vector<uint8_t> Bytes(0x20, 0x90);
  Bytes[0x1F] = 0xC3;
  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  std::set<va_t> Entries{0x401000, 0x401010};
  CFGBuilder CFG;
  CFG.setKnownFuncEntries(&Entries);
  auto First = CFG.build(Img, Dec, 0x401000, "first");
  EXPECT_LE(First.DecodedInstructionCount, 16u)
      << "jump/fallthrough must not decode past the next function start";
}

TEST(X86_32_FunctionDiscovery, CfgStopsAtNextFunctionEntry) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  // Two packed functions: NOPs then a RET that belongs to the second start.
  Bytes[0] = 0x90;
  Bytes[1] = 0x90;
  Bytes[2] = 0xC3;
  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  std::set<va_t> Entries{0x401000, 0x401002};
  CFGBuilder CFG;
  CFG.setKnownFuncEntries(&Entries);
  auto First = CFG.build(Img, Dec, 0x401000, "first");
  EXPECT_LT(First.DecodedInstructionCount, 3u)
      << "must not decode the next function's RET as part of the first body";
}

TEST(X86_32_FunctionDiscovery, LiftsMmxPunpckldq) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Body[] = {0x0F, 0x62, 0xC1, 0x0F, 0x77, 0xC3};
  std::copy(Body, Body + sizeof(Body), Bytes.begin());
  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  CFGBuilder CFG;
  auto LF = CFG.build(Img, Dec, 0x401000, "mmx_punpck");
  // PUNPCKLDQ lifts. EMMS empties the x87 tags, a state i386 has no owner
  // for, so it alone is refused rather than lifted as a NOP
  // (X86LegacyAdmission.StateInstructionsCannotBecomeNopsWithoutOwnership).
  EXPECT_TRUE(LF.DecodeFailureAddresses.empty());
  EXPECT_EQ(LF.UnsupportedInstructionAddresses, std::vector<va_t>{0x401003});
}

TEST(X86_32_FunctionDiscovery, LiftsMmxPunpckldqFromMemory) {
  std::vector<uint8_t> Bytes(0x30, 0xCC);
  // punpckldq mm0, qword ptr [0x401020]; emms; ret
  const uint8_t Body[] = {0x0F, 0x62, 0x05, 0x20, 0x10,
                          0x40, 0x00, 0x0F, 0x77, 0xC3};
  std::copy(Body, Body + sizeof(Body), Bytes.begin());
  BinaryImage Img = makePeText(std::move(Bytes));
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  CFGBuilder CFG;
  auto LF = CFG.build(Img, Dec, 0x401000, "mmx_punpck_mem");
  // As above, only the EMMS is refused.
  EXPECT_TRUE(LF.DecodeFailureAddresses.empty());
  EXPECT_EQ(LF.UnsupportedInstructionAddresses, std::vector<va_t>{0x401007});
}

TEST(X86_32_FunctionDiscovery,
     DecompileKeepsFirstPassWhenJumpTableBudgetExhausts) {
  std::vector<uint8_t> Bytes(0x20, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3};
  std::copy(Entry, Entry + sizeof(Entry), Bytes.begin());
  BinaryImage Img = makePeText(std::move(Bytes));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.JumpTableEvidenceBudgetForTesting = 0;
  Opts.EmitDumpOutput = false;
  Pipeline Pipe;
  PipelineResult Result = Pipe.run(Img, Ctx, Opts);
  ASSERT_FALSE(Result.HighFuncs.empty())
      << "decompile must keep first-pass CFGs when table evidence is exhausted";
  EXPECT_FALSE(Result.HighFuncs.front().Name.empty());
}

namespace {

void addPeSection(BinaryImage &Img, const char *Name, va_t VA,
                  std::vector<uint8_t> Bytes, SegmentFlags Flags) {
  Segment Seg;
  Seg.Name = Name;
  Seg.VA = VA;
  Seg.Size = Bytes.size();
  Seg.FileSz = Bytes.size();
  Seg.Flags = Flags;
  Seg.Data = std::move(Bytes);
  Section Sec;
  Sec.Name = Name;
  Sec.VA = VA;
  Sec.Size = Seg.Size;
  Sec.FileSz = Seg.Size;
  Sec.Flags = Flags;
  Img.Segments.push_back(std::move(Seg));
  Img.Sections.push_back(std::move(Sec));
}

/// A linked i386 PE whose switch dispatches through the table MSVC places in
/// .text after the function, two bytes past a four-byte boundary.  The next
/// function stores a pointer with `mov dword ptr [disp32], imm32`, whose two
/// relocated fields sit back to back, and reads a relocated .rdata pointer
/// an instruction names: an anchor past the table, four-byte aligned.  The
/// base relocations are recorded as the COFF loader records them.
BinaryImage makeSwitchTableAmongCode() {
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  // 0x401000: switch_on_arg.
  Emit({0x55, 0x8B, 0xEC});                         // push ebp; mov ebp, esp
  Emit({0x8B, 0x45, 0x08});                         // mov eax, [ebp+8]
  Emit({0x83, 0xF8, 0x02, 0x77, 0x1D});             // cmp eax, 2; ja 0x401028
  Emit({0xFF, 0x24, 0x85, 0x2E, 0x10, 0x40, 0x00}); // jmp [eax*4+0x40102E]
  Emit({0xB8, 0x0A, 0x00, 0x00, 0x00, 0x5D, 0xC3}); // 0x401012: case 0
  Emit({0xB8, 0x14, 0x00, 0x00, 0x00, 0x5D, 0xC3}); // 0x401019: case 1
  Emit({0xB8, 0x1E, 0x00, 0x00, 0x00, 0x5D, 0xC3}); // 0x401020: case 2
  Emit({0x90, 0x33, 0xC0, 0x5D, 0xC3});             // nop; 0x401028: default
  Emit({0x8B, 0xC0});                               // mov eax, eax
  Emit({0x12, 0x10, 0x40, 0x00, 0x19, 0x10});       // 0x40102E: the table
  Emit({0x40, 0x00, 0x20, 0x10, 0x40, 0x00});       //
  Emit({0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC});       //
  // 0x401040: mov dword ptr [0x403000], 0x401040.
  Emit({0xC7, 0x05, 0x00, 0x30, 0x40, 0x00, 0x40, 0x10, 0x40, 0x00});
  Emit({0xB9, 0x00, 0x20, 0x40, 0x00}); // mov ecx, 0x402000
  Emit({0x8B, 0x01, 0xC3});             // mov eax, [ecx]; ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  addPeSection(Img, ".rdata", 0x402000, {0x40, 0x10, 0x40, 0x00},
               SegmentFlags::Readable);
  addPeSection(Img, ".data", 0x403000, std::vector<uint8_t>(4, 0),
               SegmentFlags::Readable | SegmentFlags::Writable);
  recordAbsolutePointerRelocations(Img, {{0x40100E, 0x40102E, InvalidVA},
                                         {0x40102E, 0x401012, InvalidVA},
                                         {0x401032, 0x401019, InvalidVA},
                                         {0x401036, 0x401020, InvalidVA},
                                         {0x401042, 0x403000, InvalidVA},
                                         {0x401046, 0x401040, InvalidVA},
                                         {0x40104B, 0x402000, InvalidVA},
                                         {0x402000, 0x401040, InvalidVA}});
  coff_loader::recordRelocatedPointerRunsInCode(Img);
  return Img;
}

} // namespace

TEST(X86_32_FunctionDiscovery, CallbackStoredByARelocatedImmediateIsAFunction) {
  // A linked i386 PE stores a callback into an object with
  // `mov dword ptr [ecx+0x10], offset cb`, whose imm32 is a base relocation.
  // The callback is a function the code only takes the address of; a target
  // the storing function names inside itself stays its own label.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  // 0x401000: register_callback.
  Emit({0x55, 0x8B, 0xEC});                         // push ebp; mov ebp, esp
  Emit({0x8B, 0x4D, 0x08});                         // mov ecx, [ebp+8]
  Emit({0xC7, 0x41, 0x10, 0x40, 0x10, 0x40, 0x00}); // mov [ecx+0x10], 0x401040
  Emit({0x5D, 0xC3});                               // pop ebp; ret
  Text.resize(0x40, 0xCC);
  // 0x401040: the callback.
  Emit({0x55, 0x8B, 0xEC, 0x33, 0xC0, 0x5D, 0xC3}); // frame; xor eax,eax; ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  // The imm32 of the mov at 0x401009 is at 0x40100C.
  Img.CodeAddressRelocOperands.emplace(
      0x40100C, RelocatedAddressField{.EncodedValue = 0x401040,
                                      .TargetVA = 0x401040,
                                      .Width = 4,
                                      .TargetOwnerVA = 0x401040});

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401040))
      << "a callback a relocated immediate names is a function of its own";
}

TEST(X86_32_FunctionDiscovery, AVtableSlotConfirmsAPaddingGuess) {
  // A destructor follows its neighbour's return past int3 padding and
  // nothing calls it, so the loader only guesses its start.  A vtable in
  // .rdata holds a relocated pointer to it, which names a function: MSVC
  // enters a cold chunk only by a jump.  A start only padding marks stays a
  // guess.
  std::vector<uint8_t> Text(0x30, 0xCC);
  const uint8_t Entry[] = {0x55, 0x8B, 0xEC, 0x5D, 0xC3}; // 401000
  std::copy(std::begin(Entry), std::end(Entry), Text.begin());
  const uint8_t Dtor[] = {0x56, 0x8B, 0xF1, 0x5E, 0xC3}; // 401010
  std::copy(std::begin(Dtor), std::end(Dtor), Text.begin() + 0x10);
  const uint8_t Other[] = {0x57, 0x8B, 0xF9, 0x5F, 0xC3}; // 401020
  std::copy(std::begin(Other), std::end(Other), Text.begin() + 0x20);
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  addPeSection(Img, ".rdata", 0x402000, {0x10, 0x10, 0x40, 0x00},
               SegmentFlags::Readable);
  recordAbsolutePointerRelocations(Img, {{0x402000, 0x401010, InvalidVA}});
  ASSERT_TRUE(Img.CodePtrRelocSlots.count(0x402000));

  runPostLoadDiscovery(Img, "test");
  const std::set<va_t> Guesses = Img.boundaryGuessFunctionStarts();
  EXPECT_FALSE(Guesses.count(0x401010)) << "a vtable slot names it";
  EXPECT_TRUE(Guesses.count(0x401020)) << "only padding marks it";
}

TEST(X86_32_FunctionDiscovery, CallbacksPushedPastTheReturnAreFunctions) {
  // MSVC passes an array's constructor and destructor to its iterator with
  // `push offset`, and places them right after the function pushing them,
  // past its return.  The function's own code never reaches them, so they
  // are functions of their own though no call, prologue or padding marks
  // them.  A catch block's `mov eax, offset continuation; ret` only loads the
  // address it hands back, which stays the label it names.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  Emit({0x55, 0x8B, 0xEC});             // 401000 push ebp; mov ebp, esp
  Emit({0x68, 0x14, 0x10, 0x40, 0x00}); // 401003 push 0x401014
  Emit({0x68, 0x17, 0x10, 0x40, 0x00}); // 401008 push 0x401017
  Emit({0xE8, 0x1E, 0x00, 0x00, 0x00}); // 40100D call 0x401030
  Emit({0x5D, 0xC3});                   // 401012 pop ebp; ret
  Emit({0x8B, 0xC1, 0xC3});             // 401014 mov eax, ecx; ret
  Emit({0x33, 0xC0, 0xC3});             // 401017 xor eax, eax; ret
  Text.resize(0x30, 0xCC);
  Emit({0x55, 0x8B, 0xEC});             // 401030 push ebp; mov ebp, esp
  Emit({0xE8, 0x08, 0x00, 0x00, 0x00}); // 401033 call 0x401040
  Emit({0x5D, 0xC2, 0x08, 0x00});       // 401038 pop ebp; ret 8
  Text.resize(0x40, 0xCC);
  Emit({0xB8, 0x47, 0x10, 0x40, 0x00}); // 401040 mov eax, 0x401047
  Emit({0xC3, 0xCC});                   // 401045 ret
  Emit({0x33, 0xC0, 0xC3});             // 401047 xor eax, eax; ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  for (const auto [Field, Target] : {std::pair<va_t, va_t>{0x401004, 0x401014},
                                     std::pair<va_t, va_t>{0x401009, 0x401017},
                                     std::pair<va_t, va_t>{0x401041, 0x401047}})
    Img.CodeAddressRelocOperands.emplace(
        Field, RelocatedAddressField{.EncodedValue = Target,
                                     .TargetVA = Target,
                                     .Width = 4,
                                     .TargetOwnerVA = Target});

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  for (va_t Entry : {0x401000u, 0x401030u, 0x401040u})
    EXPECT_TRUE(hasEntry(Functions, Entry)) << std::hex << Entry;
  EXPECT_TRUE(hasEntry(Functions, 0x401014)) << "the pushed constructor";
  EXPECT_TRUE(hasEntry(Functions, 0x401017)) << "the pushed destructor";
  EXPECT_FALSE(hasEntry(Functions, 0x401047)) << "a catch's continuation";
}

TEST(X86_32_FunctionDiscovery, SwitchTableAmongCodeBelongsToItsFunction) {
  BinaryImage Img = makeSwitchTableAmongCode();
  // The table is data the code reads, though .text holds it; the two fields
  // of one instruction stay that instruction's operands.
  EXPECT_TRUE(Img.DataInCode.count(0x40102E));
  for (va_t Slot : {0x40102Eu, 0x401032u, 0x401036u}) {
    EXPECT_TRUE(Img.CodePtrRelocSlots.count(Slot)) << std::hex << Slot;
    EXPECT_FALSE(Img.CodeAddressRelocOperands.count(Slot)) << std::hex << Slot;
  }
  EXPECT_TRUE(Img.DataAddressRelocOperands.count(0x40100E));
  EXPECT_TRUE(Img.DataAddressRelocOperands.count(0x401042));
  EXPECT_TRUE(Img.CodeAddressRelocOperands.count(0x401046));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  EXPECT_TRUE(hasEntry(Functions, 0x401040));
  for (va_t Case : {0x401012u, 0x401019u, 0x401020u})
    EXPECT_FALSE(hasEntry(Functions, Case))
        << "0x" << std::hex << Case << " is a case of the switch whose table "
        << "follows its code";

  std::set<va_t> Entries;
  for (const auto &F : Functions)
    Entries.insert(F.first);
  CFGBuilder CFG;
  CFG.setKnownFuncEntries(&Entries);
  const LowFunc Switch = CFG.build(Img, Dec, 0x401000, "switch_on_arg");
  ASSERT_EQ(Switch.JumpTables.size(), 1u)
      << "an aligned anchor past the misaligned table does not end it";
  EXPECT_EQ(Switch.JumpTables.front().BaseAddr, 0x40102Eu);
  EXPECT_EQ(std::set<va_t>(Switch.JumpTables.front().Targets.begin(),
                           Switch.JumpTables.front().Targets.end()),
            (std::set<va_t>{0x401012, 0x401019, 0x401020}));
}

TEST(X86_32_FunctionDiscovery, DataSegmentDispatchKeepsIndependentCallbacks) {
  for (bool IndependentCase : {false, true}) {
    SCOPED_TRACE(IndependentCase);
    BinaryImage Img = makeSwitchTableAmongCode();
    constexpr va_t Table = 0x404000;
    std::vector<uint8_t> Bytes(12);
    for (unsigned I = 0; I != 3; ++I) {
      const va_t OldSlot = 0x40102E + I * 4;
      const uint32_t Target = readLE<uint32_t>(Img.readVA(OldSlot, 4));
      writeLE<uint32_t>(Bytes.data() + I * 4, Target);
      Img.CodePtrRelocSlots.erase(OldSlot);
    }
    std::fill_n(Img.Segments[0].Data.begin() + 0x2E, 12, 0xCC);
    Img.DataInCode.erase(0x40102E);
    Img.RelCodeTableAnchors.erase(0x40102E);
    Img.DataAddressRelocOperands.erase(0x40100E);
    writeLE<uint32_t>(Img.Segments[0].Data.data() + 0x0E, Table);
    addPeSection(Img, ".rdata.switch", Table, std::move(Bytes),
                 SegmentFlags::Readable);
    recordAbsolutePointerRelocations(Img, {{0x40100E, Table, InvalidVA},
                                           {Table, 0x401012, InvalidVA},
                                           {Table + 4, 0x401019, InvalidVA},
                                           {Table + 8, 0x401020, InvalidVA}});
    if (IndependentCase) {
      addPeSection(Img, ".rdata.callback", 0x405000, {0x19, 0x10, 0x40, 0x00},
                   SegmentFlags::Readable);
      recordAbsolutePointerRelocations(Img, {{0x405000, 0x401019, InvalidVA}});
    }
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    const auto Functions = FuncDetector().detect(Img, Dec);
    EXPECT_TRUE(hasEntry(Functions, 0x401000));
    EXPECT_TRUE(hasEntry(Functions, 0x401040))
        << "an ordinary relocated data read is not a jump dispatch";
    if (IndependentCase)
      EXPECT_TRUE(hasEntry(Functions, 0x401019))
          << "a separate callback slot supplies independent entry evidence";
    else
      for (va_t Case : {0x401012u, 0x401019u, 0x401020u})
        EXPECT_FALSE(hasEntry(Functions, Case)) << std::hex << Case;
  }
}

TEST(X86_32_FunctionDiscovery, SwitchCasesStayLabelsPastAnEntryBeforeTheTable) {
  // MSVC's seven-byte cases set a result and jump to a shared tail.  The
  // table follows an aligned prologue match in dead code past that tail.
  // The cases still belong to the dispatcher reading the table, so they are
  // its labels, though that entry lies between them and the table's slots.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  Emit({0x55, 0x8B, 0xEC});                         // push ebp; mov ebp, esp
  Emit({0x8B, 0x45, 0x08});                         // mov eax, [ebp+8]
  Emit({0x83, 0xF8, 0x02, 0x77, 0x1C});             // cmp eax, 2; ja 0x401027
  Emit({0xFF, 0x24, 0x85, 0x3C, 0x10, 0x40, 0x00}); // jmp [eax*4+0x40103C]
  Emit({0xB8, 0x0A, 0x00, 0x00, 0x00, 0xEB, 0x10}); // 0x401012: case 0
  Emit({0xB8, 0x14, 0x00, 0x00, 0x00, 0xEB, 0x09}); // 0x401019: case 1
  Emit({0xB8, 0x1E, 0x00, 0x00, 0x00, 0xEB, 0x02}); // 0x401020: case 2
  Emit({0x33, 0xC0});                               // 0x401027: default
  Emit({0x5D, 0xC3});                               // 0x401029: the tail
  Emit({0xCC, 0xCC, 0xCC, 0xCC, 0xCC});             //
  Emit({0x56, 0x57, 0x8B, 0x44, 0x24, 0x0C});       // 0x401030: dead code
  Emit({0x5F, 0x5E, 0xC3, 0xCC, 0xCC, 0xCC});       //
  Emit({0x12, 0x10, 0x40, 0x00, 0x19, 0x10});       // 0x40103C: the table
  Emit({0x40, 0x00, 0x20, 0x10, 0x40, 0x00});       //
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  recordAbsolutePointerRelocations(Img, {{0x40100E, 0x40103C, InvalidVA},
                                         {0x40103C, 0x401012, InvalidVA},
                                         {0x401040, 0x401019, InvalidVA},
                                         {0x401044, 0x401020, InvalidVA}});
  coff_loader::recordRelocatedPointerRunsInCode(Img);
  ASSERT_TRUE(Img.DataInCode.count(0x40103C));
  ASSERT_TRUE(Img.DataAddressRelocOperands.count(0x40100E));

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Arch::X86));
  FuncDetector Detector;
  const auto Functions = Detector.detect(Img, Dec);
  EXPECT_TRUE(hasEntry(Functions, 0x401000));
  ASSERT_TRUE(hasEntry(Functions, 0x401030))
      << "the entry between the cases and the table";
  for (va_t Case : {0x401012u, 0x401019u, 0x401020u})
    EXPECT_FALSE(hasEntry(Functions, Case))
        << "0x" << std::hex << Case << " is a case of the dispatcher";
}

TEST(X86_32_FunctionDiscovery, SwitchTableAmongCodeLiftsPastTheMirrorLimit) {
  // The table follows its dispatcher in a .text larger than the LLVM emitter
  // mirrors as one global.  The recovered switch dispatches on its selector
  // and omits the table LOAD, so the address that LOAD would read, and the
  // table's base in it, are never formed and need no global.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  Emit({0x55, 0x8B, 0xEC});                         // push ebp; mov ebp, esp
  Emit({0x8B, 0x45, 0x08});                         // mov eax, [ebp+8]
  Emit({0x83, 0xF8, 0x02, 0x77, 0x1D});             // cmp eax, 2; ja 0x401028
  Emit({0xFF, 0x24, 0x85, 0x2E, 0x10, 0x40, 0x00}); // jmp [eax*4+0x40102E]
  Emit({0xB8, 0x0A, 0x00, 0x00, 0x00, 0x5D, 0xC3}); // 0x401012: case 0
  Emit({0xB8, 0x14, 0x00, 0x00, 0x00, 0x5D, 0xC3}); // 0x401019: case 1
  Emit({0xB8, 0x1E, 0x00, 0x00, 0x00, 0x5D, 0xC3}); // 0x401020: case 2
  Emit({0x90, 0x33, 0xC0, 0x5D, 0xC3});             // nop; 0x401028: default
  Emit({0x8B, 0xC0});                               // mov eax, eax
  Emit({0x12, 0x10, 0x40, 0x00, 0x19, 0x10});       // 0x40102E: the table
  Emit({0x40, 0x00, 0x20, 0x10, 0x40, 0x00});       //
  Text.resize(limits::kMaxSingleGlobalEmbedLen + 0x1000, 0xCC);
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  recordAbsolutePointerRelocations(Img, {{0x40100E, 0x40102E, InvalidVA},
                                         {0x40102E, 0x401012, InvalidVA},
                                         {0x401032, 0x401019, InvalidVA},
                                         {0x401036, 0x401020, InvalidVA}});
  coff_loader::recordRelocatedPointerRunsInCode(Img);
  ASSERT_TRUE(Img.DataInCode.count(0x40102E));

  // A large module's arbitration runs out of budget and leaves no slot
  // suppressible; a budget of zero does the same here.
  for (std::optional<size_t> Budget :
       {std::optional<size_t>(), std::optional<size_t>(0)})
    for (bool NoOpt : {true, false}) {
      SCOPED_TRACE(NoOpt);
      SCOPED_TRACE(Budget.has_value());
      llvm::LLVMContext Ctx;
      PipelineOptions Opts;
      Opts.EmitDumpOutput = false;
      Opts.LiftMode = true;
      Opts.NoOpt = NoOpt;
      Opts.JumpTableEvidenceBudgetForTesting = Budget;
      Opts.OnlyFunctionEntries = {0x401000};
      const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_NE(Result.LlvmModule, nullptr);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      if (!NoOpt)
        continue; // the optimizer folds the cases' constants into arithmetic
      bool Dispatches = false;
      for (const llvm::Function &Function : *Result.LlvmModule)
        for (const llvm::BasicBlock &Block : Function)
          if (const auto *Switch = llvm::dyn_cast_or_null<llvm::SwitchInst>(
                  Block.getTerminator()))
            Dispatches |= Switch->getNumCases() == 3;
      EXPECT_TRUE(Dispatches) << "the three cases dispatch through a switch";
    }
}

TEST(X86_32_FunctionDiscovery, ANarrowLoadBaseAmongCodeIsData) {
  // A two-level switch reads a byte index table laid among the code
  // (`movzx eax, byte [eax + table]`) before the pointer-wide jump table it
  // indexes.  A base relocation records the byte table's displacement, and it
  // lands in `.text`, but a sub-pointer load cannot read a code pointer, so
  // the operand names data, not a code address.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.LoadsAtLinkAddress = true;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  // movzx eax, byte [eax + 0x401010]   (0F B6 80 10 10 40 00)
  // mov eax, dword [eax*4 + 0x401014]  (8B 04 85 14 10 40 00)
  // ret
  std::vector<uint8_t> Text = {0x0F, 0xB6, 0x80, 0x10, 0x10, 0x40, 0x00, 0x8B,
                               0x04, 0x85, 0x14, 0x10, 0x40, 0x00, 0xC3};
  Text.resize(0x40, 0xCC);
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);

  Decoder Dec;
  ASSERT_TRUE(Dec.init(Img));
  const auto Narrow = instructionRelocations(
      Img, decodeAt(Img, Dec, 0x401000)); // the movzx: byte-wide load
  const auto Wide = instructionRelocations(
      Img, decodeAt(Img, Dec, 0x401007)); // the jmp table: pointer-wide load
  ASSERT_EQ(Narrow.Addresses.size(), 1u);
  ASSERT_EQ(Wide.Addresses.size(), 1u);
  EXPECT_EQ(Narrow.Addresses.front().Provenance,
            ConstantAddressProvenance::DataAddress)
      << "a byte-load base in .text is data, not a code address";
  EXPECT_EQ(Wide.Addresses.front().Provenance,
            ConstantAddressProvenance::CodeAddress)
      << "the pointer-wide jump table stays a code address";
}

TEST(X86_32_FunctionDiscovery, InCodePointerTableMirrorsPastTheLimit) {
  // A function-pointer table MSVC lays among its code, read as data, in a
  // .text larger than the emitter mirrors as one global.  The whole segment
  // cannot be a single global, but the table's own run (three code-pointer
  // slots) can: the read resolves through that run's mirror instead of
  // refusing because the segment is too large.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  // 0x401000: load the second table slot as data and return it.
  Emit({0xA1, 0x54, 0x10, 0x40, 0x00, 0xC3}); // mov eax, [0x401054]; ret
  Text.resize(0x10, 0xCC);
  Emit({0x33, 0xC0, 0xC3}); // 0x401010 case0: xor eax, eax; ret
  Text.resize(0x18, 0xCC);
  Emit({0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3}); // 0x401018 case1: mov eax,1; ret
  Text.resize(0x20, 0xCC);
  Emit({0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3}); // 0x401020 case2: mov eax,2; ret
  Text.resize(0x50, 0xCC);
  // 0x401050: three consecutive relocated code-pointer slots (a run).
  for (uint32_t T : {0x401010u, 0x401018u, 0x401020u})
    for (unsigned B = 0; B < 4; ++B)
      Text.push_back(static_cast<uint8_t>(T >> (8 * B)));
  Text.resize(limits::kMaxSingleGlobalEmbedLen + 0x1000, 0xCC);
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  recordAbsolutePointerRelocations(Img, {{0x401001, 0x401054, InvalidVA},
                                         {0x401050, 0x401010, InvalidVA},
                                         {0x401054, 0x401018, InvalidVA},
                                         {0x401058, 0x401020, InvalidVA}});
  coff_loader::recordRelocatedPointerRunsInCode(Img);
  ASSERT_TRUE(Img.DataInCode.count(0x401050));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries = {0x401000, 0x401010, 0x401018, 0x401020};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
  // The read resolves through a mirror global, not a stale load of 0x401054.
  std::string Text2;
  llvm::raw_string_ostream OS(Text2);
  Result.LlvmModule->print(OS, nullptr);
  EXPECT_EQ(Text2.find("inttoptr (i32 4198484"), std::string::npos) << Text2;
  EXPECT_NE(Text2.find("getelementptr"), std::string::npos) << Text2;
}

TEST(X86_32_FunctionDiscovery, ARuntimeIndexedReadOnlyTableResolves) {
  // `table[obj->field]`: a live-in object pointer in EBX, its field loaded at
  // pointer width, scaled by the int stride and added to a read-only .rdata
  // table base, as GOF2's sub_4028D4 does.  The index is an unbounded runtime
  // value, so the offset is unprovable, but the base is a genuine read-only
  // table: the read resolves through the .rdata run mirror by the base's own
  // offset plus the runtime index, rather than refusing as an ambiguous
  // read-only table base.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  // EBX is a live-in with no def, so prove() admits the field load as an
  // opaque runtime word, exactly GOF2's shape.
  Emit({0x8B, 0x43, 0x58});                         // mov eax, [ebx+0x58]
  Emit({0x8B, 0x04, 0x85, 0x00, 0x20, 0x40, 0x00}); // mov eax, [eax*4+0x402000]
  Emit({0xC3});                                     // ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  std::vector<uint8_t> Rdata;
  for (uint32_t Word : {3u, 1u, 4u, 1u, 5u, 9u, 2u, 6u})
    for (unsigned Byte = 0; Byte < 4; ++Byte)
      Rdata.push_back(static_cast<uint8_t>(Word >> (8 * Byte)));
  addPeSection(Img, ".rdata", 0x402000, std::move(Rdata),
               SegmentFlags::Readable);
  // The base relocation of the displacement operand at 0x401006.
  recordAbsolutePointerRelocations(Img, {{0x401006, 0x402000, InvalidVA}});

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries = {0x401000};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
  // The read lands in a real read-only global addressed by a GEP, not a
  // reconstructed pointer from the stale absolute table VA (0x402000).
  std::string Text2;
  llvm::raw_string_ostream OS(Text2);
  Result.LlvmModule->print(OS, nullptr);
  EXPECT_EQ(Text2.find("inttoptr (i32 4202496"), std::string::npos) << Text2;
  EXPECT_EQ(Text2.find("4202496"), std::string::npos)
      << "the table base VA must not survive as a constant\n"
      << Text2;
  // A .rdata run global exists and the load GEPs into it.
  const bool HasRodataGlobal = Text2.find("@") != std::string::npos &&
                               Text2.find(".rodata") != std::string::npos;
  EXPECT_TRUE(HasRodataGlobal) << Text2;
  EXPECT_NE(Text2.find("getelementptr i8"), std::string::npos) << Text2;
}

TEST(X86_32_FunctionDiscovery, ARuntimeOrStaticPointerReadsThroughTheMirror) {
  // `p = arg ? arg : &empty; return *p`, the default a static object in a
  // .data that also holds a relocated code pointer, so the emitter mirrors
  // it.  The static arm is the mirror's own pointer and the runtime arm the
  // caller's, so the read needs no table proof and keeps no image address.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  Emit({0x8B, 0x44, 0x24, 0x04});       // 401000 mov eax, [esp+4]
  Emit({0x85, 0xC0, 0x75, 0x05});       // 401004 test eax, eax; jne 40100D
  Emit({0xB8, 0x04, 0x30, 0x40, 0x00}); // 401008 mov eax, offset empty
  Emit({0x0F, 0xB7, 0x00, 0xC3});       // 40100D movzx eax, word [eax]; ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  addPeSection(Img, ".data", 0x403000,
               {0x00, 0x10, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00},
               SegmentFlags::Readable | SegmentFlags::Writable);
  recordAbsolutePointerRelocations(
      Img, {{0x401009, 0x403004, InvalidVA}, {0x403000, 0x401000, InvalidVA}});

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries = {0x401000};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Result.LlvmModule->print(OS, nullptr);
  EXPECT_EQ(IR.find("4206596"), std::string::npos) << IR; // 0x403004
}

TEST(X86_32_FunctionDiscovery, AMergeOfMirroredAndReadOnlyStaticsResolvesEach) {
  // The same merge also chooses a string in a .rdata that holds no pointer
  // (`p = a ? &empty : b ? "x" : arg`).  The string's relocated operand
  // proves it an address, which its own route rebuilds, so the read keeps
  // neither static's image address.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  Emit({0x8B, 0x44, 0x24, 0x04});       // 401000 mov eax, [esp+4]
  Emit({0x8B, 0x4C, 0x24, 0x08});       // 401004 mov ecx, [esp+8]
  Emit({0x85, 0xC9, 0x74, 0x0E});       // 401008 test ecx, ecx; je 40101A
  Emit({0x83, 0xF9, 0x01, 0x75, 0x0E}); // 40100C cmp ecx, 1; jne 40101F
  Emit({0xB8, 0x00, 0x20, 0x40, 0x00}); // 401011 mov eax, offset string
  Emit({0xEB, 0x07, 0x90, 0x90});       // 401016 jmp 40101F
  Emit({0xB8, 0x04, 0x30, 0x40, 0x00}); // 40101A mov eax, offset empty
  Emit({0x0F, 0xB7, 0x00, 0xC3});       // 40101F movzx eax, word [eax]; ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  addPeSection(Img, ".rdata", 0x402000, {'x', 0x00, 0x00, 0x00},
               SegmentFlags::Readable);
  addPeSection(Img, ".data", 0x403000,
               {0x00, 0x10, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00},
               SegmentFlags::Readable | SegmentFlags::Writable);
  recordAbsolutePointerRelocations(Img, {{0x401012, 0x402000, InvalidVA},
                                         {0x40101B, 0x403004, InvalidVA},
                                         {0x403000, 0x401000, InvalidVA}});

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries = {0x401000};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Result.LlvmModule->print(OS, nullptr);
  EXPECT_EQ(IR.find("4202496"), std::string::npos) << IR; // 0x402000
  EXPECT_EQ(IR.find("4206596"), std::string::npos) << IR; // 0x403004
}

TEST(X86_32_FunctionDiscovery, AMergeWithAnUnprovedImageAddressKeepsItsProof) {
  // As above, but no relocation proves the string's operand an address: it
  // is a number that lands in the image, where it would stand as an
  // original address.  The merge alone then proves nothing, and the read
  // never keeps that address.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  Emit({0x8B, 0x44, 0x24, 0x04});       // 401000 mov eax, [esp+4]
  Emit({0x8B, 0x4C, 0x24, 0x08});       // 401004 mov ecx, [esp+8]
  Emit({0x85, 0xC9, 0x74, 0x0E});       // 401008 test ecx, ecx; je 40101A
  Emit({0x83, 0xF9, 0x01, 0x75, 0x0E}); // 40100C cmp ecx, 1; jne 40101F
  Emit({0xB8, 0x00, 0x20, 0x40, 0x00}); // 401011 mov eax, offset string
  Emit({0xEB, 0x07, 0x90, 0x90});       // 401016 jmp 40101F
  Emit({0xB8, 0x04, 0x30, 0x40, 0x00}); // 40101A mov eax, offset empty
  Emit({0x0F, 0xB7, 0x00, 0xC3});       // 40101F movzx eax, word [eax]; ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  addPeSection(Img, ".rdata", 0x402000, {'x', 0x00, 0x00, 0x00},
               SegmentFlags::Readable);
  addPeSection(Img, ".data", 0x403000,
               {0x00, 0x10, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00},
               SegmentFlags::Readable | SegmentFlags::Writable);
  recordAbsolutePointerRelocations(
      Img, {{0x40101B, 0x403004, InvalidVA}, {0x403000, 0x401000, InvalidVA}});

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries = {0x401000};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  if (!Result.Success)
    return;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Result.LlvmModule->print(OS, nullptr);
  EXPECT_EQ(IR.find("4202496"), std::string::npos) << IR; // 0x402000
  EXPECT_EQ(IR.find("4206596"), std::string::npos) << IR; // 0x403004
}

TEST(X86_32_FunctionDiscovery, APointerSpilledAcrossALoopsCallsKeepsItsTable) {
  // A loop walks an array of integers in .rdata with a pointer it keeps in a
  // frame slot across the call in its body: `add dword ptr [ebp-4], 4`.  The
  // slot holds the array's address or itself advanced, so every read through
  // it reads the array, which the LLVM emitter mirrors with the code pointer
  // beside it.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  Emit({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x08}); // push ebp; mov ebp, esp; sub
  Emit({0xC7, 0x45, 0xFC, 0x00, 0x20, 0x40, 0x00}); // mov [ebp-4], 0x402000
  Emit({0xC7, 0x45, 0xF8, 0x00, 0x00, 0x00, 0x00}); // mov [ebp-8], 0
  Emit({0x8B, 0x45, 0xFC});                   // 0x401014: mov eax, [ebp-4]
  Emit({0x8B, 0x00, 0x50});                   // mov eax, [eax]; push eax
  Emit({0xE8, 0x21, 0x00, 0x00, 0x00});       // call 0x401040
  Emit({0x83, 0xC4, 0x04});                   // add esp, 4
  Emit({0x83, 0x45, 0xFC, 0x04});             // add [ebp-4], 4
  Emit({0xFF, 0x45, 0xF8});                   // inc [ebp-8]
  Emit({0x83, 0x7D, 0xF8, 0x03, 0x7C, 0xE5}); // cmp [ebp-8], 3; jl
  Emit({0x8B, 0xE5, 0x5D, 0xC3});             // mov esp, ebp; pop; ret
  Text.resize(0x40, 0xCC);
  Emit({0x8B, 0x44, 0x24, 0x04, 0xC3}); // 0x401040: mov eax, [esp+4]; ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  addPeSection(Img, ".rdata", 0x402000,
               {0x05, 0, 0, 0, 0x0F, 0, 0, 0, 0x1E, 0, 0, 0, // the integers
                0x40, 0x10, 0x40, 0x00},                     // &0x401040
               SegmentFlags::Readable);
  recordAbsolutePointerRelocations(
      Img, {{0x401009, 0x402000, InvalidVA}, {0x40200C, 0x401040, InvalidVA}});
  ASSERT_TRUE(Img.CodePtrRelocSlots.count(0x40200C));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries = {0x401000, 0x401040};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
}

TEST(X86_32_FunctionDiscovery, ANestedLoopsSpilledIndexStaysAnOffset) {
  // An outer loop keeps its index in a frame slot and an inner loop its
  // record pointer, `&table[i]` advanced by 4, across a call that takes a
  // pushed argument.  Each loop header merges ESP, and the inner loop's merge
  // lies on the outer one's back edge; every value it merges is the frame's
  // own ESP, so the pushes stay frame slots apart from the two spills.
  BinaryImage Img;
  Img.Arch = Arch::X86;
  Img.Bits = Bitness::Bits32;
  Img.Format = BinaryFormat::COFF;
  Img.IsRelocatable = false;
  Img.Base = 0x400000;
  Img.Entry = 0x401000;
  std::vector<uint8_t> Text;
  auto Emit = [&](std::initializer_list<uint8_t> Bytes) {
    Text.insert(Text.end(), Bytes);
  };
  Emit({0x55, 0x8B, 0xEC, 0x83, 0xEC, 0x0C}); // push ebp; mov ebp, esp; sub
  Emit({0xC7, 0x45, 0xFC, 0x00, 0x00, 0x00, 0x00}); // mov [ebp-4], 0
  Emit({0x8B, 0x45, 0xFC});             // 0x40100D: mov eax, [ebp-4]
  Emit({0x6B, 0xC0, 0x0C});             // imul eax, eax, 12
  Emit({0x05, 0x00, 0x20, 0x40, 0x00}); // add eax, 0x402000
  Emit({0x89, 0x45, 0xF8});             // mov [ebp-8], eax
  Emit({0xC7, 0x45, 0xF4, 0x00, 0x00, 0x00, 0x00}); // mov [ebp-0Ch], 0
  Emit({0x8B, 0x45, 0xF8});                   // 0x401022: mov eax, [ebp-8]
  Emit({0x8B, 0x00, 0x50});                   // mov eax, [eax]; push eax
  Emit({0xE8, 0x33, 0x00, 0x00, 0x00});       // call 0x401060
  Emit({0x83, 0xC4, 0x04});                   // add esp, 4
  Emit({0x83, 0x45, 0xF8, 0x04});             // add [ebp-8], 4
  Emit({0xFF, 0x45, 0xF4});                   // inc [ebp-0Ch]
  Emit({0x83, 0x7D, 0xF4, 0x03, 0x7C, 0xE5}); // cmp [ebp-0Ch], 3; jl
  Emit({0xFF, 0x45, 0xFC});                   // inc [ebp-4]
  Emit({0x83, 0x7D, 0xFC, 0x02, 0x7C, 0xC7}); // cmp [ebp-4], 2; jl
  Emit({0x8B, 0xE5, 0x5D, 0xC3});             // mov esp, ebp; pop; ret
  Text.resize(0x60, 0xCC);
  Emit({0x8B, 0x44, 0x24, 0x04, 0xC3}); // 0x401060: mov eax, [esp+4]; ret
  addPeSection(Img, ".text", 0x401000, std::move(Text),
               SegmentFlags::Readable | SegmentFlags::Executable);
  // Two records of three integers, then the helper's address.
  std::vector<uint8_t> Rdata;
  for (uint32_t Word : {5u, 15u, 30u, 7u, 8u, 9u, 0x401060u})
    for (unsigned Byte = 0; Byte < 4; ++Byte)
      Rdata.push_back(static_cast<uint8_t>(Word >> (8 * Byte)));
  addPeSection(Img, ".rdata", 0x402000, std::move(Rdata),
               SegmentFlags::Readable);
  recordAbsolutePointerRelocations(
      Img, {{0x401014, 0x402000, InvalidVA}, {0x402018, 0x401060, InvalidVA}});
  ASSERT_TRUE(Img.CodePtrRelocSlots.count(0x402018));

  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.LiftMode = true;
  Opts.OnlyFunctionEntries = {0x401000, 0x401060};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_NE(Result.LlvmModule, nullptr);
  EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
}
