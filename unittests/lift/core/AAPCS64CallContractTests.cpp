//===- AAPCS64CallContractTests.cpp - AArch64 call arguments -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// AArch64 code reaches another module's routine through the pointer it loads
// from the slot the loader binds to that routine: a PLT entry's `adrp x16;
// ldr x17, [x16, #slot]; add x16, x16, #slot; br x17`, or a call built with
// -fno-plt.  The routine's prototype gives the arguments it takes, which a
// function passing its own on takes as its parameters.
//
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "WindowsMixedScalarFixture.h"
#include "WindowsVariadicFixture.h"
#include "gtest/gtest.h"

#include "neverd/Limits.h"
#include "neverd/backend/RewriteSourceIdentity.h"
#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/loader/FunctionDiscovery.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
#define AAPCS64_CALL_VALUE(Type, Name, Value)                                  \
  inline constexpr Type Name = Value;
#define AAPCS64_CALL_TEXT(Name, Text) inline constexpr char Name[] = Text;
#include "AAPCS64CallContractFixtures.def"

using namespace neverd;

constexpr va_t Text = 0x10000;
constexpr va_t Plt = 0x10100;
constexpr va_t Got = 0x20000;

uint32_t adrp(unsigned Rd, va_t At, va_t Target) {
  const uint32_t Pages =
      static_cast<uint32_t>((Target >> 12) - (At >> 12)) & 0x1FFFFF;
  return 0x90000000u | (Pages & 3) << 29 | (Pages >> 2) << 5 | Rd;
}
uint32_t ldrX(unsigned Rt, unsigned Rn, va_t Slot) {
  return 0xF9400000u | static_cast<uint32_t>(Slot & 0xFFF) / 8 << 10 | Rn << 5 |
         Rt;
}
uint32_t addX(unsigned Rd, unsigned Rn, va_t Slot) {
  return 0x91000000u | static_cast<uint32_t>(Slot & 0xFFF) << 10 | Rn << 5 | Rd;
}
uint32_t br(unsigned Rn) { return 0xD61F0000u | Rn << 5; }
uint32_t blr(unsigned Rn) { return 0xD63F0000u | Rn << 5; }
constexpr uint32_t PushFrame = 0xA9BF7BFDu; // stp x29, x30, [sp, #-16]!
constexpr uint32_t SetFrame = 0x910003FDu;  // mov x29, sp
constexpr uint32_t AddW0One = 0x11000400u;  // add w0, w0, #1
constexpr uint32_t PopFrame = 0xA8C17BFDu;  // ldp x29, x30, [sp], #16
constexpr uint32_t Ret = 0xD65F03C0u;

/// The canonical PLT entry at \p At for the import in \p Slot.
std::vector<uint32_t> pltEntry(va_t At, va_t Slot) {
  return {adrp(16, At, Slot), ldrX(17, 16, Slot), addX(16, 16, Slot), br(17)};
}

void place(Segment &Seg, va_t At, const std::vector<uint32_t> &Words) {
  for (size_t I = 0; I < Words.size(); ++I)
    for (unsigned B = 0; B < 4; ++B)
      Seg.Data[At - Seg.VA + I * 4 + B] =
          static_cast<uint8_t>(Words[I] >> (8 * B));
}

Segment codeSegment(const char *Name, va_t VA, size_t Size) {
  Segment Seg;
  Seg.Name = Name;
  Seg.VA = VA;
  Seg.Size = Seg.FileSz = Size;
  Seg.Data.assign(Size, 0);
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  return Seg;
}

/// An AArch64 ELF image with \p Code placed at its addresses, the function
/// symbols \p Functions, and \p Imports bound to consecutive GOT slots from
/// Got.  The loader binds the slot of a symbol no module of the image
/// defines; one the image defines (\p Defined) it relocates to the image's
/// own routine.  The PLT entries pair with their imports as the loader pairs
/// them.
BinaryImage
makeImage(const std::vector<std::pair<va_t, std::vector<uint32_t>>> &Code,
          const std::vector<std::pair<va_t, const char *>> &Functions,
          const std::vector<const char *> &Imports, bool Defined = false) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Segment TextSeg = codeSegment(".text", Text, Plt - Text);
  Segment PltSeg = codeSegment(".plt", Plt, 0x100);
  for (const auto &[At, Words] : Code)
    place(At < Plt ? TextSeg : PltSeg, At, Words);
  Img.Segments.push_back(std::move(TextSeg));
  Img.Segments.push_back(std::move(PltSeg));
  Segment GotSeg;
  GotSeg.Name = ".got";
  GotSeg.VA = Got;
  GotSeg.Size = GotSeg.FileSz = 8 * Imports.size();
  GotSeg.Data.assign(GotSeg.Size, 0);
  GotSeg.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(GotSeg));
  for (size_t I = 0; I < Imports.size(); ++I) {
    Img.Imports.push_back({"libc.so", Imports[I], 0, Got + 8 * I});
    if (!Defined)
      EXPECT_TRUE(Img.recordImportStorageSlot(
          Got + 8 * I, Imports[I], 0, ImportStorageEvidence::LoaderBind));
  }
  for (const auto &[Entry, Name] : Functions) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    Img.Symbols.push_back(std::move(Function));
  }
  scanImportThunks(Img);
  return Img;
}

PipelineResult run(const BinaryImage &Img, llvm::LLVMContext &Ctx,
                   std::set<va_t> Entries, bool LLVM = false) {
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = std::move(Entries);
  Opts.LiftMode = LLVM;
  return Pipeline().run(Img, Ctx, Opts);
}

/// HighC for the functions at \p Entries of \p Img.
std::string highC(const BinaryImage &Img, std::set<va_t> Entries) {
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, std::move(Entries));
  EXPECT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  return Source;
}

/// The text of function \p Name's definition, its signature included.
std::string body(const std::string &Source, const std::string &Name) {
  const size_t At = Source.find(" " + Name + "(");
  if (At == std::string::npos)
    return {};
  const size_t Open = Source.find('{', At);
  const size_t Close = Source.find("\n}", Open);
  if (Open == std::string::npos || Close == std::string::npos)
    return {};
  const size_t Begin = Source.rfind('\n', At) + 1;
  return Source.substr(Begin, Close + 2 - Begin);
}

TEST(AAPCS64CallContract, FloatingCallResultsSurviveViewsAndSharedReturns) {
  for (bool Double : {false, true})
    for (bool IntegerResult : {false, true}) {
      SCOPED_TRACE(Double);
      SCOPED_TRACE(IntegerResult);
      const uint32_t MoveToOne = Double ? 0x1E604001 : 0x1E204001;
      const uint32_t MoveToZero = Double ? 0x1E604020 : 0x1E204020;
      std::vector<uint32_t> Code{
          0xA9BE7BFD, // stp x29,x30,[sp,-32]!
          0xF9000BF3, // str x19,[sp,16]
          SetFrame,
          0xAA0103F3, // mov x19,x1
          0xB4000140, // cbz x0,zero
          0xAA1F03E1, // mov x1,xzr
          0,          // bl strtof/strtod
          0xB4000113, // cbz x19,done
          0x2F00E401, // movi d1,0
          0xF1000A7F, // cmp x19,2
          0x54000041, // b.ne selected
          MoveToOne,  // fmov s1,s0 / d1,d0
          MoveToZero, // selected: fmov s0,s1 / d0,d1
          0x14000002, // b done
          0x2F00E400, // zero: movi d0,0
      };
      Code[6] = 0x94000000 | ((Plt - (Text + 6 * 4)) / 4);
      if (IntegerResult)
        Code.push_back(0xD28000E0); // done: mov x0,7
      Code.insert(Code.end(), {0xF9400BF3, 0xA8C27BFD, Ret});
      const auto Image = makeImage({{Text, Code}, {Plt, pltEntry(Plt, Got)}},
                                   {{Text, "conditional_parse"}},
                                   {Double ? "strtod" : "strtof"});
      for (bool LLVM : {false, true}) {
        llvm::LLVMContext Context;
        auto Result = run(Image, Context, {Text}, LLVM);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Output(Source);
        CEmitterOptions Options;
        Options.TheArch = Image.Arch;
        Options.Format = Image.Format;
        Options.Image = &Image;
        ASSERT_TRUE(
            LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Output, Options,
                                       nullptr, &Image)
                 : HighCEmitter().emit(Result.HighFuncs, Output, Options));
        EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
        EXPECT_NE(Source.find(IntegerResult ? "int64_t conditional_parse"
                              : Double      ? "double conditional_parse"
                                            : "float conditional_parse"),
                  std::string::npos)
            << Source;
        const std::string Oracle = IntegerResult ? R"oracle(
int main(void) {
  return conditional_parse((uintptr_t)"-3.25", 0) != 7 ||
         conditional_parse((uintptr_t)"-3.25", 1) != 7 ||
         conditional_parse((uintptr_t)"-3.25", 2) != 7 ||
         conditional_parse(0, 0) != 7;
}
)oracle"
                                                 : R"oracle(
int main(void) {
  return conditional_parse((uintptr_t)"-3.25", 0) != -3.25 ||
         conditional_parse((uintptr_t)"-3.25", 1) != 0 ||
         conditional_parse((uintptr_t)"-3.25", 2) != -3.25 ||
         conditional_parse(0, 0) != 0;
}
)oracle";
        source_call_execution_test::compileAndRun(Source + Oracle);
      }
    }
}

TEST(AAPCS64CallContract, SelectedCallsKeepBothProvenReturnWords) {
  constexpr va_t Callee = Text + 0x40;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 8)) / 4);
  // Return {x + 7, x * 8}. The caller consumes both words before any write.
  BinaryImage Image = makeImage(
      {{Text, {PushFrame, SetFrame, Call, 0xCA010000u, PopFrame, Ret}},
       {Callee, {0xD37DF001u, 0x91001C00u, Ret}}},
      {{Text, "pair_caller"}, {Callee, "omitted_pair"}}, {});
  for (BinaryFormat Format :
       {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
    Image.Format = Format;
    PipelineAnalysisCache Cache;
    std::string Cold;
    for (const auto &Entries :
         {std::set<va_t>{Text}, std::set<va_t>{Text, Callee},
          std::set<va_t>{Text}}) {
      SCOPED_TRACE(static_cast<int>(Format));
      SCOPED_TRACE(Entries.size());
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = Entries;
      Options.AnalysisCache = &Cache;
      const auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.HighFuncs.size(), Entries.size());
      const auto Caller =
          std::find_if(Result.MedFuncs.begin(), Result.MedFuncs.end(),
                       [](const MedFunc &F) { return F.Entry == Text; });
      ASSERT_NE(Caller, Result.MedFuncs.end());
      bool Pair = false;
      for (const auto &Block : Caller->Blocks)
        for (const auto &Op : Block.Ops)
          if (Op.Opcode == NdOp::CALL && Op.NumInputs &&
              Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == Callee)
            Pair = Op.Output.Kind == MedVar::Temp && Op.Output.Size == 16;
      EXPECT_TRUE(Pair);
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, COptions));
      EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
      EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
      if (Entries.size() == 1) {
        if (Cold.empty())
          Cold = Source;
        else
          EXPECT_EQ(Source, Cold);
        // Compile the selected source against the independently specified
        // callee too: a scalar extern declaration would truncate x1 again.
        Source += R"C(
unsigned __int128 omitted_pair(uint64_t x) {
  return (unsigned __int128)(x << 3) << 64 | (uint64_t)(x + 7);
}

)C";
      }
      source_call_execution_test::compileAndRun(Source + R"C(
int main(void) {
  uint64_t values[] = {0, 1, 7, 0xFFFFFFFF, 0x123456789ABCDEF0,
                       0xFFFFFFFFFFFFFFFF};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    if ((uint64_t)pair_caller(values[i]) !=
        ((values[i] + 7) ^ (values[i] << 3)))
      return 1;
  return 0;
}
)C");
    }
  }
}

TEST(AAPCS64CallContract, ReadingOnlyTheSecondReturnWordStillRequiresTheBody) {
  constexpr va_t Callee = Text + 0x40;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 8)) / 4);
  for (const auto [WritesSecond, Split] :
       {std::pair{false, false}, {true, false}, {false, true}, {true, true}}) {
    const BinaryImage Image = makeImage(
        {{Text,
          Split ? std::vector<uint32_t>{PushFrame, SetFrame, Call, 0x14000002u,
                                        0xD503201Fu, 0xAA0103E0u, PopFrame, Ret}
                : std::vector<uint32_t>{PushFrame, SetFrame, Call, 0xAA0103E0u,
                                        PopFrame, Ret}},
         {Callee, WritesSecond
                      ? std::vector<uint32_t>{0xD37DF001u, 0x91001C00u, Ret}
                      : std::vector<uint32_t>{PushFrame, SetFrame,
                                              0xD63F0040u, // blr x2
                                              0x91001C00u, PopFrame, Ret}}},
        {{Text, "second_word"}, {Callee, "omitted_pair"}}, {});
    for (const auto &Entries :
         {std::set<va_t>{Text}, std::set<va_t>{Text, Callee}}) {
      const std::string Source = highC(Image, Entries);
      if (!WritesSecond) {
        EXPECT_EQ(Source.find("__int128"), std::string::npos) << Source;
        continue;
      }
      ASSERT_EQ(Source.find("unknown value"), std::string::npos) << Source;
      const std::string External = Entries.size() == 1 ? R"C(
unsigned __int128 omitted_pair(uint64_t x) {
  return (unsigned __int128)(x << 3) << 64 | (uint64_t)(x + 7);
}
)C"
                                                       : "";
      source_call_execution_test::compileAndRun(Source + External + R"C(
int main(void) {
  uint64_t values[] = {0, 1, 7, 0x123456789ABCDEF0, UINT64_MAX};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    if ((uint64_t)second_word(values[i]) != (values[i] << 3))
      return 1;
  return 0;
}
)C");
    }
  }
}

TEST(AAPCS64CallContract, AConstantFormatSelectsIntegerAndVectorArguments) {
  constexpr va_t Format = 0x21000;
  const uint32_t Call = 0x94000000u | ((Plt - (Text + 20)) / 4);
  BinaryImage Image = makeImage(
      {{Text,
        {PushFrame, SetFrame, 0xAA0203E3u, // mov x3, x2
         adrp(2, Text + 12, Format), addX(2, 2, Format), Call, PopFrame, Ret}},
       {Plt, pltEntry(Plt, Got)}},
      {{Text, "format_values"}}, {"snprintf"});
  Segment Data;
  Data.Name = ".rodata";
  Data.VA = Format;
  const std::string FormatText = "%d %.2f";
  Data.Data.assign(FormatText.begin(), FormatText.end());
  Data.Data.push_back(0);
  Data.Size = Data.FileSz = Data.Data.size();
  Data.Flags = SegmentFlags::Readable;
  Image.Segments.push_back(std::move(Data));
  llvm::LLVMContext Context;
  const auto Result = run(Image, Context, {Text});
  ASSERT_TRUE(Result.Success) << Result.Error;
  const auto Bound = Result.FormattedCalls.find(Text + 20);
  ASSERT_NE(Bound, Result.FormattedCalls.end());
  ASSERT_EQ(Bound->second.Arguments.size(), 5u);
  EXPECT_TRUE(Bound->second.Arguments.back().Floating);
  const std::string Source = highC(Image, {Text});
  ASSERT_EQ(Source.find("unknown value"), std::string::npos) << Source;
  EXPECT_NE(Source.find("\"%d %.2f\""), std::string::npos) << Source;
  source_call_execution_test::compileAndRun(Source + R"C(
#include <string.h>
int main(void) {
  char text[32];
  int (*format)(char *, unsigned long, int, double) =
      (int (*)(char *, unsigned long, int, double))(void *)format_values;
  for (int i = -2; i != 3; ++i) {
    char expected[32];
    int n = snprintf(expected, sizeof expected, "%d %.2f", i, 3.25);
    if (format(text, sizeof text, i, 3.25) != n || strcmp(text, expected))
      return 1;
  }
  return 0;
}
)C");
}

TEST(AAPCS64CallContract, AFormatMustBeImmutableAndSupportedByTheTargetABI) {
  constexpr va_t Format = 0x21000;
  const uint32_t Call = 0x94000000u | ((Plt - (Text + 16)) / 4);
  for (unsigned Variant = 0; Variant != 5; ++Variant) {
    BinaryImage Image =
        makeImage({{Text,
                    {PushFrame, SetFrame, adrp(2, Text + 8, Format),
                     addX(2, 2, Format), Call, PopFrame, Ret}},
                   {Plt, pltEntry(Plt, Got)}},
                  {{Text, "format_values"}}, {"snprintf"});
    Segment Data;
    Data.Name = ".rodata";
    Data.VA = Format;
    const std::string FormatText = Variant == 1 ? "%Lf" : "%d %.2f";
    Data.Data.assign(FormatText.begin(), FormatText.end());
    Data.Data.push_back(0);
    Data.Size = Data.FileSz = Data.Data.size();
    Data.Flags = Variant == 0 ? SegmentFlags::Readable | SegmentFlags::Writable
                              : SegmentFlags::Readable;
    Image.Segments.push_back(std::move(Data));
    if (Variant == 2)
      Image.Format = BinaryFormat::MachO;
    if (Variant == 3)
      Image.Format = BinaryFormat::COFF;
    llvm::LLVMContext Context;
    const auto Result = run(Image, Context, {Text});
    ASSERT_TRUE(Result.Success) << Result.Error;
    EXPECT_EQ(Result.FormattedCalls.count(Text + 16), Variant == 4 ? 1u : 0u)
        << Variant;
  }
}

TEST(AAPCS64CallContract, AnIndirectObjectReturnHasNoFabricatedScalarResult) {
  class Header : public NullDebugContext {
  public:
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      if (Address != Text)
        return std::nullopt;
      FunctionSym Function;
      Function.Name = "copy_result";
      Function.ReturnType = NdType::makeStruct(
          {NdType::makeInt(8), NdType::makeInt(8), NdType::makeInt(8)});
      Function.ReturnType->Passing = NdRecordPassing::ByValue;
      Function.Params = {{"a", NdType::makeInt(8)},
                         {"b", NdType::makeInt(8)},
                         {"c", NdType::makeInt(8)}};
      return Function;
    }
  } Debug;
  const BinaryImage Image =
      makeImage({{Text, {0xF9000100u, 0xF9000501u, 0xF9000902u, Ret}}},
                {{Text, "copy_result"}}, {});
  llvm::LLVMContext Context;
  const auto Result = run(Image, Context, {Text});
  ASSERT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream Output(Source);
  CEmitterOptions Options;
  Options.TheArch = Image.Arch;
  Options.Format = Image.Format;
  Options.Image = &Image;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, Options, &Debug));
  EXPECT_NE(Source.find("void copy_result("), std::string::npos) << Source;
  EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
  source_call_execution_test::compileAndRun(Source + R"C(
int main(void) {
  uint64_t result[3] = {0};
  copy_result(7, 11, 13, (void *)result);
  return result[0] != 7 || result[1] != 11 || result[2] != 13;
}
)C");
}

TEST(AAPCS64CallContract, ReadingW8DoesNotReplaceTheIndirectResultPointer) {
  constexpr va_t Callee = Text + 0x80;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 16)) / 4);
  BinaryImage Image =
      makeImage({{Text,
                  {0xA9BD7BFDu, SetFrame, // 48-byte frame
                   0x910043E8u,           // add x8, sp, #16
                   0xB9002FE8u,           // str w8, [sp, #44] (reads W8)
                   Call, 0xA94107E0u,     // ldp x0, x1, [sp, #16]
                   0xF94013E2u,           // ldr x2, [sp, #32]
                   0x8B010000u, 0x8B020000u, 0xA8C37BFDu, Ret}},
                 {Callee,
                  {0xF9000100u, 0x91000401u, 0xF9000501u, 0x91000802u,
                   0xF9000902u, Ret}}},
                {{Text, "sum_triplet"}, {Callee, "fill_triplet"}}, {});
  const std::string Source = highC(Image, {Text, Callee});
  ASSERT_EQ(Source.find("unknown value"), std::string::npos) << Source;
  source_call_execution_test::compileAndRun(Source + R"C(
int main(void) {
  uint64_t values[] = {0, 1, 17, 0xFFFFFFFF};
  for (unsigned i = 0; i != sizeof(values) / sizeof(values[0]); ++i)
    if ((uint64_t)sum_triplet(values[i]) != 3 * values[i] + 3)
      return 1;
  return 0;
}
)C");
}

TEST(AAPCS64CallContract, CxxCleanupUsesTheFrameAtTheThrowingCall) {
  const auto Call = [](va_t At, va_t Target) {
    return 0x94000000u |
           (static_cast<uint32_t>((Target - At) / 4) & 0x3FFFFFFu);
  };
  BinaryImage Image = makeImage(
      {{Text,
        {0xA9BE7BFDu, SetFrame, 0xF9000BE0u, // frame; local = x0
         0xB4000061u,                        // cbz x1, +12
         Call(Text + 16, Plt), 0x14000002u, Call(Text + 24, Plt), 0xF9400BE0u,
         0xA8C27BFDu, Ret, 0xAA0003F3u,
         0xF9400BA0u, // pad: save exception; x0 = fp[16]
         Call(Text + 48, Plt + 16), 0xAA1303E0u, Call(Text + 56, Plt + 32)}},
       {Plt, pltEntry(Plt, Got)},
       {Plt + 16, pltEntry(Plt + 16, Got + 8)},
       {Plt + 32, pltEntry(Plt + 32, Got + 16)}},
      {{Text, "protected_value"}}, {"getchar", "putchar", "_Unwind_Resume"});
  ExceptionFunction EH;
  EH.CodeRange = {Text, Text + 60};
  EH.Encoding = ExceptionEncoding::DwarfFDE;
  EH.ParseStatus = ExceptionParseStatus::Complete;
  EH.Personality = ExceptionPersonality::GxxPersonalityV0;
  EH.Itanium.emplace();
  EH.Itanium->IsCallSiteAddressForm = true;
  ItaniumCallSite Site;
  // The machine range includes the epilogue, whose restored FP is different
  // from the FP at either call. The C++ clause runs only when a call throws.
  Site.GuardedRange = {Text + 12, Text + 40};
  Site.LandingPadVA = Text + 40;
  EH.Itanium->CallSites.push_back(Site);
  Image.ExceptionMetadata.Functions.push_back(std::move(EH));
  const std::string Source = highC(Image, {Text});
  ASSERT_NE(Source.find("catch (...)"), std::string::npos) << Source;
  ASSERT_EQ(Source.find("unknown value"), std::string::npos) << Source;
  source_call_execution_test::compileAndRun(
      Source + R"C(
static bool should_throw;
static int cleaned;
extern "C" int getchar(void) {
  if (should_throw) throw 42;
  return 0;
}
extern "C" int putchar(int value) { cleaned = value; return value; }
int main(void) {
  for (int branch = 0; branch != 2; ++branch) {
    should_throw = false;
    cleaned = 0;
    if (protected_value(7, branch) != 7 || cleaned) return 1;
    should_throw = true;
    try { protected_value(7, branch); return 2; }
    catch (int value) { if (value != 42 || cleaned != 7) return 3; }
  }
  return 0;
}
)C",
      {"--driver-mode=g++", "-x", "c++", "-std=c++20"});
}

TEST(AAPCS64CallContract, CleanupFrameAddressesAlsoSurviveInSavedRegisters) {
  const auto Call = [](va_t At, va_t Target) {
    return 0x94000000u |
           (static_cast<uint32_t>((Target - At) / 4) & 0x3FFFFFFu);
  };
  for (bool FrameAddress : {true, false}) {
    BinaryImage Image = makeImage(
        {{Text,
          {0xA9BD7BFDu, SetFrame, 0xF9000BE0u,       // local = x0
           0xF90013FAu,                              // save x26 at sp+32
           FrameAddress ? 0x910043BAu : 0xAA0203FAu, // x26=fp+16 or x2
           0xB4000061u, Call(Text + 24, Plt), 0x14000002u, Call(Text + 32, Plt),
           0xF9400BE0u, 0xF94013FAu, 0xA8C37BFDu, Ret, 0xAA0003F3u,
           0xF9400340u, // pad: x0 = *x26
           Call(Text + 60, Plt + 16), 0xAA1303E0u, Call(Text + 68, Plt + 32)}},
         {Plt, pltEntry(Plt, Got)},
         {Plt + 16, pltEntry(Plt + 16, Got + 8)},
         {Plt + 32, pltEntry(Plt + 32, Got + 16)}},
        {{Text, "protected_value"}}, {"getchar", "putchar", "_Unwind_Resume"});
    ExceptionFunction EH;
    EH.CodeRange = {Text, Text + 72};
    EH.Encoding = ExceptionEncoding::DwarfFDE;
    EH.ParseStatus = ExceptionParseStatus::Complete;
    EH.Personality = ExceptionPersonality::GxxPersonalityV0;
    EH.Itanium.emplace();
    EH.Itanium->IsCallSiteAddressForm = true;
    ItaniumCallSite Site;
    Site.GuardedRange = {Text + 20, Text + 52};
    Site.LandingPadVA = Text + 52;
    EH.Itanium->CallSites.push_back(Site);
    Image.ExceptionMetadata.Functions.push_back(std::move(EH));
    const std::string Source = highC(Image, {Text});
    ASSERT_NE(Source.find("catch (...)"), std::string::npos) << Source;
    if (!FrameAddress) {
      // An arbitrary saved pointer is not proved to be the frame, even if
      // the very same register carried a frame address in the previous run.
      EXPECT_NE(Source.find("unknown register"), std::string::npos) << Source;
      continue;
    }
    ASSERT_EQ(Source.find("unknown value"), std::string::npos) << Source;
    source_call_execution_test::compileAndRun(
        Source + R"C(
static bool should_throw;
static int cleaned;
extern "C" int getchar(void) {
  if (should_throw) throw 42;
  return 0;
}
extern "C" int putchar(int value) { cleaned = value; return value; }
int main(void) {
  for (int branch = 0; branch != 2; ++branch) {
    should_throw = false;
    cleaned = 0;
    if (protected_value(7, branch) != 7 || cleaned) return 1;
    should_throw = true;
    try { protected_value(7, branch); return 2; }
    catch (int value) { if (value != 42 || cleaned != 7) return 3; }
  }
  return 0;
}
)C",
        {"--driver-mode=g++", "-x", "c++", "-std=c++20"});
  }
}

TEST(AAPCS64CallContract, RegisterArgumentsSurviveAddressStores) {
  constexpr va_t Callee = Text + 0x80;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 28)) / 4);
  const BinaryImage Image =
      makeImage({{Text,
                  {PushFrame, SetFrame, 0xAA0103E3u, // mov x3, x1
                   0xD28002E1u, 0xD2800002u,         // x1 = 23; x2 = 0
                   0xF8226803u,                      // str x3, [x0, x2]
                   0xD28000E0u, Call, PopFrame, Ret}},
                 {Callee, {0x8B010000u, Ret}}},
                {{Text, "store_then_call"}, {Callee, "sum_words"}}, {});
  const std::string Source = highC(Image, {Text, Callee});
  ASSERT_EQ(Source.find("unknown value"), std::string::npos) << Source;
#if defined(__aarch64__)
  source_call_execution_test::compileAndRun(Source + R"C(
int main(void) {
  uint64_t stored = 0;
  if ((uint64_t)store_then_call(&stored, 123) != 30 || stored != 123)
    return 1;
  return 0;
}
)C");
#endif
}

TEST(AAPCS64CallContract, ArgumentSetupSurvivesMoreThanOnePredecessor) {
  constexpr va_t Callee = Text + 0x80;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 40)) / 4);
  const BinaryImage Image =
      makeImage({{Text,
                  {PushFrame, SetFrame, 0xAA0203E0u, // mov x0, x2
                   0xB4000063u,                      // cbz x3, +12
                   0xD28000E1u, 0x14000002u,         // x1 = 7; b +8
                   0xD2800161u,                      // x1 = 11
                   0xB4000044u,                      // cbz x4, +8
                   0xD28001A1u,                      // x1 = 13
                   0x14000001u, Call, PopFrame, Ret}},
                 {Callee, {0x8B010000u, Ret}}},
                {{Text, "split_setup"}, {Callee, "sum_words"}}, {});
  const std::string Source = highC(Image, {Text, Callee});
  ASSERT_EQ(Source.find("unknown value"), std::string::npos) << Source;
  source_call_execution_test::compileAndRun(Source + R"C(
int main(void) {
  for (unsigned a = 0; a != 2; ++a)
    for (unsigned b = 0; b != 2; ++b)
      if ((uint64_t)split_setup(2, 3, 101, a, b) !=
          101 + (b ? 13 : a ? 7 : 11))
        return 1;
  return 0;
}
)C");
}

TEST(AAPCS64CallContract, SelectedLLVMCallsKeepBothProvenReturnWords) {
  constexpr va_t Callee = Text + 0x40;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 8)) / 4);
  const BinaryImage Image = makeImage(
      {{Text, {PushFrame, SetFrame, Call, 0xCA010000u, PopFrame, Ret}},
       {Callee, {0xD37DF001u, 0x91001C00u, Ret}}},
      {{Text, "pair_caller"}, {Callee, "omitted_pair"}}, {});
  PipelineAnalysisCache Cache;
  for (const auto &Entries :
       {std::set<va_t>{Text}, std::set<va_t>{Text, Callee},
        std::set<va_t>{Text}}) {
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = Entries;
    Options.AnalysisCache = &Cache;
    Options.LiftMode = Options.SourceProjection = true;
    Options.NoOpt = true;
    const auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_TRUE(Result.LlvmModule);
    std::string IR;
    llvm::raw_string_ostream Out(IR);
    Result.LlvmModule->print(Out, nullptr);
    bool SawPair = false;
    for (const auto &Function : *Result.LlvmModule)
      for (const auto &Block : Function)
        for (const auto &Instruction : Block)
          if (const auto *Call = llvm::dyn_cast<llvm::CallBase>(&Instruction);
              Call && Call->getCalledFunction()) {
            auto Address =
                rewrite_source::getOriginalVA(*Call->getCalledFunction());
            ASSERT_TRUE(static_cast<bool>(Address));
            if (*Address && **Address == Callee) {
              EXPECT_EQ(Result.LlvmModule->getDataLayout().getTypeAllocSize(
                            Call->getType()),
                        16u)
                  << IR;
              SawPair = true;
            }
          }
    EXPECT_TRUE(SawPair) << IR;
  }
}

TEST(AAPCS64CallContract, OmittedReturnWordsRequireEveryReturnToDefineThem) {
  constexpr va_t Callee = Text + 0x40;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 8)) / 4);
  // x1 is defined on only one return path. Reading it at the caller cannot
  // make the missing value into a second field, cold or after cache warmup.
  BinaryImage Image = makeImage(
      {{Text, {PushFrame, SetFrame, Call, 0xCA010000u, PopFrame, Ret}},
       {Callee, {0xB4000060u, 0xD28000E1u, Ret, Ret}}},
      {{Text, "partial_pair_caller"}, {Callee, "partial_pair"}}, {});
  PipelineAnalysisCache Cache;
  for (const auto &Entries :
       {std::set<va_t>{Text}, std::set<va_t>{Text, Callee},
        std::set<va_t>{Text}}) {
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = Entries;
    Options.AnalysisCache = &Cache;
    const auto Result = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Result.Success) << Result.Error;
    for (const auto &Function : Result.MedFuncs)
      if (Function.Entry == Text)
        for (const auto &Block : Function.Blocks)
          for (const auto &Op : Block.Ops)
            if (Op.Opcode == NdOp::CALL && Op.NumInputs &&
                Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == Callee)
              EXPECT_NE(Op.Output.Size, 16);
  }
}

TEST(AAPCS64CallContract, APLTEntryPassesTheArgumentsOfItsImport) {
  // islower takes one argument, which its PLT entry passes on in x0 without
  // writing it; getuid takes none.
  constexpr va_t Lower = Plt, Uid = Plt + 0x10;
  const BinaryImage Img =
      makeImage({{Lower, pltEntry(Lower, Got)}, {Uid, pltEntry(Uid, Got + 8)}},
                {}, {"islower", "getuid"});
  ASSERT_NE(Img.findImportAt(Lower), nullptr);
  const std::string Source = highC(Img, {Lower, Uid});
  const std::string LowerBody = body(Source, "sub_10100");
  ASSERT_FALSE(LowerBody.empty()) << Source;
  EXPECT_NE(LowerBody.find("sub_10100(int64_t arg0)"), std::string::npos)
      << LowerBody;
  EXPECT_NE(LowerBody.find("islower(arg0)"), std::string::npos) << LowerBody;
  EXPECT_EQ(LowerBody.find("unknown value"), std::string::npos) << LowerBody;
  const std::string UidBody = body(Source, "sub_10110");
  ASSERT_FALSE(UidBody.empty()) << Source;
  EXPECT_NE(UidBody.find("sub_10110(void)"), std::string::npos) << UidBody;
  EXPECT_NE(UidBody.find("getuid()"), std::string::npos) << UidBody;
}

TEST(AAPCS64CallContract, PLTEntriesOfVariadicSystemCallsPassTheirArgsOn) {
  // syscall reads at most six arguments after the number, ptrace three after
  // the request and prctl four after the option.  Each entry passes every
  // argument on, which C does one by one.
  constexpr va_t Sys = Plt, Trace = Plt + 0x10, Control = Plt + 0x20;
  const BinaryImage Img = makeImage({{Sys, pltEntry(Sys, Got)},
                                     {Trace, pltEntry(Trace, Got + 8)},
                                     {Control, pltEntry(Control, Got + 16)}},
                                    {}, {"syscall", "ptrace", "prctl"});
  const std::string Source = highC(Img, {Sys, Trace, Control});
  EXPECT_NE(Source.find("long sub_10100(long arg0, ...) {"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("return syscall(arg0, argument1, argument2, "
                        "argument3, argument4, argument5, argument6);"),
            std::string::npos)
      << Source;
  // glibc's request is an enumeration, which <sys/ptrace.h> declares.
  EXPECT_NE(Source.find("#include <sys/ptrace.h>"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("long sub_10110(enum __ptrace_request arg0, ...) {"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("return ptrace(arg0, argument1, argument2, "
                        "argument3);"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("int sub_10120(int arg0, ...) {"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("return prctl(arg0, argument1, argument2, argument3, "
                        "argument4);"),
            std::string::npos)
      << Source;
}

TEST(AAPCS64CallContract, PLTEntriesPassArgumentsOnAsTheirCLibraryDeclares) {
  // Bionic's ptrace and ioctl take their requests as ints, which glibc's
  // headers declare an enumeration and an unsigned long: the entries declare
  // them as <sys/ptrace.h> and <sys/ioctl.h> do.
  constexpr va_t Trace = Plt, Control = Plt + 0x10;
  BinaryImage Img = makeImage(
      {{Trace, pltEntry(Trace, Got)}, {Control, pltEntry(Control, Got + 8)}},
      {}, {"ptrace", "ioctl"});
  Img.ELFMetadata.emplace();
  Img.ELFMetadata->Interpreter = "/system/bin/linker64";
  const std::string Source = highC(Img, {Trace, Control});
  EXPECT_NE(Source.find("extern long ptrace(int, ...);"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("long sub_10100(int arg0, ...) {"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("__ptrace_request"), std::string::npos) << Source;
  EXPECT_NE(Source.find("extern int ioctl(int, int, ...);"), std::string::npos)
      << Source;
  EXPECT_NE(Source.find("int sub_10110(int arg0, int arg1, ...) {"),
            std::string::npos)
      << Source;
}

TEST(AAPCS64CallContract, APLTEntryInLLVMCCallsItsImportByName) {
  // The entry calls the word it loads from islower's slot.  Its function is
  // not islower, which it calls by that name as <ctype.h> declares it; the
  // word is no value of the C.
  const BinaryImage Img =
      makeImage({{Plt, pltEntry(Plt, Got)}}, {}, {"islower"});
  llvm::LLVMContext Ctx;
  llvm::SMDiagnostic Error;
  const auto Module = llvm::parseAssemblyString(R"(
target triple = "aarch64-unknown-linux-gnu"
@__nd_codeptr_20000 = external global i64
define i64 @sub_10100(i64 %arg0) {
  %ld = load volatile i64, ptr @__nd_codeptr_20000, align 8
  %fn = inttoptr i64 %ld to ptr
  %call = tail call i64 %fn(i64 %arg0)
  ret i64 %call
}
)",
                                                Error, Ctx);
  ASSERT_TRUE(Module) << Error.getMessage().str();
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Options, nullptr, &Img,
                                  Module->getFunction("sub_10100")));
  EXPECT_NE(Source.find("#include <ctype.h>"), std::string::npos) << Source;
  EXPECT_NE(Source.find("uint64_t sub_10100(uint64_t arg0) {"),
            std::string::npos)
      << Source;
  EXPECT_NE(Source.find("return islower(arg0);"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("volatile"), std::string::npos) << Source;
}

TEST(AAPCS64CallContract, OnlyReachingEntryRegistersBecomeForwardedParameters) {
  for (unsigned Mode : {0, 1, 2, 3}) {
    SCOPED_TRACE(Mode);
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Format = BinaryFormat::ELF;
    MedFunc F;
    F.Entry = Text;
    F.CC = CallingConv::ARM_AAPCS;
    F.Blocks.emplace_back();
    F.Blocks.front().Id = 0;
    F.Blocks.front().StartAddr = Text;
    F.Blocks.front().EndAddr = Text + 16;
    auto Call = [](va_t Target) {
      MedOp Op;
      Op.Opcode = NdOp::CALL;
      Op.addInput(MedVar::makeConst(Target, 8));
      return Op;
    };
    if (Mode == 1)
      F.Blocks.front().Ops.push_back(Call(Text + 0x80));
    if (Mode >= 2) {
      F.Blocks.emplace_back();
      F.Blocks.back().Id = 1;
      F.Blocks.back().StartAddr = Text + 32;
      F.Blocks.back().EndAddr = Text + 48;
      if (Mode == 2)
        F.ModuleAnalysisRoots.insert(Text + 32);
      else
        F.Blocks.back().ExceptionalPreds.emplace_back();
    }
    F.Blocks.back().Ops.push_back(Call(Text + 0x40));
    std::map<va_t, int> Regs{{Text + 0x40, 3}}, Total = Regs;
    recoverCallAbi(F, Image.Arch, {}, &Image, &Regs, &Total);
    ASSERT_FALSE(F.CallInfos.empty());
    const auto &C = F.CallInfos.back();
    ASSERT_EQ(C.Args.size(), 3u);
    if (Mode == 0) {
      EXPECT_EQ(F.Params.size(), 3u);
      for (const auto &V : C.Args)
        EXPECT_EQ(V.Kind, MedVar::Param);
    } else {
      EXPECT_TRUE(F.Params.empty());
      for (const auto &V : C.Args)
        EXPECT_EQ(V.Kind, MedVar::Unspecified);
    }
  }
}

TEST(AAPCS64CallContract,
     DirectCalleesPrecedeDeepLowerAddressChainsWithinBudget) {
  constexpr va_t First = 0x10000, Zero = 0x20000, One = 0x20040,
                 Caller = 0x30000;
  auto BL = [](va_t At, va_t Target) {
    return 0x94000000u | (uint32_t((Target - At) / 4) & 0x03ffffffu);
  };
  BinaryImage Image;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::ELF;
  auto Text = codeSegment(".text", First, Caller - First + 0x100);
  auto Add = [&](va_t At, const char *Name, std::vector<uint32_t> Words) {
    place(Text, At, Words);
    auto S = Symbol::makeFunc(At);
    S.Name = Name;
    S.Size = Words.size() * 4;
    Image.Symbols.push_back(std::move(S));
  };
  for (size_t I = 0; I <= limits::kMaxCallEffectExtraLifts; ++I) {
    const auto At = First + I * 32;
    if (I != limits::kMaxCallEffectExtraLifts)
      Add(At, "", {PushFrame, SetFrame, BL(At + 8, At + 32), PopFrame, Ret});
    else
      Add(At, "", {0xD2800120u, Ret}); // mov x0, 9
  }
  Add(Zero, "takes_none", {0xD28000e0u, Ret}); // mov x0, 7
  Add(One, "takes_one", {0xF900001fu, Ret});   // str xzr, [x0]
  Add(Caller, "selected_caller",
      {PushFrame, SetFrame, BL(Caller + 8, First), BL(Caller + 12, Zero),
       BL(Caller + 16, One), PopFrame, Ret});
  Image.Segments.push_back(std::move(Text));
  llvm::LLVMContext Context;
  const auto Result = run(Image, Context, {Caller});
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_TRUE(Result.OmittedCallees);
  std::set<va_t> Summarized;
  for (const auto &F : Result.OmittedCallees->Bodies)
    Summarized.insert(F->Entry);
  EXPECT_TRUE(Summarized.contains(Zero));
  EXPECT_TRUE(Summarized.contains(One));
  EXPECT_LE(Summarized.size(), limits::kMaxCallEffectExtraLifts);
  const auto &Function = Result.MedFuncs.front();
  unsigned Checked = 0;
  for (const auto &Call : Function.CallInfos)
    if (Call.TargetAddr == Zero || Call.TargetAddr == One) {
      EXPECT_EQ(Call.Args.size(), Call.TargetAddr == Zero ? 0u : 1u);
      ++Checked;
    }
  EXPECT_EQ(Checked, 2u);
}

/// lower(c) tail-calls islower and bump(c) returns toupper(c) + 1, each
/// through the GOT slot the call loads, as -fno-plt builds them.
BinaryImage makeGOTCalls() {
  constexpr va_t Lower = Text, Bump = Text + 0x10;
  constexpr va_t LowerSlot = Got, BumpSlot = Got + 8;
  return makeImage(
      {{Lower, {adrp(1, Lower, LowerSlot), ldrX(1, 1, LowerSlot), br(1)}},
       {Bump,
        {PushFrame, SetFrame, adrp(8, Bump + 8, BumpSlot), ldrX(8, 8, BumpSlot),
         blr(8), AddW0One, PopFrame, Ret}}},
      {{Lower, "lower"}, {Bump, "bump"}}, {"islower", "toupper"});
}

TEST(AAPCS64CallContract, ACallThroughAGOTSlotPassesTheIncomingArgument) {
  const BinaryImage Img = makeGOTCalls();
  const std::string Source = highC(Img, {Text, Text + 0x10});
  const std::string Lower = body(Source, "lower");
  ASSERT_FALSE(Lower.empty()) << Source;
  EXPECT_NE(Lower.find("lower(int64_t arg0)"), std::string::npos) << Lower;
  EXPECT_NE(Lower.find("islower(arg0)"), std::string::npos) << Lower;
  const std::string Bump = body(Source, "bump");
  ASSERT_FALSE(Bump.empty()) << Source;
  EXPECT_NE(Bump.find("toupper(arg0)"), std::string::npos) << Bump;
  EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
}

TEST(AAPCS64CallContract, TheLLVMRoutePassesTheArgumentThroughAGOTSlot) {
  // The call through the slot passes the function's own argument, whether
  // the emitter calls the import or the word it loads.
  const BinaryImage Img = makeGOTCalls();
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, {Text, Text + 0x10}, true);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_TRUE(Result.LlvmModule);
  for (const char *Name : {"lower", "bump"}) {
    SCOPED_TRACE(Name);
    const llvm::Function *Fn = Result.LlvmModule->getFunction(Name);
    ASSERT_NE(Fn, nullptr);
    std::string IR;
    llvm::raw_string_ostream OS(IR);
    Fn->print(OS);
    ASSERT_EQ(Fn->arg_size(), 1u) << IR;
    const llvm::CallInst *Call = nullptr;
    for (const llvm::BasicBlock &Block : *Fn)
      for (const llvm::Instruction &I : Block)
        if (const auto *C = llvm::dyn_cast<llvm::CallInst>(&I);
            C && !llvm::isa<llvm::IntrinsicInst>(C))
          Call = C;
    ASSERT_NE(Call, nullptr) << IR;
    ASSERT_EQ(Call->arg_size(), 1u) << IR;
    EXPECT_EQ(Call->getArgOperand(0)->stripPointerCasts(), Fn->getArg(0)) << IR;
  }
}

/// A shared object that defines g(x) { return x + x; } and calls it through
/// its PLT entry, as `f: b g@plt` does for a preemptible g.  The loader
/// relocates g's slot to g itself.
constexpr va_t OwnG = Text, OwnF = Text + 0x10, OwnStub = Plt;
BinaryImage makeOwnRoutinePLT(const char *Import = "g",
                              const char *Alias = nullptr) {
  constexpr uint32_t AddW0W0 = 0x0B000000u; // add w0, w0, w0
  const uint32_t BranchToStub =
      0x14000000u | (static_cast<uint32_t>(OwnStub - OwnF) >> 2 & 0x3FFFFFF);
  std::vector<std::pair<va_t, const char *>> Functions = {{OwnG, "g"},
                                                          {OwnF, "f"}};
  if (Alias)
    Functions.push_back({OwnG, Alias});
  BinaryImage Img = makeImage({{OwnG, {AddW0W0, Ret}},
                               {OwnF, {BranchToStub}},
                               {OwnStub, pltEntry(OwnStub, Got)}},
                              Functions, {Import},
                              /*Defined=*/true);
  for (unsigned B = 0; B < 8; ++B)
    Img.Segments.back().Data[B] = static_cast<uint8_t>(OwnG >> (8 * B));
  Img.CodePtrRelocSlots.insert(Got);
  return Img;
}

TEST(AAPCS64CallContract,
     ASlotTheLoaderStartsAtThisImagesRoutinePassesItsArgs) {
  // The PLT entry passes g's one argument on, to g by its name.
  const BinaryImage Img = makeOwnRoutinePLT();
  const std::string Source = highC(Img, {OwnG, OwnStub});
  const std::string Entry = body(Source, "sub_10100");
  ASSERT_FALSE(Entry.empty()) << Source;
  EXPECT_NE(Entry.find("sub_10100(int64_t arg0)"), std::string::npos) << Entry;
  EXPECT_TRUE(llvm::Regex("[^[:alnum:]_]g\\(arg0\\)").match(Entry)) << Entry;
  EXPECT_EQ(Entry.find("unknown value"), std::string::npos) << Entry;
}

TEST(AAPCS64CallContract, ACallThroughAnAliasesSlotKeepsTheAliasesName) {
  // libgcc's __lttf2 and __letf2 name one routine: a call through the slot
  // the loader binds to one of them names the routine by that name.
  const BinaryImage Img = makeOwnRoutinePLT("galias", "galias");
  const std::string Source = highC(Img, {OwnG, OwnF, OwnStub});
  for (const char *Name : {"f", "sub_10100"}) {
    SCOPED_TRACE(Name);
    const std::string Body = body(Source, Name);
    ASSERT_FALSE(Body.empty()) << Source;
    EXPECT_TRUE(llvm::Regex("[^[:alnum:]_]galias\\(arg0\\)").match(Body))
        << Body;
  }
}

TEST(AAPCS64CallContract, ACallToThePLTEntryOfItsOwnRoutinePassesItsArgs) {
  // f reaches g through g's PLT entry and passes its argument on, whether
  // or not the entry and g are decompiled with it.
  const BinaryImage Img = makeOwnRoutinePLT();
  for (const std::set<va_t> &Entries :
       {std::set<va_t>{OwnG, OwnF, OwnStub}, std::set<va_t>{OwnF}}) {
    SCOPED_TRACE(Entries.size());
    const std::string Source = highC(Img, Entries);
    const std::string F = body(Source, "f");
    ASSERT_FALSE(F.empty()) << Source;
    EXPECT_NE(F.find("f(int64_t arg0)"), std::string::npos) << F;
    EXPECT_TRUE(llvm::Regex("[^[:alnum:]_]g\\(arg0\\)").match(F)) << F;
    EXPECT_FALSE(llvm::Regex("[^[:alnum:]_]g_[[:xdigit:]]+\\(").match(Source))
        << Source;
  }
  // The LLVM route passes it too, rather than a value of its own: inlined
  // or not, what f returns depends on its argument.
  llvm::LLVMContext Ctx;
  const PipelineResult Result = run(Img, Ctx, {OwnG, OwnF}, true);
  ASSERT_TRUE(Result.Success) << Result.Error;
  ASSERT_TRUE(Result.LlvmModule);
  const llvm::Function *Fn = Result.LlvmModule->getFunction("f");
  ASSERT_NE(Fn, nullptr);
  std::string IR;
  llvm::raw_string_ostream OS(IR);
  Fn->print(OS);
  ASSERT_EQ(Fn->arg_size(), 1u) << IR;
  EXPECT_FALSE(Fn->getArg(0)->use_empty()) << IR;
  for (const llvm::BasicBlock &Block : *Fn)
    for (const llvm::Instruction &I : Block)
      if (const auto *Call = llvm::dyn_cast<llvm::CallInst>(&I);
          Call && !llvm::isa<llvm::IntrinsicInst>(Call))
        for (const llvm::Value *Argument : Call->args())
          EXPECT_FALSE(llvm::isa<llvm::Constant>(Argument)) << IR;
}

TEST(AAPCS64CallContract, WindowsVariadicPrefixSurvivesOmittedCalleeCache) {
  windows_variadic_test::Header Debug;
  Debug.Slot = Got;
  constexpr va_t Caller = Text + 0x80;
  for (unsigned Tail : {0u, 1u, 7u, 8u}) {
    std::vector<uint32_t> Code = {PushFrame, SetFrame,
                                  0xD10043FFu,                // sub sp, sp, #16
                                  0x52800001u | (Tail << 5)}; // mov w1, #Tail
    for (unsigned I = 0; I < Tail; ++I) {
      const unsigned Register = I < 6 ? I + 2 : 8;
      Code.push_back(0x52800000u | (11 * (I + 1) << 5) | Register);
      if (I >= 6)
        Code.push_back(0xF90003E8u | ((I - 6) << 10)); // str x8, [sp, #slot]
    }
    Code.push_back(adrp(16, Text + Code.size() * 4, Got));
    Code.insert(Code.end(), {ldrX(17, 16, Got), blr(17),
                             0x910043FFu, // add sp, sp, #16
                             PopFrame, Ret});
    BinaryImage Image = makeImage(
        {{Text, Code},
         {Caller,
          {PushFrame, SetFrame,
           0x94000000u |
               (static_cast<uint32_t>((Text - (Caller + 8)) / 4) & 0x3FFFFFF),
           PopFrame, Ret}}},
        {{Text, "forward_variadic"}, {Caller, "middle_variadic"}},
        {"fixture_variadic"});
    Image.Format = BinaryFormat::COFF;
    Image.Imports.front().Module = "fixture.dll";
    PipelineAnalysisCache Cache;
    for (const auto &Entries :
         {std::set<va_t>{Text}, std::set<va_t>{Caller},
          std::set<va_t>{Text, Caller}, std::set<va_t>{Caller}})
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(Tail);
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(Entries.size());
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.LiftMode = LLVM;
        Options.SourceProjection = LLVM;
        Options.OnlyFunctionEntries = Entries;
        Options.AnalysisCache = &Cache;
        auto Result = Pipeline().run(Image, Context, Options, &Debug);
        ASSERT_TRUE(Result.Success) << Result.Error;
        for (const auto &Function : Result.MedFuncs)
          EXPECT_EQ(Function.Params.size(), 1u) << Function.Name;
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                          &Debug, &Image));
        } else {
          ASSERT_TRUE(
              HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
        }
        if (Entries.count(Text))
          source_call_execution_test::compileAndRun(
              Source + windows_variadic_test::oracle(Tail, "forward_variadic"));
        else
          source_call_execution_test::checkSyntax(Source);
      }
  }
}

TEST(AAPCS64CallContract, MixedHeaderScalarsKeepIndependentRegisterBanks) {
  windows_mixed_scalar_test::Header Debug(Got);
  // Only the integer setup changes: d0 and d1 are untouched incoming values.
  // x0/x1/x2/x3 are four source arguments, with the two FP arguments between.
  auto Image =
      makeImage({{Text,
                  {0x528000E0u, 0x52800121u, 0x52800162u, 0xD2824683u,
                   adrp(16, Text + 16, Got), ldrX(17, 16, Got), br(17)}}},
                {{Text, kMixedWrapperName}}, {"fixture_mixed"});
  Image.Format = BinaryFormat::COFF;
  Image.Imports.front().Module = "fixture.dll";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Text};
    Options.LiftMode = LLVM;
    Options.SourceProjection = LLVM;
    auto Result = Pipeline().run(Image, Context, Options, &Debug);
    ASSERT_TRUE(Result.Success) << Result.Error;
    if (LLVM) {
      ASSERT_TRUE(Result.LlvmModule);
      const auto *Wrapper = Result.LlvmModule->getFunction(kMixedWrapperName);
      ASSERT_NE(Wrapper, nullptr);
      EXPECT_TRUE(Wrapper->getReturnType()->isDoubleTy());
      ASSERT_EQ(Wrapper->arg_size(), kMixedHeaderFPParameters);
      for (const auto &Parameter : Wrapper->args())
        EXPECT_TRUE(Parameter.getType()->isDoubleTy());
    }
    CEmitterOptions COptions;
    COptions.TheArch = Image.Arch;
    COptions.Format = Image.Format;
    COptions.Image = &Image;
    std::string Source;
    llvm::raw_string_ostream Out(Source);
    ASSERT_TRUE(
        LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions, &Debug,
                                   &Image)
             : HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
    source_call_execution_test::compileAndRun(
        Source + windows_mixed_scalar_test::Provider + kMixedHeaderOracle);
  }
}

TEST(AAPCS64CallContract, WindowsHeaderFloatsSurviveSelectionAndBothEmitters) {
  class Header : public NullDebugContext {
  public:
    uint16_t ReturnBytes = 8, FirstBytes = 8, SecondBytes = 8;
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name != "fixture_fp")
        return std::nullopt;
      FunctionSym Function;
      Function.Name = Name.str();
      Function.ReturnType = NdType::makeFloat(ReturnBytes);
      Function.Params = {{"x", NdType::makeFloat(FirstBytes)},
                         {"y", NdType::makeFloat(SecondBytes)}};
      Function.PrototypeFromHeader = true;
      return Function;
    }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      auto Function =
          Address == Got ? resolvePrototype("fixture_fp") : std::nullopt;
      if (Function)
        Function->Addr = Address;
      return Function;
    }
  } Debug;
  constexpr va_t Caller = Text + 0x40;
  constexpr va_t Outer = Text + 0x80;
  auto Forward = [](va_t At, va_t Target) {
    const uint32_t Call =
        0x94000000u |
        (static_cast<uint32_t>((Target - (At + 8)) / 4) & 0x3FFFFFF);
    return std::vector<uint32_t>{PushFrame, SetFrame, Call, PopFrame, Ret};
  };
  BinaryImage Image = makeImage(
      {{Text,
        {PushFrame, SetFrame, adrp(16, Text + 8, Got), ldrX(17, 16, Got),
         blr(17), PopFrame, Ret}},
       {Caller, Forward(Caller, Text)},
       {Outer, Forward(Outer, Caller)}},
      {{Text, "forward_fp"}, {Caller, "middle_fp"}, {Outer, "outer_fp"}},
      {"fixture_fp"});
  Image.Format = BinaryFormat::COFF;
  Image.Imports.front().Module = "fixture.dll";
  for (const auto &Widths :
       {std::vector<uint16_t>{4, 4, 4}, std::vector<uint16_t>{8, 8, 8},
        std::vector<uint16_t>{4, 8, 4}, std::vector<uint16_t>{8, 4, 8}}) {
    const uint16_t Bytes = Widths[0];
    Debug.ReturnBytes = Bytes;
    Debug.FirstBytes = Widths[1];
    Debug.SecondBytes = Widths[2];
    PipelineAnalysisCache Cache;
    for (const auto &Entries :
         {std::set<va_t>{Text}, std::set<va_t>{Caller}, std::set<va_t>{Outer},
          std::set<va_t>{Text, Caller}, std::set<va_t>{Text, Caller, Outer},
          std::set<va_t>{Caller, Outer}, std::set<va_t>{Outer}}) {
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << Bytes << ':' << Widths[1] << ':' << Widths[2]);
        SCOPED_TRACE(Entries.size());
        SCOPED_TRACE(LLVM);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.LiftMode = LLVM;
        Options.SourceProjection = LLVM;
        Options.OnlyFunctionEntries = Entries;
        Options.AnalysisCache = &Cache;
        auto Result = Pipeline().run(Image, Context, Options, &Debug);
        ASSERT_TRUE(Result.Success) << Result.Error;
        for (const auto &Function : Result.MedFuncs) {
          EXPECT_EQ(Function.Params.size(), 2u) << Function.Name;
          ASSERT_TRUE(Function.ReturnType);
          EXPECT_EQ(Function.ReturnType->Kind, NdTypeKind::Float)
              << Function.Name;
          EXPECT_EQ(Function.ReturnType->Size, Bytes) << Function.Name;
          ASSERT_EQ(Function.TypedParams.size(), 2u);
          for (size_t I = 0; I < Function.TypedParams.size(); ++I) {
            const auto &Parameter = Function.TypedParams[I];
            ASSERT_TRUE(Parameter.Type);
            EXPECT_EQ(Parameter.Type->Kind, NdTypeKind::Float);
            EXPECT_EQ(Parameter.Type->Size, Widths[I + 1]);
          }
        }
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          {
            const char *External = Entries.count(Text)     ? "fixture_fp"
                                   : Entries.count(Caller) ? "forward_fp"
                                                           : "middle_fp";
            const auto *Callee = Result.LlvmModule->getFunction(External);
            if (!Entries.count(Text)) {
              const va_t Omitted = Entries.count(Caller) ? Text : Caller;
              for (const auto &Declaration : *Result.LlvmModule) {
                auto Address = rewrite_source::getOriginalVA(Declaration);
                ASSERT_TRUE(static_cast<bool>(Address));
                if (*Address && **Address == Omitted) {
                  EXPECT_TRUE(Declaration.isDeclaration());
                  Callee = &Declaration;
                }
              }
            }
            std::string IR;
            llvm::raw_string_ostream IROut(IR);
            Result.LlvmModule->print(IROut, nullptr);
            ASSERT_NE(Callee, nullptr) << IR;
            ASSERT_EQ(Callee->arg_size(), 2u) << IR;
            auto Float = [&](uint16_t Size) {
              return Size == 4 ? llvm::Type::getFloatTy(Context)
                               : llvm::Type::getDoubleTy(Context);
            };
            for (const auto &Selected : Result.MedFuncs) {
              const auto *Declaration =
                  Result.LlvmModule->getFunction(Selected.Name);
              ASSERT_NE(Declaration, nullptr) << Selected.Name;
              EXPECT_EQ(Declaration->getReturnType(), Float(Bytes)) << IR;
              ASSERT_EQ(Declaration->arg_size(), Widths.size() - 1);
              for (size_t I = 0; I < Declaration->arg_size(); ++I)
                EXPECT_EQ(Declaration->getArg(I)->getType(),
                          Float(Widths[I + 1]))
                    << IR;
            }
            EXPECT_EQ(Callee->getReturnType(), Float(Bytes)) << IR;
            EXPECT_EQ(Callee->getFunctionType()->getParamType(0),
                      Float(Widths[1]))
                << IR;
            EXPECT_EQ(Callee->getFunctionType()->getParamType(1),
                      Float(Widths[2]))
                << IR;
            if (Entries.count(Text)) {
              const auto *Forward =
                  Result.LlvmModule->getFunction("forward_fp");
              ASSERT_NE(Forward, nullptr);
              bool Indirect = false;
              for (const auto &Block : *Forward)
                for (const auto &Instruction : Block)
                  if (const auto *Call =
                          llvm::dyn_cast<llvm::CallBase>(&Instruction);
                      Call && Call->isIndirectCall()) {
                    Indirect = true;
                    EXPECT_EQ(Call->getFunctionType(),
                              Callee->getFunctionType());
                  }
              EXPECT_TRUE(Indirect) << IR;
            }
          }
          ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                          &Debug, &Image));
        } else {
          ASSERT_TRUE(
              HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
        }
        EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
        const std::string Entry = Entries.count(Outer)    ? "outer_fp"
                                  : Entries.count(Caller) ? "middle_fp"
                                                          : "forward_fp";
        const std::string Provider = Entries.count(Text)     ? "fixture_fp"
                                     : Entries.count(Caller) ? "forward_fp"
                                                             : "middle_fp";
        const std::string Type = Bytes == 4 ? kFloatSpelling : kDoubleSpelling;
        const std::string First =
            Widths[1] == 4 ? kFloatSpelling : kDoubleSpelling;
        const std::string Second =
            Widths[2] == 4 ? kFloatSpelling : kDoubleSpelling;
        source_call_execution_test::compileAndRun(
            Source + llvm::formatv(kHeaderFloatOracle, Type, Provider, First,
                                   Second, Entry)
                         .str());
      }
    }
  }
}

TEST(AAPCS64CallContract, FixedStackOperandsKeepNarrowWidthsAcrossBlocks) {
  class Header : public NullDebugContext {
  public:
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name != "fixture_stack")
        return std::nullopt;
      FunctionSym Function;
      Function.Name = Name.str();
      Function.PrototypeFromHeader = true;
      Function.PrototypeLibrary = "test-header";
      Function.ReturnType = NdType::makeInt(8, true);
      for (unsigned I = 0; I < 8; ++I)
        Function.Params.emplace_back("a" + std::to_string(I),
                                     NdType::makeInt(8, false));
      Function.Params.emplace_back("value",
                                   NdType::makePtr(NdType::makeInt(8, true)));
      Function.Params.emplace_back("byte", NdType::makeInt(1, false));
      Function.Params.emplace_back("half", NdType::makeInt(2, false));
      return Function;
    }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      auto Function =
          Address == Got ? resolvePrototype("fixture_stack") : std::nullopt;
      if (Function)
        Function->Addr = Address;
      return Function;
    }
  } Debug;
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF})
    for (unsigned Split : {0, 1, 2}) {
      SCOPED_TRACE(static_cast<unsigned>(Format));
      SCOPED_TRACE(Split);
      std::vector<uint32_t> Code{0xD10103FF,  // sub sp,sp,64
                                 0xA9037BFD,  // stp x29,x30,[sp,48]
                                 0x910003FD,  // mov x29,sp
                                 0x910083EA,  // add x10,sp,32
                                 0xD280054B,  // mov x11,42
                                 0xF900014B,  // str x11,[x10]
                                 0xF90003EA,  // str x10,[sp]
                                 0x5280156B,  // mov w11,171
                                 0x390023EB,  // strb w11,[sp,8]
                                 0x5282468B,  // mov w11,4660
                                 0x790023EB}; // strh w11,[sp,16]
      if (Split == 1)
        Code.push_back(0x14000001); // b next block
      if (Split == 2)
        Code.insert(Code.end(), {0x10000071, br(17), 0xFFFFFFFF});
      for (unsigned I = 0; I < 8; ++I)
        Code.push_back(0xD2800000 | ((I + 1) << 5) | I);
      const va_t Call = Text + Code.size() * 4;
      Code.insert(Code.end(), {adrp(16, Call, Got), ldrX(17, 16, Got), blr(17),
                               0xA9437BFD, 0x910103FF, Ret});
      auto Image = makeImage({{Text, Code}}, {{Text, "stack_wrapper"}},
                             {"fixture_stack"});
      Image.Format = Format;
      // PIE headers can map low values too. The stored scalar 42 must not
      // become a pointer merely because those bytes have image ownership.
      Segment HeaderBytes;
      HeaderBytes.Name = ".headers";
      HeaderBytes.VA = 0;
      HeaderBytes.Size = HeaderBytes.FileSz = 64;
      HeaderBytes.Data.resize(64);
      HeaderBytes.Flags = SegmentFlags::Readable;
      Image.Segments.push_back(std::move(HeaderBytes));
      Image.Imports.front().Module = "fixture.dll";
      for (bool LLVM : {false, true})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(LLVM);
          SCOPED_TRACE(NoOpt);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries = {Text};
          Options.LiftMode = LLVM;
          Options.SourceProjection = true;
          Options.NoOpt = NoOpt;
          auto Result = Pipeline().run(Image, Context, Options, &Debug);
          ASSERT_TRUE(Result.Success) << Result.Error;
          std::string Source;
          llvm::raw_string_ostream Output(Source);
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          ASSERT_TRUE(LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Output,
                                                 COptions, &Debug, &Image)
                           : HighCEmitter().emit(Result.HighFuncs, Output,
                                                 COptions, &Debug));
          const std::string Provider = R"(
int64_t fixture_stack(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                      int64_t *value, uint8_t byte, uint16_t half) {
  if (a0 != 1 || a1 != 2 || a2 != 3 || a3 != 4 || a4 != 5 || a5 != 6 ||
      a6 != 7 || a7 != 8 || !value || *value != 42 || byte != 171 ||
      half != 4660) __builtin_trap();
  *value = 71;
  return -27;
}
int main(void) { return stack_wrapper() != -27; }
)";
          for (llvm::StringRef Level : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(
                Source + Provider,
                {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
        }
    }
}

TEST(AAPCS64CallContract, AVectorStackCopyForwardsBothScalarArguments) {
  class Header : public NullDebugContext {
  public:
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name != "fixture_wide_stack")
        return std::nullopt;
      FunctionSym Function;
      Function.Name = Name.str();
      Function.PrototypeFromHeader = true;
      Function.PrototypeLibrary = "test-header";
      Function.ReturnType = NdType::makeInt(8, true);
      for (unsigned I = 0; I < 14; ++I)
        Function.Params.emplace_back(
            "a" + std::to_string(I),
            NdType::makeInt(I == 8 || I == 9 ? 4 : 8, false));
      return Function;
    }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      auto Function = Address == Got ? resolvePrototype("fixture_wide_stack")
                                     : std::nullopt;
      if (Function)
        Function->Addr = Address;
      return Function;
    }
  } Debug;
  // An ordinary compiler-generated fourteen-argument forwarder. Q0 copies
  // arguments 10 and 11 together; it is not an independent vector parameter.
  std::vector<uint32_t> Code{0xD10103FF, // sub sp,sp,64
                             0xA9462FEA, // ldp x10,x11,[sp,96]
                             0xB94043E8, // ldr w8,[sp,64]
                             0xB9404BE9, // ldr w9,[sp,72]
                             0x3DC017E0, // ldr q0,[sp,80]
                             0xB90003E8, // str w8,[sp]
                             0xA902FBEB, // stp x11,x30,[sp,40]
                             0xF90013EA, // str x10,[sp,32]
                             0x3D8007E0, // str q0,[sp,16]
                             0xB9000BE9, // str w9,[sp,8]
                             uint32_t(0x94000000 | ((Plt - (Text + 40)) / 4)),
                             0xF9401BFE, // ldr x30,[sp,48]
                             0x910103FF, // add sp,sp,64
                             Ret};
  for (BinaryFormat Format : {BinaryFormat::COFF, BinaryFormat::ELF})
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(testing::Message()
                     << int(Format) << ':' << LLVM << ':' << NoOpt);
        auto Image =
            makeImage({{Text, Code}, {Plt, pltEntry(Plt, Got)}},
                      {{Text, "wide_stack_forwarder"}}, {"fixture_wide_stack"});
        Image.Format = Format;
        Image.Imports.front().Module = "fixture.dll";
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Text};
        Options.LiftMode = LLVM;
        Options.SourceProjection = true;
        Options.NoOpt = NoOpt;
        auto Result = Pipeline().run(Image, Context, Options, &Debug);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Output(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        ASSERT_TRUE(LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Output,
                                               COptions, &Debug, &Image)
                         : HighCEmitter().emit(Result.HighFuncs, Output,
                                               COptions, &Debug));
        const std::string Provider = R"C(
static unsigned calls;
int64_t fixture_wide_stack(uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                          uint32_t a8, uint32_t a9, uint64_t a10, uint64_t a11,
                          uint64_t a12, uint64_t a13) {
  if (a0 != 1 || a1 != 2 || a2 != 3 || a3 != 4 || a4 != 5 || a5 != 6 ||
      a6 != 7 || a7 != 8 || a8 != 0x1234abcd || a9 != 0x9abc8765 ||
      a10 != UINT64_C(0x1020304050607080) ||
      a11 != UINT64_C(0x8877665544332211) || a12 != 13 || a13 != 14)
    __builtin_trap();
  ++calls;
  return 19;
}
int main(void) {
  return wide_stack_forwarder(1, 2, 3, 4, 5, 6, 7, 8, 0x1234abcd, 0x9abc8765,
                              UINT64_C(0x1020304050607080),
                              UINT64_C(0x8877665544332211), 13, 14) != 19 ||
         calls != 1;
}
)C";
        for (llvm::StringRef Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(
              Source + Provider,
              {Level, "-fsanitize=undefined", "-fsanitize-trap=all"});
      }
}

} // namespace
