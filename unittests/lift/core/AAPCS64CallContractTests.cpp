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
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/AsmParser/Parser.h"
#include "llvm/BinaryFormat/ELF.h"
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

TEST(AAPCS64CallContract, IndirectResultsKeepFloatingInputsBeforeTheBuffer) {
  constexpr va_t Callee = Text + 0x80;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 20)) / 4);
  BinaryImage Image = makeImage(
      {{Text,
        {0xA9BD7BFDu, SetFrame, 0xD28000E0u, 0x1E229000u, // X0=7, S0=5
         0x910043E8u, Call, // X8 is the result buffer, after the FP input
         0xB9401BE0u, 0xF9400BE1u, 0x0B010000u, 0xA8C37BFDu, Ret}},
       {Callee, {0xF9000100u, 0x1E380001u, 0xB9000901u, Ret}}},
      {{Text, "read_float_buffer"}, {Callee, "write_float_buffer"}}, {});
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    Image.Format = Format;
    Image.ExceptionMetadata.Functions.clear();
    if (Format == BinaryFormat::ELF) {
      // An ordinary entry's scalar parameters remain the same when the
      // omitted body has an unwind record but no authenticated personality.
      ExceptionFunction Unwind;
      Unwind.CodeRange = {Callee, Callee + 16};
      Unwind.Encoding = ExceptionEncoding::DwarfFDE;
      Unwind.ParseStatus = ExceptionParseStatus::Complete;
      Image.ExceptionMetadata.Functions.push_back(std::move(Unwind));
    }
    SCOPED_TRACE(static_cast<int>(Format));
    for (bool OmitCallee : {false, true}) {
      SCOPED_TRACE(OmitCallee);
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Text};
        if (!OmitCallee)
          Options.OnlyFunctionEntries.insert(Callee);
        Options.LiftMode = Options.SourceProjection = LLVM;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        const auto Caller = llvm::find_if(
            Result.MedFuncs, [](const MedFunc &F) { return F.Entry == Text; });
        ASSERT_NE(Caller, Result.MedFuncs.end());
        ASSERT_EQ(Caller->CallInfos.size(), 1u);
        const auto &Info = Caller->CallInfos.front();
        ASSERT_EQ(Info.Args.size(), 3u);
        EXPECT_EQ(Info.IntegerRegisterArgCount, 1);
        EXPECT_EQ(Info.FloatingRegisterArgCount, 1);
        EXPECT_EQ(Info.ArgReadBytes, (std::vector<uint16_t>{0, 4, 0}));
        EXPECT_EQ(Info.Args.back().Size, 8);
        if (OmitCallee) {
          ASSERT_TRUE(Info.SourceCalleeType);
          ASSERT_EQ(Info.SourceCalleeType->ParamTypes.size(), 3u);
          EXPECT_EQ(Info.SourceCalleeType->ParamTypes[1]->Kind,
                    NdTypeKind::Float);
          EXPECT_EQ(Info.SourceCalleeType->ParamTypes[1]->Size, 4);
          EXPECT_EQ(Info.SourceCalleeType->ParamTypes[2]->Kind,
                    NdTypeKind::Ptr);
        }
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        ASSERT_TRUE(LLVM
                        ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                              nullptr, &Image)
                        : HighCEmitter().emit(Result.HighFuncs, Out, COptions));
        if (OmitCallee) {
          // An independently supplied implementation must agree with the
          // omitted callee's projected C signature, including its low FP lane.
          Source += LLVM ? "\nuint64_t write_float_buffer(uint64_t value, "
                         : "\nint64_t write_float_buffer(int64_t value, ";
          Source += R"C(float input, void *buffer) {
  *(uint64_t *)buffer = value;
  *(int32_t *)((char *)buffer + 8) = (int32_t)input;
  return value;
}
)C";
        }
        Source += "\nint main(void) { return read_float_buffer() != 12; }\n";
        for (const char *Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(Source, {Level});
      }
    }
  }
}

TEST(AAPCS64CallContract, OmittedScalarFloatingResultKeepsItsParameterTypes) {
  constexpr va_t Callee = Text + 0x80;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 16)) / 4);
  for (bool Double : {false, true})
    for (bool Recursive : {false, true}) {
      SCOPED_TRACE(Double);
      SCOPED_TRACE(Recursive);
      std::vector<uint32_t> Body =
          Double ? std::vector<uint32_t>{0x1E620001u, 0x1E612800u, Ret}
                 : std::vector<uint32_t>{0x1E220001u, 0x1E212800u, Ret};
      if (Recursive)
        Body = {PushFrame,
                SetFrame,
                Double ? 0x1E604001u : 0x1E204001u, // S1/D1 = S0/D0
                0x7100001Fu,
                0x5400008Du, // cmp w0, 0; b.le done
                0x52800000u,
                0x97FFFFFAu,                        // w0 = 0; bl self
                Double ? 0x1E602800u : 0x1E202800u, // result += result
                PopFrame,
                Ret};
      auto Image = makeImage(
          {{Text,
            {PushFrame, SetFrame, 0x2A0003E0u, // mov w0, w0
             Double ? 0x1E629000u : 0x1E229000u, Call,
             Double ? 0x1E780000u : 0x1E380000u, PopFrame, Ret}}, // S0/D0 = 5
           {Callee, Body}},
          {{Text, "use_scalar_float"}, {Callee, "scalar_float_result"}}, {});
      for (auto Format :
           {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
        Image.Format = Format;
        SCOPED_TRACE(static_cast<int>(Format));
        for (bool LLVM : {false, true}) {
          SCOPED_TRACE(LLVM);
          PipelineAnalysisCache Cache;
          for (bool Omit : {true, false, true}) {
            SCOPED_TRACE(Omit);
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.OnlyFunctionEntries = {Text};
            if (!Omit)
              Options.OnlyFunctionEntries.insert(Callee);
            Options.AnalysisCache = &Cache;
            Options.LiftMode = Options.SourceProjection = LLVM;
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_TRUE(Result.Success) << Result.Error;
            std::string Source;
            llvm::raw_string_ostream Out(Source);
            CEmitterOptions COptions;
            COptions.TheArch = Image.Arch;
            COptions.Format = Image.Format;
            COptions.Image = &Image;
            ASSERT_TRUE(
                LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                           nullptr, &Image)
                     : HighCEmitter().emit(Result.HighFuncs, Out, COptions));
            if (Omit) {
              const std::string Floating = Double ? "double" : "float";
              Source += "\n" + Floating + " scalar_float_result(";
              Source += LLVM ? "uint32_t integer, " : "int32_t integer, ";
              Source += Floating + " value) { return ";
              Source +=
                  Recursive
                      ? "(int32_t)integer > 0 ? value + value : value; }\n"
                      : "(" + Floating + ")(int32_t)integer + value; }\n";
            }
            Source += Recursive ? "\n#define EXPECTED(x) ((x) > 0 ? 10 : 5)\n"
                                : "\n#define EXPECTED(x) ((x) + 5)\n";
            Source += R"C(
int main(void) {
  int32_t values[] = {0, 1, -7, 100};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    if ((int32_t)use_scalar_float(values[i]) != EXPECTED(values[i]))
      return 1;
  return 0;
}
)C";
            for (const char *Level : {"-O0", "-O2"})
              source_call_execution_test::compileAndRun(Source, {Level});
          }
        }
      }
    }
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
      EXPECT_NE(Source.find("incoming register has no definition"),
                std::string::npos)
          << Source;
      EXPECT_EQ(Source.find("unknown register"), std::string::npos) << Source;
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

TEST(AAPCS64CallContract, IndirectCallKeepsSetupBeforeDispatchBranch) {
  constexpr va_t Callee = Text + 0x80;
  const std::vector<uint32_t> Code{PushFrame,
                                   SetFrame,
                                   0xAA0003E8u, // mov x8,x0 (callback)
                                   0xAA0103E0u, // mov x0,x1
                                   0xAA0203E1u, // mov x1,x2
                                   0xAA0303E2u, // mov x2,x3
                                   0xB4000064u, // cbz x4,indirect
                                   0x94000000u | ((Callee - (Text + 28)) / 4),
                                   0x14000002u, // b done
                                   blr(8),
                                   PopFrame,
                                   Ret};
  const BinaryImage Image =
      makeImage({{Text, Code}, {Callee, {0x8B010000u, 0x8B020000u, Ret}}},
                {{Text, "dispatch_sum"}, {Callee, "sum_three"}}, {});
  llvm::LLVMContext Context;
  const auto Result = run(Image, Context, {Text, Callee});
  ASSERT_TRUE(Result.Success) << Result.Error;
  bool Found = false;
  for (const auto &Function : Result.MedFuncs)
    if (Function.Entry == Text)
      for (const auto &Call : Function.CallInfos)
        if (Call.IsIndirect) {
          Found = true;
          EXPECT_EQ(Call.Args.size(), 3u);
        }
  ASSERT_TRUE(Found);
  const std::string Source = highC(Image, {Text, Callee});
  source_call_execution_test::compileAndRun(Source + R"C(
uint64_t callback_sum(uint64_t a, uint64_t b, uint64_t c) {
  return a + b + c + 17;
}
int main(void) {
  for (uint64_t n = 0; n != 8; ++n) {
    if ((uint64_t)dispatch_sum((void *)(uintptr_t)&callback_sum, n, 7, 101, 0) !=
        n + 7 + 101 + 17) return 1;
    if ((uint64_t)dispatch_sum((void *)(uintptr_t)&callback_sum, n, 7, 101, 1) !=
        n + 7 + 101) return 2;
  }
  return 0;
}
)C");
}

TEST(AAPCS64CallContract, SplitIndirectSetupStopsAtClobbersAndAmbiguousEdges) {
  for (Arch Architecture : {Arch::AArch64, Arch::X64}) {
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::MachO, BinaryFormat::COFF}) {
      SCOPED_TRACE(unsigned(Architecture));
      SCOPED_TRACE(unsigned(Format));
      const auto Registers = getTargetRegInfo(Architecture)
                                 .integerArgumentLayout(Format)
                                 .Registers;
      for (unsigned Boundary = 0; Boundary != 5; ++Boundary)
        for (bool Reverse : {false, true}) {
          SCOPED_TRACE(Boundary);
          SCOPED_TRACE(Reverse);
          MedFunc Function;
          Function.Entry = Text;
          Function.Name = "split_indirect";
          Function.Blocks.resize(3);
          for (int I = 0; I != 3; ++I) {
            Function.Blocks[I].Id = I;
            Function.Blocks[I].StartAddr = Text + I * 16;
            Function.Blocks[I].EndAddr = Text + (I + 1) * 16;
            if (I)
              Function.Blocks[I].Preds = {I - 1};
            if (I != 2)
              Function.Blocks[I].Succs = {I + 1};
          }
          auto Set = [&](unsigned Index, uint64_t Value, int Version = 1) {
            MedOp Op;
            Op.Opcode = NdOp::COPY;
            Op.Output.Kind = MedVar::Reg;
            Op.Output.Id = Index + 1;
            Op.Output.SSAVer = Version;
            Op.Output.RegOff = Registers[Index];
            Op.Output.Size = 8;
            Op.Output.TheArch = Architecture;
            Op.addInput(MedVar::makeConst(Value, 8));
            return Op;
          };
          Function.Blocks[0].Ops = {Set(0, 11), Set(1, 22), Set(2, 33)};
          if (Boundary == 1 || Boundary == 2) {
            MedOp Call;
            Call.Opcode = NdOp::CALL;
            Call.Addr = Text + 16;
            Call.addInput(MedVar::makeConst(Plt, 8));
            Function.Blocks[1].Ops.push_back(Call);
            if (Boundary == 2)
              Function.Blocks[1].Ops.push_back(Set(0, 44, 2));
          } else if (Boundary == 3) {
            Function.Blocks[2].ExceptionalPreds = {{0, Text + 32}};
          } else if (Boundary == 4) {
            Function.Blocks[2].Preds = {0, 1};
            Function.Blocks[1].Preds.clear();
          }
          MedOp Indirect;
          Indirect.Opcode = NdOp::INDIR_CALL;
          Indirect.Addr = Text + 32;
          MedVar Target = Set(0, 0).Output;
          Target.Kind = MedVar::Temp;
          Target.Id = 20;
          Target.RegOff = 0;
          Indirect.addInput(Target);
          Function.Blocks[2].Ops.push_back(Indirect);
          if (Reverse)
            std::reverse(Function.Blocks.begin(), Function.Blocks.end());
          BinaryImage Image;
          Image.Arch = Architecture;
          Image.Format = Format;
          Image.Bits = Bitness::Bits64;
          recoverCallAbi(Function, Architecture, {}, &Image);
          const auto Call = llvm::find_if(
              Function.CallInfos, [](const auto &C) { return C.IsIndirect; });
          ASSERT_NE(Call, Function.CallInfos.end());
          ASSERT_EQ(Call->Args.size(), Boundary == 0   ? 3u
                                       : Boundary == 2 ? 1u
                                                       : 0u);
          for (size_t I = 0; I != Call->Args.size(); ++I) {
            ASSERT_TRUE(Call->Args[I].isConst());
            EXPECT_EQ(Call->Args[I].ConstVal,
                      Boundary == 2 ? 44u : 11u * (I + 1));
          }
        }
    }
  }
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

TEST(AAPCS64CallContract, ConstantReturnPhisKeepBothFieldsAcrossBranches) {
  constexpr va_t Callee = Text + 0x40;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 8)) / 4);
  auto Image = makeImage(
      {{Text, {PushFrame, SetFrame, Call, 0xCA010000u, PopFrame, Ret}},
       {Callee,
        {0xB40000C0u, // cbz x0, zero
         0xD28000E0u, // mov x0, #7
         0xF2E24000u, // movk x0, #0x1200, lsl #48
         0xD2800161u, // mov x1, #11
         0xF2E68001u, // movk x1, #0x3400, lsl #48
         0x14000003u, // b join
         0xD28001A0u, // zero: mov x0, #13
         0xD2800001u, // mov x1, #0
         0x7100143Fu, // join: cmp w1, #5 (a low view of the returned x1)
         0x54000049u, // b.ls exit
         0x91000400u, // add x0, x0, #1
         Ret}}},
      {{Text, "read_branch_pair"}, {Callee, "branch_pair"}}, {});
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    Image.Format = Format;
    SCOPED_TRACE(static_cast<int>(Format));
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      PipelineAnalysisCache Cache;
      for (const auto &Entries :
           {std::set<va_t>{Text}, std::set<va_t>{Text, Callee},
            std::set<va_t>{Text}}) {
        SCOPED_TRACE(Entries.size());
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = Entries;
        Options.AnalysisCache = &Cache;
        Options.LiftMode = Options.SourceProjection = LLVM;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        bool Pair = false;
        for (const auto &Function : Result.MedFuncs)
          if (Function.Entry == Text)
            for (const auto &Block : Function.Blocks)
              for (const auto &Op : Block.Ops)
                if (Op.Opcode == NdOp::CALL && Op.NumInputs &&
                    Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == Callee)
                  Pair = Op.Output.Kind == MedVar::Temp && Op.Output.Size == 16;
        ASSERT_TRUE(Pair);
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        ASSERT_TRUE(LLVM
                        ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                              nullptr, &Image)
                        : HighCEmitter().emit(Result.HighFuncs, Out, COptions));
        if (Entries.size() == 1)
          Source += R"c(
__typeof__(branch_pair(0)) branch_pair(uint64_t x) {
  __typeof__(branch_pair(0)) result;
  const uint64_t words[] = {x ? 0x1200000000000008ULL : 13,
                           x ? 0x340000000000000BULL : 0};
  _Static_assert(sizeof(result) == sizeof(words), "complete returned pair");
  __builtin_memcpy(&result, words, sizeof(words));
  return result;
}
)c";
        Source += R"c(
int main(void) {
  const uint64_t inputs[] = {0, 1, 7, 0xFFFFFFFFFFFFFFFFULL};
  for (unsigned i = 0; i != sizeof(inputs) / sizeof(inputs[0]); ++i)
    if ((uint64_t)read_branch_pair(inputs[i]) !=
        (inputs[i] ? 0x2600000000000003ULL : 13))
      return 1;
  return 0;
}
)c";
        for (const char *Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(Source, {Level});
      }
    }
  }
}

TEST(AAPCS64CallContract, SharedAggregateReturnKeepsBothFields) {
  constexpr va_t Callee = Text + 0x40;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 8)) / 4);
  for (const uint32_t Second : {0u, 42u}) {
    SCOPED_TRACE(Second);
    auto Image = makeImage(
        {{Text, {PushFrame, SetFrame, Call, 0xCA010000u, PopFrame, Ret}},
         {Callee,
          {0xB4000180u, // cbz x0, zero
           0x360800A0u, // tbz w0, #1, second
           0xD1000400u, // first: sub x0, x0, #1
           0xB4000120u, // cbz x0, zero
           0x37100040u, // tbnz w0, #2, second
           0x17FFFFFDu, // b first
           0xD1000400u, // second: sub x0, x0, #1
           0xB40000A0u, // cbz x0, zero
           0x3617FF40u, // tbz w0, #2, first
           0xD28000E0u, // mov x0, #7
           0xD2800161u, // mov x1, #11
           Ret,
           0xD2800000u,                 // zero: mov x0, #0
           0xD2800001u | (Second << 5), // mov x1, second field
           Ret}}},
        {{Text, "read_zero_pair"}, {Callee, "zero_pair"}}, {});
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      Image.Format = Format;
      SCOPED_TRACE(static_cast<int>(Format));
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Text, Callee};
        Options.LiftMode = Options.SourceProjection = LLVM;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        ASSERT_TRUE(LLVM
                        ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                              nullptr, &Image)
                        : HighCEmitter().emit(Result.HighFuncs, Out, COptions));
        Source += "\n#define SHARED_SECOND " + std::to_string(Second) + "\n";
        Source += R"c(
int main(void) {
  const uint64_t inputs[] = {0, 1, 2, 5, 6, 7, 15};
  for (unsigned i = 0; i != sizeof(inputs) / sizeof(inputs[0]); ++i)
    if ((uint64_t)read_zero_pair(inputs[i]) !=
        (inputs[i] >= 5 ? 12 : SHARED_SECOND))
      return 1;
  return 0;
}
)c";
        for (const char *Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(Source, {Level});
      }
    }
  }
}

TEST(AAPCS64CallContract, ReturnFieldsConsumedThroughPhisStayObserved) {
  constexpr va_t Callee = Text + 0x40;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 12)) / 4);
  for (bool ConsumeSecond : {true, false}) {
    SCOPED_TRACE(ConsumeSecond);
    auto Image = makeImage(
        {{Text,
          {PushFrame, SetFrame,
           0xB4000060u, // cbz x0, literal
           Call,
           0x14000003u,                               // b join
           0xD28001A0u,                               // literal: mov x0, #13
           0xD2800221u,                               // mov x1, #17
           ConsumeSecond ? 0xCA010000u : 0xAA0003E0u, // join: xor or copy x0
           PopFrame, Ret}},
         {Callee, {0xD37DF001u, 0x91001C00u, Ret}}},
        {{Text, "joined_pair"}, {Callee, "phi_pair"}}, {});
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      Image.Format = Format;
      SCOPED_TRACE(static_cast<int>(Format));
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM);
        PipelineAnalysisCache Cache;
        for (const auto &Entries :
             {std::set<va_t>{Text}, std::set<va_t>{Text, Callee},
              std::set<va_t>{Text}}) {
          // A selected body can independently prove its return record. The
          // unread-PHI case checks only the caller's observation inference.
          if (!ConsumeSecond && Entries.size() != 1)
            continue;
          SCOPED_TRACE(Entries.size());
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries = Entries;
          Options.AnalysisCache = &Cache;
          Options.LiftMode = Options.SourceProjection = LLVM;
          const auto Result = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Result.Success) << Result.Error;
          bool Pair = false;
          for (const auto &Function : Result.MedFuncs)
            if (Function.Entry == Text)
              for (const auto &Block : Function.Blocks)
                for (const auto &Op : Block.Ops)
                  if (Op.Opcode == NdOp::CALL && Op.NumInputs &&
                      Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == Callee)
                    Pair =
                        Op.Output.Kind == MedVar::Temp && Op.Output.Size == 16;
          ASSERT_EQ(Pair, ConsumeSecond);
          if (!ConsumeSecond)
            continue;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          ASSERT_TRUE(
              LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                         nullptr, &Image)
                   : HighCEmitter().emit(Result.HighFuncs, Out, COptions));
          if (Entries.size() == 1)
            Source += R"c(
__typeof__(phi_pair(0)) phi_pair(uint64_t x) {
  __typeof__(phi_pair(0)) result;
  const uint64_t words[] = {x + 7, x << 3};
  _Static_assert(sizeof(result) == sizeof(words), "complete returned pair");
  __builtin_memcpy(&result, words, sizeof(words));
  return result;
}
)c";
          Source += R"c(
int main(void) {
  const uint64_t values[] = {0, 1, 7, 0x123456789ABCDEF0ULL, UINT64_MAX};
  for (unsigned i = 0; i != sizeof(values) / sizeof(values[0]); ++i) {
    const uint64_t x = values[i];
    if ((uint64_t)joined_pair(x) != (x ? ((x + 7) ^ (x << 3)) : (13 ^ 17)))
      return 1;
  }
  return 0;
}
)c";
          for (const char *Level : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(Source, {Level});
        }
      }
    }
  }
}

TEST(AAPCS64CallContract, FloatingReturnFieldsConsumedThroughPhisStayObserved) {
  constexpr va_t Callee = Text + 0x40;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 12)) / 4);
  auto Image =
      makeImage({{Text,
                  {PushFrame, SetFrame, 0xB4000060u, Call, 0x14000003u,
                   0x1E6E1000u, // literal: fmov d0, #1.0
                   0x1E601001u, // fmov d1, #2.0
                   0x1E612800u, // join: fadd d0, d0, d1
                   0x9E790000u, // fcvtzu x0, d0
                   PopFrame, Ret}},
                 {Callee,
                  {0x9E630000u, // ucvtf d0, x0
                   0x1E602801u, // fadd d1, d0, d0
                   Ret}}},
                {{Text, "joined_fp_pair"}, {Callee, "phi_fp_pair"}}, {});
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    Image.Format = Format;
    SCOPED_TRACE(static_cast<int>(Format));
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      for (const auto &Entries :
           {std::set<va_t>{Text}, std::set<va_t>{Text, Callee}}) {
        SCOPED_TRACE(Entries.size());
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = Entries;
        Options.LiftMode = Options.SourceProjection = LLVM;
        const auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        ASSERT_TRUE(LLVM
                        ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                              nullptr, &Image)
                        : HighCEmitter().emit(Result.HighFuncs, Out, COptions));
        if (Entries.size() == 1) {
          Source += std::string("\n#define FP_PAIR_INPUT ") +
                    (LLVM ? "uint64_t\n" : "int64_t\n");
          Source += R"c(
__typeof__(phi_fp_pair(0)) phi_fp_pair(FP_PAIR_INPUT x) {
  __typeof__(phi_fp_pair(0)) result;
  const double words[] = {(double)(uint64_t)x, (double)(uint64_t)x * 2.0};
  _Static_assert(sizeof(result) == sizeof(words), "complete floating pair");
  __builtin_memcpy(&result, words, sizeof(words));
  return result;
}
)c";
        }
        Source += R"c(
int main(void) {
  for (uint64_t x = 0; x != 32; ++x)
    if ((uint64_t)joined_fp_pair(x) != (x ? x * 3 : 3)) return 1;
  return 0;
}
)c";
        for (const char *Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(Source, {Level});
      }
    }
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

TEST(AAPCS64CallContract, AggregateFieldsIncludeTheExplicitCallResult) {
  constexpr va_t Pair = Text + 0x40, Leaf = Text + 0x80;
  const uint32_t CallPair = 0x94000000u | ((Pair - (Text + 8)) / 4);
  const uint32_t CallLeaf = 0x94000000u | ((Leaf - (Pair + 8)) / 4);
  auto Image = makeImage(
      {{Text, {PushFrame, SetFrame, CallPair, 0xCA010000u, PopFrame, Ret}},
       {Pair, {PushFrame, SetFrame, CallLeaf, 0xD2800161u, PopFrame, Ret}},
       {Leaf, {0xD28000E0u, Ret}}},
      {{Text, "read_pair_with_call"},
       {Pair, "pair_with_call"},
       {Leaf, "first_field"}},
      {});
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    Image.Format = Format;
    SCOPED_TRACE(static_cast<int>(Format));
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      PipelineAnalysisCache Cache;
      for (const auto &Entries :
           {std::set<va_t>{Text}, std::set<va_t>{Text, Pair, Leaf},
            std::set<va_t>{Text}}) {
        SCOPED_TRACE(Entries.size());
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = Entries;
        Options.AnalysisCache = &Cache;
        Options.LiftMode = Options.SourceProjection = LLVM;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        for (const auto &Function : Result.MedFuncs)
          if (Function.Entry == Text)
            for (const auto &Block : Function.Blocks)
              for (const auto &Op : Block.Ops)
                if (Op.Opcode == NdOp::CALL && Op.NumInputs &&
                    Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == Pair)
                  EXPECT_EQ(Op.Output.Size, 16);
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        ASSERT_TRUE(LLVM
                        ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                              nullptr, &Image)
                        : HighCEmitter().emit(Result.HighFuncs, Out, COptions));
        if (Entries.size() == 1)
          Source += R"c(
__typeof__(pair_with_call()) pair_with_call(void) {
  __typeof__(pair_with_call()) result;
  _Static_assert(sizeof(result) == 16, "complete returned pair");
  const uint64_t fields[2] = {7, 11};
  __builtin_memcpy(&result, fields, sizeof(fields));
  return result;
}
)c";
        Source += "\nint main(void) { return read_pair_with_call() != 12; }\n";
        for (const char *Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(Source, {Level});
      }
    }
  }
}

TEST(AAPCS64CallContract, SelectedFloatingRecordsKeepEveryObservedField) {
  constexpr va_t Callee = Text + 0x60;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 8)) / 4);
  for (bool FirstRead : {false, true}) {
    SCOPED_TRACE(FirstRead);
    std::vector<uint32_t> Words{PushFrame, SetFrame, Call};
    if (FirstRead)
      Words.push_back(0x1E212800u); // fadd s0, s0, s1
    Words.push_back(FirstRead ? 0x1E260000u : 0x1E260020u);
    Words.insert(Words.end(), {PopFrame, Ret});
    auto Image =
        makeImage({{Text, Words}, {Callee, {0x1E211000u, 0x1E229001u, Ret}}},
                  {{Text, "read_hfa"}, {Callee, "omitted_hfa"}}, {});
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Format));
      Image.Format = Format;
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM);
        PipelineAnalysisCache Cache;
        for (const auto &Entries :
             {std::set<va_t>{Text}, std::set<va_t>{Text, Callee},
              std::set<va_t>{Text}}) {
          SCOPED_TRACE(Entries.size());
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries = Entries;
          Options.AnalysisCache = &Cache;
          Options.LiftMode = Options.SourceProjection = LLVM;
          auto Result = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Result.Success) << Result.Error;
          std::string Source;
          llvm::raw_string_ostream Out(Source);
          CEmitterOptions COptions;
          COptions.TheArch = Image.Arch;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          ASSERT_TRUE(
              LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                         nullptr, &Image)
                   : HighCEmitter().emit(Result.HighFuncs, Out, COptions));
          ASSERT_EQ(Source.find("no recovered semantics"), std::string::npos)
              << Source;
          if (Entries.size() == 1)
            Source += R"C(
#include <string.h>
__typeof__(omitted_hfa()) omitted_hfa(void) {
  __typeof__(omitted_hfa()) result;
  float fields[2] = {3.0f, 5.0f};
  _Static_assert(sizeof result == sizeof fields, "HFA field extent");
  memcpy(&result, fields, sizeof fields);
  return result;
}


)C";
          Source += "\nint main(void) { return (uint32_t)read_hfa() != " +
                    std::to_string(FirstRead ? 0x41000000u : 0x40a00000u) +
                    "u; }\n";
          for (const char *Level : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(Source, {Level});
        }
      }
    }
  }
}

TEST(AAPCS64CallContract, FloatingRecordFieldsNeedDefinitionsOnEveryReturn) {
  constexpr va_t Callee = Text + 0x60;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 8)) / 4);
  // The first arm returns without defining S1. Reading that register after
  // the call cannot authenticate the proposed second field.
  const auto Image = makeImage(
      {{Text, {PushFrame, SetFrame, Call, 0x1E260020u, PopFrame, Ret}},
       {Callee, {0x34000040u, 0x1E229001u, 0x1E211000u, Ret}}},
      {{Text, "read_partial_hfa"}, {Callee, "partial_hfa"}}, {});
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
    bool Checked = false;
    for (const auto &Function : Result.MedFuncs)
      if (Function.Entry == Text)
        for (const auto &Block : Function.Blocks)
          for (const auto &Op : Block.Ops)
            if (Op.Opcode == NdOp::CALL && Op.NumInputs &&
                Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == Callee) {
              EXPECT_FALSE(Op.RegisterReturnFields);
              Checked = true;
            }
    EXPECT_TRUE(Checked);
  }
}

TEST(AAPCS64CallContract, FloatingRecordCallsKeepScalarArgumentWidths) {
  constexpr va_t Callee = Text + 0x60;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 16)) / 4);
  // Both an integer and a float parameter participate in the returned pair.
  // An old-style C declaration promotes the float to double and changes S0.
  auto Image =
      makeImage({{Text,
                  {PushFrame, SetFrame, 0x528000E0u, 0x1E229000u, Call,
                   0x1E212800u, 0x1E260000u, PopFrame, Ret}},
                 {Callee, {0x1E204001u, 0x1E220000u, Ret}}},
                {{Text, "read_parameter_hfa"}, {Callee, "parameter_hfa"}}, {});
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    Image.Format = Format;
    SCOPED_TRACE(static_cast<int>(Format));
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      PipelineAnalysisCache Cache;
      for (const auto &Entries :
           {std::set<va_t>{Text}, std::set<va_t>{Text, Callee},
            std::set<va_t>{Text}}) {
        SCOPED_TRACE(Entries.size());
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = Entries;
        Options.AnalysisCache = &Cache;
        Options.LiftMode = Options.SourceProjection = LLVM;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        ASSERT_TRUE(LLVM
                        ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                              nullptr, &Image)
                        : HighCEmitter().emit(Result.HighFuncs, Out, COptions));
        if (Entries.size() == 1) {
          // LLVM integers carry no signedness; HighC's recovered word is
          // signed. Both prototypes pass the same complete GP register.
          Source += LLVM ? "\ntypedef uint64_t hfa_word;\n"
                         : "\ntypedef int64_t hfa_word;\n";
          Source += R"c(
__typeof__(parameter_hfa(0, 0.0f)) parameter_hfa(hfa_word integer, float value) {
  __typeof__(parameter_hfa(0, 0.0f)) result;
  float fields[2] = {(float)(int32_t)integer, value};
  __builtin_memcpy(&result, fields, sizeof(fields));
  return result;
}
)c";
        }
        Source += "\nint main(void) { return (uint32_t)read_parameter_hfa() != "
                  "UINT32_C(0x41400000); }\n";
        for (const char *Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(Source, {Level});
      }
    }
  }
}

TEST(AAPCS64CallContract, MissingFixedFloatingInputsKeepTheirPositions) {
  const auto &TRI = getTargetRegInfo(Arch::AArch64);
  constexpr va_t Callee = Text + 0x40;
  for (unsigned Mode : {0, 1, 2, 3, 4, 5, 6, 7}) {
    SCOPED_TRACE(Mode);
    const bool HasResultPointer = Mode >= 4;
    const unsigned EntryMode = Mode % 4;
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
    if (EntryMode == 1)
      F.Blocks.front().Ops.push_back(Call(Text + 0x80));
    if (EntryMode >= 2) {
      F.Blocks.emplace_back();
      F.Blocks.back().Id = 1;
      F.Blocks.back().StartAddr = Text + 32;
      F.Blocks.back().EndAddr = Text + 48;
      if (EntryMode == 2)
        F.ModuleAnalysisRoots.insert(Text + 32);
      else
        F.Blocks.back().ExceptionalPreds.emplace_back();
    }
    F.Blocks.back().Ops.push_back(Call(Callee));
    std::map<va_t, int> Integers{{Callee, 0}}, Total = Integers;
    const std::map<va_t, int> Floating{{Callee, 2}};
    const std::map<va_t, bool> Results{{Callee, HasResultPointer}};
    const std::map<va_t, std::vector<uint64_t>> Registers{
        {Callee, {TRI.FPParamRegs[0], TRI.FPParamRegs[1]}}};
    const std::map<va_t, std::map<uint64_t, uint16_t>> Widths{
        {Callee, {{TRI.FPParamRegs[0], 4}, {TRI.FPParamRegs[1], 8}}}};
    recoverCallAbi(F, Image.Arch, {}, &Image, &Integers, &Total, &Floating,
                   nullptr, &Registers, &Results, nullptr, nullptr, nullptr,
                   nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                   &Widths);
    ASSERT_FALSE(F.CallInfos.empty());
    const auto &C = F.CallInfos.back();
    ASSERT_EQ(C.Args.size(), HasResultPointer ? 3u : 2u);
    EXPECT_EQ(C.IntegerRegisterArgCount, 0);
    EXPECT_EQ(C.FloatingRegisterArgCount, 2);
    EXPECT_EQ(C.ArgReadBytes, HasResultPointer
                                  ? (std::vector<uint16_t>{4, 8, 0})
                                  : (std::vector<uint16_t>{4, 8}));
    for (size_t I = 0; I < 2; ++I)
      EXPECT_EQ(C.Args[I].Kind,
                EntryMode == 0 ? MedVar::Param : MedVar::Unspecified);
    if (HasResultPointer)
      EXPECT_EQ(C.Args.back().Size, 8);
    if (EntryMode)
      EXPECT_TRUE(F.Params.empty());
  }
}

TEST(AAPCS64CallContract, FloatingInputsCrossJoinsOnlyWithOneReachingValue) {
  constexpr va_t Callee = Text + 0x100;
  for (auto TheArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(TheArch));
    const auto &TRI = getTargetRegInfo(TheArch);
    for (unsigned Mode = 0; Mode < 9; ++Mode) {
      SCOPED_TRACE(Mode);
      BinaryImage Image;
      Image.Arch = TheArch;
      Image.Format = BinaryFormat::ELF;
      MedFunc F;
      F.Entry = Text;
      F.CC = TheArch == Arch::AArch64 ? CallingConv::ARM_AAPCS
                                      : CallingConv::SysV_AMD64;
      for (int I = 0; I < 4; ++I) {
        auto &B = F.Blocks.emplace_back();
        B.Id = I;
        B.StartAddr = Text + I * 16;
        B.EndAddr = B.StartAddr + 16;
      }
      auto Edge = [&](int From, int To) {
        F.Blocks[From].Succs.push_back(To);
        F.Blocks[To].Preds.push_back(From);
      };
      Edge(0, 1);
      Edge(0, 2);
      Edge(1, 3);
      Edge(2, 3);
      auto Call = [](va_t Target) {
        MedOp Op;
        Op.Opcode = NdOp::CALL;
        Op.addInput(MedVar::makeConst(Target, 8));
        return Op;
      };
      MedVar Value;
      Value.Kind = MedVar::Reg;
      Value.TheArch = Image.Arch;
      Value.RegOff = TRI.FPParamRegs[0];
      Value.Id = 100;
      Value.SSAVer = 1;
      Value.Size = 4;
      MedOp Write;
      Write.Opcode = NdOp::COPY;
      Write.Output = Value;
      Write.addInput(MedVar::makeConst(0x40A00000, 4));
      F.Blocks[0].Ops.push_back(Write);
      if (Mode == 1) {
        // A shared upstream PHI, as well as an instruction, is one value.
        F.Blocks[0].Succs = {1};
        F.Blocks[2].Preds.clear();
        Edge(1, 2);
        auto &Phi = F.Blocks[1].Phis.emplace_back();
        Phi.Args.push_back({0, Value});
        ++Value.SSAVer;
        Phi.Output = Value;
      } else if (Mode == 2) {
        Write.Output.SSAVer = 2;
        F.Blocks[2].Ops.push_back(Write);
      } else if (Mode == 3) {
        F.Blocks[2].Ops.push_back(Call(Text + 0x200));
      } else if (Mode == 4) {
        F.Blocks[2].ExceptionalPreds.emplace_back();
      } else if (Mode == 5) {
        F.ModuleAnalysisRoots.insert(F.Blocks[2].StartAddr);
      } else if (Mode == 8) {
        // A previous iteration's call clobbers the next iteration's input.
        Edge(3, 3);
      } else if (Mode >= 6) {
        // A non-writing loop keeps its input. A call in that loop does not.
        Edge(2, 2);
        if (Mode == 7)
          F.Blocks[2].Ops.push_back(Call(Text + 0x200));
      }
      F.Blocks[3].Ops.push_back(Call(Callee));
      std::map<va_t, int> Integers{{Callee, 0}}, Total = Integers;
      const std::map<va_t, int> Floating{{Callee, 1}};
      const std::map<va_t, std::vector<uint64_t>> Registers{
          {Callee, {TRI.FPParamRegs[0]}}};
      const std::map<va_t, std::map<uint64_t, uint16_t>> Widths{
          {Callee, {{TRI.FPParamRegs[0], 4}}}};
      recoverCallAbi(F, Image.Arch, {}, &Image, &Integers, &Total, &Floating,
                     nullptr, &Registers, nullptr, nullptr, nullptr, nullptr,
                     nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                     &Widths);
      ASSERT_FALSE(F.CallInfos.empty());
      const auto &Info = F.CallInfos.back();
      ASSERT_EQ(Info.Args.size(), 1u);
      if (Mode <= 1 || Mode == 6) {
        EXPECT_EQ(Info.Args.front(), Value);
        EXPECT_EQ(Info.Args.front().Size, 4);
      } else {
        EXPECT_EQ(Info.Args.front().Kind, MedVar::Unspecified);
      }
    }
  }
}

TEST(AAPCS64CallContract, FloatingPhiKeepsTheCompleteRegisterView) {
  constexpr va_t Callee = Text + 0x100;
  for (auto TheArch : {Arch::AArch64, Arch::X64}) {
    SCOPED_TRACE(static_cast<int>(TheArch));
    const auto &TRI = getTargetRegInfo(TheArch);
    for (bool Direct : {false, true})
      for (bool Known : {false, true}) {
        // Upstream recovery requires a fixed contract. Unknown calls retain
        // their existing local argument discovery and cannot widen its reads.
        if (!Direct && !Known)
          continue;
        for (bool Reverse : {false, true}) {
          SCOPED_TRACE(Direct);
          SCOPED_TRACE(Known);
          SCOPED_TRACE(Reverse);
          BinaryImage Image;
          Image.Arch = TheArch;
          Image.Format = BinaryFormat::ELF;
          MedFunc F;
          F.Entry = Text;
          for (int I = 0; I < 5; ++I) {
            auto &B = F.Blocks.emplace_back();
            B.Id = I;
            B.StartAddr = Text + I * 16;
            B.EndAddr = B.StartAddr + 16;
          }
          const auto Edge = [&](int From, int To) {
            F.Blocks[From].Succs.push_back(To);
            F.Blocks[To].Preds.push_back(From);
          };
          Edge(0, 1);
          Edge(0, 2);
          Edge(1, 3);
          Edge(2, 3);
          Edge(3, 4);
          MedVar Wide;
          Wide.Kind = MedVar::Reg;
          Wide.TheArch = TheArch;
          Wide.RegOff = TRI.FPParamRegs[0];
          Wide.Id = 100;
          Wide.SSAVer = 1;
          Wide.Size = 16;
          MedVar Narrow = Wide;
          Narrow.Id = 101;
          Narrow.Size = 8;
          PhiNode Full;
          Full.Output = Wide;
          Full.Args = {{1, MedVar::makeConst(0x40A00000, 16)},
                       {2, MedVar::makeConst(0, 16)}};
          PhiNode OldView;
          OldView.Output = Narrow;
          OldView.Args = {{1, MedVar::makeUnspecified(8, TheArch)},
                          {2, MedVar::makeConst(0, 8)}};
          F.Blocks[3].Phis = Reverse ? std::vector<PhiNode>{Full, OldView}
                                     : std::vector<PhiNode>{OldView, Full};
          MedOp Call;
          Call.Opcode = NdOp::CALL;
          Call.addInput(MedVar::makeConst(Callee, 8));
          F.Blocks[Direct ? 3 : 4].Ops.push_back(Call);
          std::map<va_t, int> Integers{{Callee, 0}}, Total = Integers;
          const std::map<va_t, int> Floating{{Callee, 1}};
          const std::map<va_t, std::vector<uint64_t>> Registers{
              {Callee, {TRI.FPParamRegs[0]}}};
          const std::map<va_t, std::map<uint64_t, uint16_t>> Widths{
              {Callee, {{TRI.FPParamRegs[0], 4}}}};
          recoverCallAbi(F, TheArch, {}, &Image, &Integers, &Total,
                         Known ? &Floating : nullptr, nullptr, &Registers,
                         nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
                         nullptr, nullptr, nullptr, nullptr, &Widths);
          ASSERT_EQ(F.CallInfos.size(), 1u);
          ASSERT_EQ(F.CallInfos[0].Args.size(), 1u);
          const auto &Expected = Known || Reverse ? Wide : Narrow;
          EXPECT_EQ(F.CallInfos[0].Args[0], Expected);
          EXPECT_EQ(F.CallInfos[0].Args[0].Size, Expected.Size);
        }
      }
  }
}

TEST(AAPCS64CallContract, ScalarAndWideFloatingWritesMeetBeforeACall) {
  constexpr va_t Callee = Text + 0x80;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 24)) / 4);
  for (bool Incoming : {false, true}) {
    SCOPED_TRACE(Incoming);
    auto Image =
        makeImage({{Text,
                    {PushFrame, SetFrame, 0x34000060u,     // cbz w0, zero
                     Incoming ? 0x1E204001u : 0x1E229000u, // s1 = s0 / s0 = 5
                     0x14000002u,                          // b join
                     0x2F00E400u,                          // movi d0, 0
                     Call, PopFrame, Ret}},
                   {Callee, {0x1E380000u, Ret}}}, // fcvtzs w0, s0
                  {{Text, "choose_float_view"}, {Callee, "float_to_word"}}, {});
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      Image.Format = Format;
      SCOPED_TRACE(static_cast<int>(Format));
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Text, Callee};
        Options.LiftMode = Options.SourceProjection = LLVM;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        ASSERT_TRUE(LLVM
                        ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                              nullptr, &Image)
                        : HighCEmitter().emit(Result.HighFuncs, Out, COptions));
        Source += Incoming ? R"C(
int main(void) {
  return choose_float_view(0, 7.0f) != 0 ||
         choose_float_view(1, 7.0f) != 7;
}
)C"
                           : R"C(
int main(void) {
  return choose_float_view(0) != 0 || choose_float_view(1) != 5;
}
)C";
        for (const char *Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(Source, {Level});
      }
    }
  }
}

TEST(AAPCS64CallContract, FloatingSetupDominatesAJoinBeforeTheCall) {
  constexpr va_t Callee = Text + 0x80;
  const uint32_t Call = 0x94000000u | ((Callee - (Text + 32)) / 4);
  auto Image =
      makeImage({{Text,
                  {PushFrame, SetFrame, 0x1E229000u, // S0 = 5
                   0x34000060u, 0x52800021u, 0x14000002u, 0x52800041u,
                   0x2A0103E0u, Call, 0x1E380000u, PopFrame, Ret}},
                 {Callee, {0x1E220001u, 0x1E212800u, Ret}}},
                {{Text, "join_then_call"}, {Callee, "add_floating_input"}}, {});
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    Image.Format = Format;
    SCOPED_TRACE(static_cast<int>(Format));
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      llvm::LLVMContext Context;
      auto Result = run(Image, Context, {Text, Callee}, LLVM);
      ASSERT_TRUE(Result.Success) << Result.Error;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      CEmitterOptions Options;
      Options.TheArch = Image.Arch;
      Options.Format = Image.Format;
      Options.Image = &Image;
      ASSERT_TRUE(LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, Out, Options,
                                             nullptr, &Image)
                       : HighCEmitter().emit(Result.HighFuncs, Out, Options));
      Source += R"c(
int main(void) {
  return join_then_call(0) != 7 || join_then_call(1) != 6;
}
)c";
      for (const char *Level : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(Source, {Level});
    }
  }
}

TEST(AAPCS64CallContract, FloatingRecordCallsShareOneFixedPrototype) {
  constexpr va_t Missing = Text + 0x40, Callee = Text + 0xC0;
  auto BL = [](va_t At, va_t To) {
    return 0x94000000u | (static_cast<uint32_t>((To - At) / 4) & 0x3FFFFFFu);
  };
  // The second caller reaches its HFA call through two clobbering calls.
  // Failure to recover S0 there must not invent a shorter callee signature.
  auto Image = makeImage(
      {{Text,
        {PushFrame, SetFrame, 0x528000E0u, 0x1E229000u, BL(Text + 16, Callee),
         0x1E212800u, 0x1E260000u, PopFrame, Ret}},
       {Missing,
        {PushFrame, SetFrame, 0x34000080u, BL(Missing + 12, Plt), 0x14000003u,
         0xD503201Fu, BL(Missing + 24, Plt), 0x528000E0u,
         BL(Missing + 32, Callee), 0x1E212800u, 0x1E260000u, PopFrame, Ret}},
       {Callee, {0x1E204001u, 0x1E220000u, Ret}},
       {Plt, pltEntry(Plt, Got)}},
      {{Text, "known_hfa_input"},
       {Missing, "missing_hfa_input"},
       {Callee, "shared_hfa"}},
      {"getuid"});
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    Image.Format = Format;
    SCOPED_TRACE(static_cast<int>(Format));
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      PipelineAnalysisCache Cache;
      for (const auto &Entries : {std::set<va_t>{Text, Missing},
                                  std::set<va_t>{Text, Missing, Callee},
                                  std::set<va_t>{Text, Missing}}) {
        SCOPED_TRACE(Entries.size());
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = Entries;
        Options.AnalysisCache = &Cache;
        Options.LiftMode = Options.SourceProjection = LLVM;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Out(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        ASSERT_TRUE(LLVM
                        ? LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                              nullptr, &Image)
                        : HighCEmitter().emit(Result.HighFuncs, Out, COptions));
        if (Entries.size() == 2) {
          Source += LLVM ? "\ntypedef uint64_t hfa_word;\n"
                         : "\ntypedef int64_t hfa_word;\n";
          Source += R"c(
__typeof__(shared_hfa(0, 0.0f)) shared_hfa(hfa_word integer, float value) {
  __typeof__(shared_hfa(0, 0.0f)) result;
  float fields[2] = {(float)(int32_t)integer, value};
  __builtin_memcpy(&result, fields, sizeof(fields));
  return result;
}
)c";
        }
        Source += R"c(
#include <signal.h>
#include <stdlib.h>
static void expected_trap(int sig) { _Exit(0); }
int main(void) {
  if ((uint32_t)known_hfa_input() != UINT32_C(0x41400000)) return 1;
  signal(SIGILL, expected_trap);
#ifdef SIGTRAP
  signal(SIGTRAP, expected_trap);
#endif
  missing_hfa_input(0);
  return 2;
}
)c";
        for (const char *Level : {"-O0", "-O2"})
          source_call_execution_test::compileAndRun(Source, {Level});
      }
    }
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

TEST(AAPCS64CallContract, CallEffectsFollowDeepLowerAddressChainsToCompletion) {
  constexpr va_t First = 0x10000, Zero = 0x20000, One = 0x20040,
                 Caller = 0x30000;
  constexpr size_t Chain = 273; // Exceeds the former 256-callee cutoff.
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
  for (size_t I = 0; I <= Chain; ++I) {
    const auto At = First + I * 32;
    if (I != Chain)
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
  // The source-signature memo is separate; check the complete machine effect
  // closure itself, including the deepest callee's X0 write at every caller.
  for (size_t I = 0; I <= Chain; ++I) {
    const va_t At = First + I * 32;
    ASSERT_TRUE(Result.CallMayWriteGPRs.count(At)) << I;
    EXPECT_TRUE(Result.CallMayWriteGPRs.at(At) & 1u) << I;
    ASSERT_TRUE(Result.CallBodyMayWriteGPRs.count(At)) << I;
    EXPECT_EQ(Result.CallBodyMayWriteGPRs.at(At), 0xFFFF0001u) << I;
  }
  const auto &Function = Result.MedFuncs.front();
  unsigned Checked = 0;
  for (const auto &Call : Function.CallInfos)
    if (Call.TargetAddr == Zero || Call.TargetAddr == One) {
      EXPECT_EQ(Call.Args.size(), Call.TargetAddr == Zero ? 0u : 1u);
      ++Checked;
    }
  EXPECT_EQ(Checked, 2u);
}

TEST(AAPCS64CallContract, EveryDirectCalleeGetsItsArgumentBound) {
  constexpr va_t First = 0x10000, Caller = 0x30000;
  constexpr size_t Count =
      limits::pipeline::kPipelineCallAbiMaxTransitiveCalleeReads + 9;
  auto BL = [](va_t At, va_t Target) {
    return 0x94000000u | (uint32_t((Target - At) / 4) & 0x03ffffffu);
  };
  for (auto Format :
       {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<int>(Format));
    BinaryImage Image;
    Image.Arch = Arch::AArch64;
    Image.Bits = Bitness::Bits64;
    Image.Format = Format;
    auto Text = codeSegment(".text", First, Caller - First + Count * 12 + 32);
    auto Add = [&](va_t At, std::vector<uint32_t> Words) {
      place(Text, At, Words);
      auto Symbol = Symbol::makeFunc(At);
      Symbol.Size = Words.size() * 4;
      Image.Symbols.push_back(std::move(Symbol));
    };
    std::vector<uint32_t> Words{PushFrame, SetFrame};
    for (size_t I = 0; I < Count; ++I) {
      // Every callee reads exactly x0. x1 is deliberately set at each call:
      // a caller-setup fallback must not turn that dead value into an arg.
      Add(First + I * 16, {0xF900001fu, Ret}); // str xzr, [x0]
      Words.push_back(0xD2801000u);            // mov x0, 0x80
      Words.push_back(0xD2800121u);            // mov x1, 9
      Words.push_back(BL(Caller + Words.size() * 4, First + I * 16));
    }
    Words.insert(Words.end(), {PopFrame, Ret});
    Add(Caller, std::move(Words));
    Image.Segments.push_back(std::move(Text));
    llvm::LLVMContext Context;
    const auto Result = run(Image, Context, {Caller});
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_TRUE(Result.OmittedCallees);
    std::set<va_t> Summarized;
    for (const auto &F : Result.OmittedCallees->Bodies)
      Summarized.insert(F->Entry);
    for (size_t I = 0; I < Count; ++I)
      EXPECT_TRUE(Summarized.contains(First + I * 16)) << I;
    ASSERT_EQ(Result.MedFuncs.size(), 1u);
    const auto &Calls = Result.MedFuncs.front().CallInfos;
    ASSERT_EQ(Calls.size(), Count);
    for (const auto &Call : Calls) {
      SCOPED_TRACE(Call.TargetAddr);
      ASSERT_EQ(Call.Args.size(), 1u);
      EXPECT_TRUE(Call.Args.front().isConst());
      EXPECT_EQ(Call.Args.front().ConstVal, 0x80u);
    }
  }
}

TEST(AAPCS64CallContract, LargeCalleeSignaturesDoNotNeedStructuredHighIR) {
  constexpr va_t Caller = Text, Callee = Text + 0x40;
  for (bool ManyBlocks : {false, true}) {
    SCOPED_TRACE(ManyBlocks);
    const size_t Count = ManyBlocks ? limits::kMaxStructurableMedBlocks + 1
                                    : limits::kMaxSSANodes + 1;
    for (auto Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO}) {
      SCOPED_TRACE(static_cast<int>(Format));
      BinaryImage Image;
      Image.Arch = Arch::AArch64;
      Image.Bits = Bitness::Bits64;
      Image.Format = Format;
      std::vector<uint32_t> Body;
      for (size_t I = 0; I < Count; ++I) {
        Body.push_back(0xF900001Fu); // str xzr, [x0]
        if (ManyBlocks)
          Body.push_back(0x14000001u); // b next instruction
      }
      Body.push_back(Ret);
      auto Code = codeSegment(".text", Caller, 0x40 + Body.size() * 4);
      place(Code, Callee, Body);
      place(Code, Caller,
            {PushFrame, SetFrame, 0xD2801000u, 0xD2800121u,
             0x94000000u | uint32_t((Callee - (Caller + 16)) / 4), PopFrame,
             Ret});
      Image.Segments.push_back(std::move(Code));
      for (va_t At : {Caller, Callee}) {
        auto Symbol = Symbol::makeFunc(At);
        Symbol.Size = At == Caller ? 28 : Body.size() * 4;
        Image.Symbols.push_back(std::move(Symbol));
      }
      PipelineAnalysisCache Cache;
      for (const auto &Entries :
           {std::set<va_t>{Caller}, std::set<va_t>{Caller, Callee},
            std::set<va_t>{Caller}}) {
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = Entries;
        Options.AnalysisCache = &Cache;
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        for (const auto &F : Result.MedFuncs)
          if (F.Entry == Caller) {
            ASSERT_EQ(F.CallInfos.size(), 1u);
            EXPECT_EQ(F.CallInfos.front().Args.size(), 1u);
          }
      }
    }
  }
}

TEST(AAPCS64CallContract, OpaqueVariadicVectorKeepsItsWholeRegister) {
  // The format is an incoming pointer, with no immutable format or scalar
  // consumption proof. Both halves of q0 belong to the physical call.
  for (bool Indirect : {false, true}) {
    SCOPED_TRACE(Indirect);
    std::vector<uint32_t> Words{PushFrame, SetFrame,
                                0x3DC00020}; // ldr q0, [x1]
    if (Indirect)
      Words.insert(Words.end(),
                   {adrp(16, Text + 12, Got), ldrX(17, 16, Got), blr(17)});
    else
      Words.push_back(0x94000000u | uint32_t((Plt - (Text + 12)) / 4));
    Words.insert(Words.end(), {PopFrame, Ret});
    const auto Image = makeImage({{Text, Words}, {Plt, pltEntry(Plt, Got)}},
                                 {{Text, "opaque_vector_call"}}, {"printf"});
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Text};
      Options.LiftMode = LLVM;
      Options.SourceProjection = LLVM;
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      ASSERT_EQ(Result.MedFuncs.front().CallInfos.size(), 1u);
      const auto &Call = Result.MedFuncs.front().CallInfos.front();
      ASSERT_EQ(Call.IntegerRegisterArgCount, 2);
      ASSERT_EQ(Call.FloatingRegisterArgCount, 1);
      ASSERT_EQ(Call.Args.size(), 3u);
      EXPECT_EQ(Call.VarArgFixedCount, 1);
      EXPECT_EQ(Call.Args.back().Size, 16);
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        bool Found = false;
        for (const auto &Function : *Result.LlvmModule)
          for (const auto &Block : Function)
            for (const auto &Instruction : Block)
              if (const auto *C = llvm::dyn_cast<llvm::CallBase>(&Instruction);
                  C && ((Indirect && C->isIndirectCall()) ||
                        (C->getCalledFunction() &&
                         C->getCalledFunction()->getName() == "printf"))) {
                Found = true;
                ASSERT_EQ(C->arg_size(), 3u);
                EXPECT_TRUE(C->getArgOperand(2)->getType()->isVectorTy());
                EXPECT_EQ(
                    C->getArgOperand(2)->getType()->getPrimitiveSizeInBits(),
                    128u);
              }
        EXPECT_TRUE(Found);
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions,
                                        nullptr, &Image));
      } else {
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, COptions));
      }
      EXPECT_NE(Source.find("vector_size(16)"), std::string::npos) << Source;
      EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
      // Run the emitted C's complete payload transport on the host. The LLVM
      // type assertion above separately checks the original target's bank.
      const std::string Oracle = R"oracle(
  #include <stdarg.h>
  typedef uint8_t payload __attribute__((vector_size(16)));
  int fixture_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    const void *expected = (const void *)(uintptr_t)va_arg(args, expected_carrier);
    payload value = va_arg(args, payload);
    va_end(args);
    return strcmp(format, "opaque") == 0 &&
                   memcmp(&value, expected, sizeof(value)) == 0 ? 123 : -1;
  }
  int main(void) {
    payload value = {0x80, 0x13, 0x42, 0xff, 0x7a, 0, 0xb1, 0x26,
                     0x31, 0x73, 0x29, 0xcd, 0xee, 0x54, 0x17, 0x99};
    return opaque_vector_call((uintptr_t)"opaque", &value) == 123 ? 0 : 1;
  }
  )oracle";
      for (const char *Optimization : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            "#define printf fixture_printf\n#include <string.h>\n" + Source +
                (LLVM ? "typedef uint64_t expected_carrier;\n"
                      : "typedef uintptr_t expected_carrier;\n") +
                Oracle,
            {Optimization});
    }
  }
}

TEST(AAPCS64CallContract, VariadicVectorBankProofDoesNotGuessAScalar) {
  MedCallInfo Call;
  Call.VarArgFixedCount = 1;
  Call.IntegerRegisterArgCount = 2;
  Call.FloatingRegisterArgCount = 1;
  Call.Args = {MedVar::makeConst(0, 8), MedVar::makeConst(0, 8),
               MedVar::makeUnspecified(16, Arch::AArch64),
               MedVar::makeUnspecified(16, Arch::AArch64)};
  for (Arch A : {Arch::AArch64, Arch::X64, Arch::X86, Arch::ARM})
    for (BinaryFormat F : {BinaryFormat::ELF, BinaryFormat::MachO,
                           BinaryFormat::COFF, BinaryFormat::Raw}) {
      SCOPED_TRACE(static_cast<int>(A));
      SCOPED_TRACE(static_cast<int>(F));
      const bool Supported = (A == Arch::AArch64 && F == BinaryFormat::ELF) ||
                             (A == Arch::X64 && (F == BinaryFormat::ELF ||
                                                 F == BinaryFormat::MachO));
      EXPECT_EQ(recoveredVariadicVectorArgument(Call, 2, A, F), Supported);
      for (size_t Other : {0u, 1u, 3u, 4u})
        EXPECT_FALSE(recoveredVariadicVectorArgument(Call, Other, A, F));
    }
  const auto Supported = [](const MedCallInfo &C) {
    return recoveredVariadicVectorArgument(C, 2, Arch::AArch64,
                                           BinaryFormat::ELF);
  };
  auto Altered = Call;
  Altered.VarArgFixedCount = -1;
  EXPECT_FALSE(Supported(Altered));
  Altered = Call;
  Altered.VarArgFixedCount = 3;
  EXPECT_FALSE(Supported(Altered));
  Altered = Call;
  Altered.IntegerRegisterArgCount = -1;
  EXPECT_FALSE(Supported(Altered));
  Altered = Call;
  Altered.FloatingRegisterArgCount = 3;
  EXPECT_FALSE(Supported(Altered));
  Altered = Call;
  Altered.ArgReadBytes = {0, 0, 8};
  EXPECT_FALSE(Supported(Altered));
  for (uint16_t Bytes : {4, 8, 32}) {
    Altered = Call;
    Altered.Args[2].Size = Bytes;
    EXPECT_FALSE(Supported(Altered));
  }
}

TEST(AAPCS64CallContract, BoundedVariadicImportsDoNotConsumeScratchArguments) {
  class Declared final : public NullDebugContext {
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name != "prctl")
        return std::nullopt;
      FunctionSym Function;
      Function.Name = Name.str();
      Function.ReturnType = NdType::makeInt(4);
      Function.PrototypeFromHeader = true;
      for (unsigned I = 0; I < 8; ++I)
        Function.Params.push_back({"", NdType::makeInt(8)});
      return Function;
    }
  } Debug;
  struct Case {
    const char *Name;
    size_t Count;
    bool Declared = false;
  };
  for (const auto &[Name, Count, HasDeclaration] :
       {Case{"prctl", 5}, Case{"syscall", 7}, Case{"open", 3},
        Case{"printf", 8}, Case{"prctl", 8, true}}) {
    SCOPED_TRACE(Name);
    SCOPED_TRACE(HasDeclaration);
    std::vector<uint32_t> Words{PushFrame, SetFrame};
    for (unsigned I = 0; I < 8; ++I)
      Words.push_back(0xD2800000u | ((I + 3) << 5) | I);
    const auto At = Text + Words.size() * 4;
    Words.push_back(0x94000000u | uint32_t((Plt - At) / 4));
    Words.insert(Words.end(), {PopFrame, Ret});
    const auto Image = makeImage({{Text, Words}, {Plt, pltEntry(Plt, Got)}},
                                 {{Text, "caller"}}, {Name});
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.OnlyFunctionEntries = {Text};
    const auto Result = Pipeline().run(Image, Context, Options,
                                       HasDeclaration ? &Debug : nullptr);
    ASSERT_TRUE(Result.Success) << Result.Error;
    ASSERT_EQ(Result.MedFuncs.size(), 1u);
    ASSERT_EQ(Result.MedFuncs.front().CallInfos.size(), 1u);
    const auto &Call = Result.MedFuncs.front().CallInfos.front();
    ASSERT_EQ(Call.Args.size(), Count);
    for (size_t I = 0; I < Count; ++I) {
      EXPECT_TRUE(Call.Args[I].isConst());
      EXPECT_EQ(Call.Args[I].ConstVal, I + 3);
    }
  }
}

TEST(AAPCS64CallContract, LinuxSyscallImportsReadTheNumberedArguments) {
  class Declared final : public NullDebugContext {
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name != "syscall")
        return std::nullopt;
      FunctionSym Function;
      Function.Name = Name.str();
      Function.ReturnType = NdType::makeInt(8);
      Function.PrototypeFromHeader = true;
      for (unsigned I = 0; I < 8; ++I)
        Function.Params.push_back({"", NdType::makeInt(8)});
      return Function;
    }
  } Debug;
  struct Case {
    uint16_t Number;
    size_t Count;
    bool FreeBSD = false;
    bool WithoutOS = false;
    bool Declared = false;
  };
  for (const auto &C :
       {Case{172, 1}, Case{49, 2}, Case{62, 4}, Case{56, 5}, Case{220, 6},
        Case{222, 7}, Case{65535, 7}, Case{172, 7, true},
        Case{172, 7, false, true}, Case{172, 8, false, false, true}}) {
    SCOPED_TRACE(C.Number);
    SCOPED_TRACE(C.FreeBSD);
    SCOPED_TRACE(C.WithoutOS);
    SCOPED_TRACE(C.Declared);
    std::vector<uint32_t> Code{PushFrame, SetFrame,
                               0xD2800000u | (uint32_t(C.Number) << 5)};
    for (unsigned I = 1; I < 8; ++I)
      Code.push_back(0xD2800000u | ((10 + I) << 5) | I);
    const va_t CallAt = Text + Code.size() * 4;
    Code.push_back(0x94000000u | uint32_t((Plt - CallAt) / 4));
    Code.insert(Code.end(), {PopFrame, Ret});
    auto Image = makeImage({{Text, Code}, {Plt, pltEntry(Plt, Got)}},
                           {{Text, "syscall_caller"}}, {"syscall"});
    if (!C.WithoutOS) {
      Image.ELFMetadata.emplace();
      Image.ELFMetadata->OSABI =
          C.FreeBSD ? llvm::ELF::ELFOSABI_FREEBSD : llvm::ELF::ELFOSABI_NONE;
    }
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Text};
      Options.LiftMode = LLVM;
      Options.SourceProjection = LLVM;
      auto Result = Pipeline().run(Image, Context, Options,
                                   C.Declared ? &Debug : nullptr);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      const auto &Calls = Result.MedFuncs.front().CallInfos;
      ASSERT_EQ(Calls.size(), 1u);
      ASSERT_EQ(Calls.front().Args.size(), C.Count);
      for (size_t I = 0; I < C.Count; ++I) {
        EXPECT_TRUE(Calls.front().Args[I].isConst());
        EXPECT_EQ(Calls.front().Args[I].ConstVal, I ? 10 + I : C.Number);
      }
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        const auto *Caller = Result.LlvmModule->getFunction("syscall_caller");
        ASSERT_NE(Caller, nullptr);
        const llvm::CallBase *Call = nullptr;
        for (const auto &Block : *Caller)
          for (const auto &I : Block)
            if (const auto *Each = llvm::dyn_cast<llvm::CallBase>(&I);
                Each && Each->getCalledFunction() &&
                Each->getCalledFunction()->getName() == "syscall")
              Call = Each;
        ASSERT_NE(Call, nullptr);
        EXPECT_EQ(Call->arg_size(), C.Count);
      }
    }
  }
}

TEST(AAPCS64CallContract, LinuxSyscallJoinsKeepEveryPossibleRead) {
  struct Case {
    uint16_t First, Second;
    size_t Count;
  };
  for (const auto &C :
       {Case{172, 56, 5}, Case{56, 222, 7}, Case{172, 65535, 7}}) {
    SCOPED_TRACE(C.First);
    SCOPED_TRACE(C.Second);
    std::vector<uint32_t> Code{PushFrame,
                               SetFrame,
                               0xB4000060, // cbz x0,second
                               0xD2800000u | (uint32_t(C.First) << 5),
                               0x14000002, // b joined
                               0xD2800000u | (uint32_t(C.Second) << 5)};
    for (unsigned I = 1; I < 8; ++I)
      Code.push_back(0xD2800000u | ((10 + I) << 5) | I);
    const va_t CallAt = Text + Code.size() * 4;
    Code.push_back(0x94000000u | uint32_t((Plt - CallAt) / 4));
    Code.insert(Code.end(), {PopFrame, Ret});
    auto Image = makeImage({{Text, Code}, {Plt, pltEntry(Plt, Got)}},
                           {{Text, "joined_syscall"}}, {"syscall"});
    Image.ELFMetadata.emplace();
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Text};
      Options.LiftMode = Options.SourceProjection = LLVM;
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      ASSERT_EQ(Result.MedFuncs.size(), 1u);
      const auto &Calls = Result.MedFuncs.front().CallInfos;
      ASSERT_EQ(Calls.size(), 1u);
      EXPECT_EQ(Calls.front().Args.size(), C.Count);
    }
  }
}

TEST(AAPCS64CallContract, LinuxSyscallUnusedClobbersDoNotTrap) {
  // One predecessor preserves x6; the other calls getpid, which may clobber
  // it. openat does not consume x6, but mmap does. A name or maximum-arity
  // bound alone cannot distinguish these observations.
  for (uint16_t Number : {56, 222}) {
    SCOPED_TRACE(Number);
    std::vector<uint32_t> Code{PushFrame,
                               SetFrame,
                               0xD28002E6, // mov x6,23
                               0xB4000040, // cbz x0,join
                               0x94000000u |
                                   uint32_t((Plt + 16 - (Text + 16)) / 4),
                               0xD2800000u | (uint32_t(Number) << 5)};
    for (unsigned I = 1; I < 6; ++I)
      Code.push_back(0xD2800000u | ((10 + I) << 5) | I);
    const va_t CallAt = Text + Code.size() * 4;
    Code.push_back(0x94000000u | uint32_t((Plt - CallAt) / 4));
    Code.insert(Code.end(), {PopFrame, Ret});
    auto Image =
        makeImage({{Text, Code},
                   {Plt, pltEntry(Plt, Got)},
                   {Plt + 16, pltEntry(Plt + 16, Got + 8)}},
                  {{Text, "conditional_syscall"}}, {"syscall", "getpid"});
    Image.ELFMetadata.emplace();
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      llvm::LLVMContext Context;
      PipelineOptions PipelineOpts;
      PipelineOpts.EmitDumpOutput = false;
      PipelineOpts.OnlyFunctionEntries = {Text};
      PipelineOpts.LiftMode = LLVM;
      PipelineOpts.SourceProjection = LLVM;
      auto Result = Pipeline().run(Image, Context, PipelineOpts);
      ASSERT_TRUE(Result.Success) << Result.Error;
      CEmitterOptions Options;
      Options.TheArch = Image.Arch;
      Options.Format = Image.Format;
      Options.Image = &Image;
      std::string Source;
      llvm::raw_string_ostream Out(Source);
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, Options));
      } else {
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Out, Options));
      }
      source_call_execution_test::checkSyntax(Source);
      const std::string Caller = body(Source, "conditional_syscall");
      ASSERT_FALSE(Caller.empty()) << Source;
      EXPECT_EQ(Caller.find("__builtin_trap()") != std::string::npos,
                Number == 222)
          << Caller;
      // Only the emitted scalar C executes on the host, with both imports
      // replaced by an observation fixture. No Linux syscall is executed.
      const std::string Program =
          std::string(R"(
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
static jmp_buf missing;
static void missing_value(void) { longjmp(missing, 1); }
#define __builtin_trap() missing_value()
static int32_t getpid(void) { return 17; }
static int64_t observed_syscall(int64_t number, ...) {
  va_list ap;
  va_start(ap, number);
  int64_t result = 0;
  for (int i = 0; i < (number == 56 ? 4 : 6); ++i)
    result += va_arg(ap, int64_t);
  va_end(ap);
  return result;
}
#define WORD(x) ((void)sizeof(char[sizeof(x) == sizeof(int64_t) ? 1 : -1]), (x))
)") +
          (Number == 56
               ? "#define syscall(n,a,b,c,d) "
                 "observed_syscall(n,WORD(a),WORD(b),WORD(c),WORD(d))\n"
               : "#define syscall(n,a,b,c,d,e,f) "
                 "observed_syscall(n,WORD(a),WORD(b),WORD(c),WORD(d),"
                 "WORD(e),WORD(f))\n") +
          Caller +
          (Number == 56 ? "\nint main(void) { return "
                          "conditional_syscall(0) != 50 || "
                          "conditional_syscall(1) != 50; }\n"
                        : "\nint main(void) { "
                          "if (conditional_syscall(0) != 88) return 1; "
                          "if (!setjmp(missing)) { conditional_syscall(1); "
                          "return 2; } return 0; }\n");
      for (const char *Optimization : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Program, {Optimization, "-fsanitize=undefined",
                      "-fno-sanitize-recover=undefined"});
    }
  }
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

TEST(AAPCS64CallContract, VoidFloatingCallsPreserveOutputPointerEffects) {
  class Header : public NullDebugContext {
  public:
    uint16_t Bytes = 4;
    std::optional<FunctionSym>
    resolvePrototype(llvm::StringRef Name) const override {
      if (Name != "fixture_void_fp")
        return std::nullopt;
      FunctionSym Function;
      Function.Name = Name.str();
      Function.ReturnType = NdType::makeVoid();
      const auto Float = NdType::makeFloat(Bytes);
      const auto Pointer = NdType::makePtr(Float);
      Function.Params = {
          {"value", Float}, {"first", Pointer}, {"second", Pointer}};
      Function.PrototypeFromHeader = true;
      return Function;
    }
    std::optional<FunctionSym> resolveFunction(va_t Address) const override {
      auto Function = Address == Got || Address == Plt
                          ? resolvePrototype("fixture_void_fp")
                          : std::nullopt;
      if (Function)
        Function->Addr = Address;
      return Function;
    }
  } Debug;
  for (bool Double : {false, true}) {
    Debug.Bytes = Double ? 8 : 4;
    // The callee writes two independent stack slots. Scratch x2 and s1/d1
    // are not arguments, and no return register carries the void call.
    const auto Image = makeImage(
        {{Text,
          {PushFrame, SetFrame, 0xD10083FFu,   // sub sp, sp, #32
           0x910003E0u, 0x910043E1u,           // x0 = sp; x1 = sp + 16
           Double ? 0x1E6F1000u : 0x1E2F1000u, // fmov d0/s0, #1.5
           0xD2800C62u,                        // mov x2, #99
           Double ? 0x1E601001u : 0x1E201001u, // fmov d1/s1, #2
           0x94000000u | uint32_t((Plt - (Text + 32)) / 4),
           Double ? 0xF94003E0u : 0xB94003E0u, // ldr x0/w0, [sp]
           Double ? 0xF9400BE1u : 0xB94013E1u, // ldr x1/w1, [sp, #16]
           Double ? 0xCA010000u : 0x4A010000u, // eor x0/w0, x0/w0, x1/w1
           0x910083FFu, PopFrame, Ret}},
         {Plt, pltEntry(Plt, Got)}},
        {{Text, "output_bits"}}, {"fixture_void_fp"});
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(Double);
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
        const auto *Callee = Result.LlvmModule->getFunction("fixture_void_fp");
        ASSERT_NE(Callee, nullptr);
        EXPECT_TRUE(Callee->getReturnType()->isVoidTy());
        ASSERT_EQ(Callee->arg_size(), 3u);
        EXPECT_TRUE(Double ? Callee->getArg(0)->getType()->isDoubleTy()
                           : Callee->getArg(0)->getType()->isFloatTy());
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
      Source += Double ? "\ntypedef double real;\n" : "\ntypedef float real;\n";
      // LLVM's opaque pointers spell void* in C; the physical pointer ABI
      // is identical and the provider still checks both independent writes.
      Source += LLVM ? "void fixture_void_fp(real value, void *first, void "
                       "*second) {\n"
                     : "void fixture_void_fp(real value, real *first, real "
                       "*second) {\n";
      Source += R"(
  *(real *)first = value + 1;
  *(real *)second = value - 1;
}
int main(void) {
)";
      Source +=
          Double
              ? "return (uint64_t)output_bits() != "
                "UINT64_C(0x7fe4000000000000);\n}\n"
              : "return (uint32_t)output_bits() != UINT32_C(0x7f200000);\n}\n";
      for (const char *Optimization : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(Source, {Optimization});
    }
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
