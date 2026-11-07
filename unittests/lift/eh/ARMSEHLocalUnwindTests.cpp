//===- ARMSEHLocalUnwindTests.cpp - Windows ARM local unwinds -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Windows ARM `__jump_unwind` exits and the registers its termination handlers
/// read: the target names only a Thumb code address, in a `movw`/`movt` pair or
/// a literal pool, the frame comes from the frame register, and an `__except`
/// arm restores that register for what follows it.
///
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/decode/Decoder.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/ARM/ARMRegs.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t Entry = 0x401000;
constexpr va_t Foo = Entry + 0x60;
constexpr va_t Stub = Entry + 0x70;
constexpr va_t Funclet = Entry + 0x80;
constexpr va_t ImportSlot = 0x402000;

/// xcpt4's shapes as MSVC ARM compiles them.  Each homes its arguments with
/// `push {r0, r1}`, so r7 + 0xc holds Counter, except where noted.
enum class Shape {
  /// `__try { if (Flag) return; foo(); } __finally { ... }` with the target in
  /// a `movw`/`movt` pair: the handler stores 99 on an unwind, the normal path
  /// keeps its copy, which stores 100.
  Movw,
  /// The same with the target in a literal pool, as /O0 loads it.
  Literal,
  /// The handler returns from the `__finally`: its own `__jump_unwind`.
  HandlerUnwinds,
  /// A GS prologue: `bl __security_push_cookie` allocates four bytes more than
  /// the body's own arithmetic shows.
  Cookie,
  /// An `__except` arm rejoins before a later `__try`: r4 is a PHI there, of
  /// arms that hold one value.
  ExceptJoin,
};

struct ShapeCode {
  std::vector<uint16_t> Parent;
  std::vector<uint16_t> Handler;
  /// Past the parent's own code, as a helper the shape calls.
  std::vector<uint16_t> Helper;
  uint32_t GuardBegin = 0;
  uint32_t GuardEnd = 0;
  /// The ARM unwind codes, innermost first: what `mov r7, sp` and the
  /// allocations fix.
  std::vector<uint64_t> Allocations;
  uint32_t End = 0;
};

ShapeCode codeOf(Shape S) {
  // Handler: cbz r0, done; ldr r2, [r7, #0xc]; movs r3, #99; str r3, [r2]
  //          done: bx lr
  const std::vector<uint16_t> Stores99{0xb110, 0x68fa, 0x2363, 0x6013, 0x4770};
  switch (S) {
  case Shape::Movw:
    // push {r0, r1}; push {r7, lr}; mov r7, sp
    // cbz r0, work; movw r1, #0x1027; movt r1, #0x40; add.w r0, r7, #0x10;
    // bl stub
    // work: bl foo; nop
    // ldr r2, [r7, #0xc]; movs r3, #100; str r3, [r2]; nop
    // target: pop {r7}; ldr pc, [sp], #12
    return {{0xb403, 0xb580, 0x466f, 0xb138, 0xf241, 0x0127, 0xf2c0, 0x0140,
             0xf107, 0x0010, 0xf000, 0xf82c, 0xf000, 0xf822, 0xbf00, 0x68fa,
             0x2364, 0x6013, 0xbf00, 0xbc80, 0xf85d, 0xfb0c},
            Stores99,
            {},
            0x06,
            0x1e,
            {8, 8},
            0x2c};
  case Shape::Literal:
    // ... cbz r0, work; ldr r1, lit; add.w r0, r7, #0x10; bl stub ...
    // lit: .long 0x401021
    return {{0xb403, 0xb580, 0x466f, 0xb120, 0x4907, 0xf107, 0x0010, 0xf000,
             0xf82f, 0xf000, 0xf825, 0xbf00, 0x68fa, 0x2364, 0x6013, 0xbf00,
             0xbc80, 0xf85d, 0xfb0c, 0xbf00, 0x1021, 0x0040},
            Stores99,
            {},
            0x06,
            0x18,
            {8, 8},
            0x26};
  case Shape::HandlerUnwinds:
    // push {r0, r1}; push {r7, lr}; mov r7, sp; bl foo; nop
    // ldr r2, [r7, #0xc]; movs r3, #100; str r3, [r2]; nop
    // target: pop {r7}; ldr pc, [sp], #12
    // Handler: push {r4, lr}; cbz r0, done; ldr r2, [r7, #0xc]; movs r3, #99;
    // str r3, [r2]; movw r1, #0x1015; movt r1, #0x40; add.w r0, r7, #0x10;
    // bl stub; done: pop {r4, pc}
    return {{0xb403, 0xb580, 0x466f, 0xf000, 0xf82b, 0xbf00, 0x68fa, 0x2364,
             0x6013, 0xbf00, 0xbc80, 0xf85d, 0xfb0c},
            {0xb510, 0xb150, 0x68fa, 0x2363, 0x6013, 0xf241, 0x0115, 0xf2c0,
             0x0140, 0xf107, 0x0010, 0xf7ff, 0xffeb, 0xbd10},
            {},
            0x06,
            0x0c,
            {8, 8},
            0x1a};
  case Shape::Cookie:
    // push {r0, r1}; push {r7, lr}; bl helper; mov r7, sp
    // cbz r0, work; movw r1, #0x102b; movt r1, #0x40; add.w r0, r7, #0x14;
    // bl stub
    // work: bl foo; nop
    // ldr r2, [r7, #0x10]; movs r3, #100; str r3, [r2]; nop
    // target: add sp, #4; pop {r7}; ldr pc, [sp], #12
    // Handler: cbz r0, done; ldr r2, [r7, #0x10]; movs r3, #99; str r3, [r2]
    // done: bx lr
    // Helper: sub sp, #4; bx lr
    return {{0xb403, 0xb580, 0xf000, 0xf84c, 0x466f, 0xb138, 0xf241,
             0x012b, 0xf2c0, 0x0140, 0xf107, 0x0014, 0xf000, 0xf82a,
             0xf000, 0xf820, 0xbf00, 0x693a, 0x2364, 0x6013, 0xbf00,
             0xb001, 0xbc80, 0xf85d, 0xfb0c},
            {0xb110, 0x693a, 0x2363, 0x6013, 0x4770},
            {0xb081, 0x4770},
            0x0a,
            0x22,
            {4, 8, 8},
            0x32};
  case Shape::ExceptJoin:
    // push {r4, lr}; sub sp, #8; add r4, sp, #4; movs r0, #0; str r0, [r4]
    // bl foo; nop; b after
    // arm: movs r0, #1; str r0, [r4]
    // after: bl foo; nop
    // ldr r0, [r4]; add sp, #8; pop {r4, pc}
    // Handler: cbz r0, done; movs r1, #99; str r1, [r4]; done: bx lr
    return {{0xb510, 0xb082, 0xac01, 0x2000, 0x6020, 0xf000, 0xf829, 0xbf00,
             0xe001, 0x2001, 0x6020, 0xf000, 0xf823, 0xbf00, 0x6820, 0xb002,
             0xbd10},
            {0xb108, 0x2163, 0x6021, 0x4770},
            {},
            0x16,
            0x1c,
            {8, 8},
            0x22};
  }
  return {};
}

void place(std::vector<uint8_t> &Bytes, uint32_t At,
           const std::vector<uint16_t> &Halves) {
  std::memcpy(Bytes.data() + At, Halves.data(), Halves.size() * 2);
}

UnwindOperation unwindCode(UnwindOperationKind Kind, uint64_t Bytes = 0) {
  UnwindOperation Op;
  Op.Kind = Kind;
  Op.StackOffset = Bytes;
  return Op;
}

BinaryImage makeImage(Shape S) {
  const ShapeCode Code = codeOf(S);
  BinaryImage Img;
  Img.Arch = Arch::ARM;
  Img.Bits = Bitness::Bits32;
  Img.Mode = InstructionMode::Thumb;
  Img.Format = BinaryFormat::COFF;
  Img.Base = 0x400000;
  Img.Entry = Entry;

  std::vector<uint8_t> Bytes(0x100, 0);
  place(Bytes, 0, Code.Parent);
  // foo: bx lr
  place(Bytes, Foo - Entry, {0x4770});
  // stub: movw r12, #0x2000; movt r12, #0x40; ldr.w pc, [r12]
  place(Bytes, Stub - Entry, {0xf242, 0x0c00, 0xf2c0, 0x0c40, 0xf8dc, 0xf000});
  place(Bytes, Funclet - Entry, Code.Handler);
  if (!Code.Helper.empty())
    place(Bytes, 0xa0, Code.Helper);

  Segment Text;
  Text.Name = ".text";
  Text.VA = Entry;
  Text.Size = Bytes.size();
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Text.Data = Bytes;
  Img.Segments.push_back(std::move(Text));
  Section CodeSection;
  CodeSection.Name = ".text";
  CodeSection.VA = Entry;
  CodeSection.Size = Bytes.size();
  CodeSection.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Img.Sections.push_back(std::move(CodeSection));

  Segment Data;
  Data.Name = ".idata";
  Data.VA = ImportSlot;
  Data.Size = 8;
  Data.Flags = SegmentFlags::Readable;
  Data.Data.resize(8);
  Img.Segments.push_back(std::move(Data));
  Section DataSection;
  DataSection.Name = ".idata";
  DataSection.VA = ImportSlot;
  DataSection.Size = 8;
  DataSection.Flags = SegmentFlags::Readable;
  Img.Sections.push_back(std::move(DataSection));

  Img.Symbols.push_back(Symbol::makeFunc(Entry, Code.End));
  Img.Symbols.push_back(Symbol::makeFunc(Foo, 2));
  if (!Code.Helper.empty())
    Img.Symbols.push_back(Symbol::makeFunc(Entry + 0xa0, 4));
  Import Unwind;
  Unwind.Name = "__jump_unwind";
  Unwind.Module = "VCRUNTIME140.dll";
  Unwind.IATAddr = ImportSlot;
  Img.Imports.push_back(Unwind);
  EXPECT_TRUE(Img.recordImportStub(Stub, 0));

  ExceptionFunction EH;
  EH.CodeRange = {Entry, Entry + Code.End};
  EH.Encoding = ExceptionEncoding::ARM32Unpacked;
  EH.Personality = ExceptionPersonality::CSpecificHandler;
  EH.PrologueSize = Code.GuardBegin;
  if (S == Shape::ExceptJoin)
    EH.PrologueSize = 6;
  EH.UnwindOperations.push_back(
      unwindCode(UnwindOperationKind::SetStackPointerFromRegister));
  for (uint64_t Bytes : Code.Allocations)
    EH.UnwindOperations.push_back(
        unwindCode(UnwindOperationKind::AllocateStack, Bytes));
  EH.UnwindOperations.push_back(unwindCode(UnwindOperationKind::End));
  EH.SEH.emplace();
  if (S == Shape::ExceptJoin) {
    SEHScopeRecord Except;
    Except.GuardedRange = {Entry + 0x0a, Entry + 0x10};
    Except.Kind = SEHScopeKind::CatchAll;
    Except.HandlerVA = Entry + 0x12;
    Except.ContinuationVA = Entry + 0x12;
    EH.SEH->Scopes.push_back(Except);
  }
  SEHScopeRecord Finally;
  Finally.GuardedRange = {Entry + Code.GuardBegin, Entry + Code.GuardEnd};
  Finally.Kind = SEHScopeKind::Finally;
  Finally.FilterOrFinallyVA = Funclet;
  Finally.HandlerVA = Funclet;
  EH.SEH->Scopes.push_back(Finally);
  Img.ExceptionMetadata.Functions.push_back(std::move(EH));
  Img.ExceptionMetadata.rebuildIndex();
  return Img;
}

std::string decompileHighC(const BinaryImage &Img) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  auto Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions COpts;
  COpts.TheArch = Arch::ARM;
  COpts.Format = BinaryFormat::COFF;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COpts));
  OS.flush();
  return Source;
}

/// The parent's definition alone: the handler still prints standalone in a
/// whole-unit view.
std::string parentText(const std::string &Source) {
  const std::string Marker = "sub_401000(";
  size_t At = Source.find(Marker);
  while (At != std::string::npos) {
    const size_t Brace = Source.find_first_of(";{", At);
    if (Brace != std::string::npos && Source[Brace] == '{') {
      int Depth = 0;
      for (size_t I = Brace; I < Source.size(); ++I) {
        if (Source[I] == '{')
          ++Depth;
        else if (Source[I] == '}' && --Depth == 0)
          return Source.substr(At, I - At + 1);
      }
    }
    At = Source.find(Marker, At + 1);
  }
  return {};
}

size_t occurrences(const std::string &Text, const std::string &Needle) {
  size_t Count = 0;
  for (size_t At = Text.find(Needle); At != std::string::npos;
       At = Text.find(Needle, At + 1))
    ++Count;
  return Count;
}

void expectExitRunsTheClauseOnce(const std::string &P) {
  ASSERT_FALSE(P.empty());
  EXPECT_EQ(P.find("__jump_unwind"), std::string::npos) << P;
  EXPECT_EQ(P.find("sub_401080("), std::string::npos) << P;
  const size_t Try = P.find("__try {");
  const size_t Finally = P.find("} __finally {");
  ASSERT_NE(Try, std::string::npos) << P;
  ASSERT_NE(Finally, std::string::npos) << P;
  const size_t Jump = std::min(P.find("goto ", Try), P.find("return", Try));
  EXPECT_LT(Jump, Finally) << P;
  EXPECT_NE(P.find("if (AbnormalTermination())", Finally), std::string::npos)
      << P;
  EXPECT_EQ(occurrences(P, "= 99"), 1u) << P;
  EXPECT_GT(P.find("= 99"), Finally) << P;
  EXPECT_EQ(occurrences(P, "= 100"), 1u) << P;
  EXPECT_GT(P.find("= 100"), Finally) << P;
}

TEST(ARMSEHLocalUnwind, ATargetInMovwAndMovtIsAJumpRunningTheClauseOnce) {
  expectExitRunsTheClauseOnce(
      parentText(decompileHighC(makeImage(Shape::Movw))));
}

TEST(ARMSEHLocalUnwind, ATargetInALiteralPoolIsAJumpRunningTheClauseOnce) {
  expectExitRunsTheClauseOnce(
      parentText(decompileHighC(makeImage(Shape::Literal))));
}

TEST(ARMSEHLocalUnwind, ACookieHelperOutsideTheBodysArithmeticKeepsTheFrame) {
  // The frame the code names is the entry stack pointer: four bytes above
  // what the prologue's own instructions leave, for the slot the helper made.
  expectExitRunsTheClauseOnce(
      parentText(decompileHighC(makeImage(Shape::Cookie))));
}

TEST(ARMSEHLocalUnwind, AHandlersOwnUnwindIsAJumpOutOfItsClause) {
  const std::string Source = decompileHighC(makeImage(Shape::HandlerUnwinds));
  const std::string P = parentText(Source);
  ASSERT_FALSE(P.empty()) << Source;
  EXPECT_EQ(P.find("__jump_unwind"), std::string::npos) << P;
  const size_t Finally = P.find("} __finally {");
  ASSERT_NE(Finally, std::string::npos) << P;
  const size_t Guard = P.find("if (AbnormalTermination())", Finally);
  ASSERT_NE(Guard, std::string::npos) << P;
  const bool Leaves = P.find("goto ", Guard) != std::string::npos ||
                      P.find("return", Guard) != std::string::npos;
  EXPECT_TRUE(Leaves) << P;
  EXPECT_EQ(occurrences(P, "= 99"), 1u) << P;
}

MedFunc convertParent(const BinaryImage &Img) {
  Decoder Dec;
  EXPECT_TRUE(Dec.init(Arch::ARM, InstructionMode::Thumb));
  auto Low = CFGBuilder().build(Img, Dec, Img.Entry, "parent");
  return LowToMedConverter().convert(Low, Arch::ARM, BinaryFormat::COFF);
}

TEST(ARMSEHFinallyRegisters, AnExceptArmRejoiningLeavesTheRegistersValue) {
  // The arm's seed and the normal path's r4 join before the `__try`: two SSA
  // names of the frame address the prologue set.
  const MedFunc Med = convertParent(makeImage(Shape::ExceptJoin));
  const auto Registers = Med.SEHFuncletRegisters.find(Funclet);
  ASSERT_NE(Registers, Med.SEHFuncletRegisters.end());
  const auto R4 = Registers->second.find(armreg::R4);
  ASSERT_NE(R4, Registers->second.end());
  EXPECT_EQ(R4->second.ValueKind,
            SEHFuncletRegisterValue::Kind::EntryStackOffset);
  EXPECT_EQ(R4->second.Offset, -0xc);
}

TEST(ARMSEHFinallyBody, ATryAfterAnExceptArmFillsItsClause) {
  const std::string P =
      parentText(decompileHighC(makeImage(Shape::ExceptJoin)));
  ASSERT_FALSE(P.empty());
  const size_t Finally = P.find("} __finally {");
  ASSERT_NE(Finally, std::string::npos) << P;
  EXPECT_NE(P.find("if (AbnormalTermination())", Finally), std::string::npos)
      << P;
  EXPECT_NE(P.find("= 99;", Finally), std::string::npos) << P;
  EXPECT_EQ(P.find("unknown"), std::string::npos) << P;
}

} // namespace
