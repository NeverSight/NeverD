//===- AArch64SystemCallSetupTests.cpp - System code around AArch64 calls -===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Check call argument setup around AArch64 system-register operations and
/// barriers, including hypervisor calls and callee scratch registers.
//===----------------------------------------------------------------------===//

#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace neverd;

constexpr va_t Text = 0x10000;
constexpr va_t RoData = 0x11000;
constexpr uint32_t PushFrame = 0xA9BF7BFDu; // stp x29, x30, [sp, #-16]!
constexpr uint32_t SetFrame = 0x910003FDu;  // mov x29, sp
constexpr uint32_t PopFrame = 0xA8C17BFDu;  // ldp x29, x30, [sp], #16
constexpr uint32_t Ret = 0xD65F03C0u;

/// `mov xRd, #Imm`
uint32_t movX(unsigned Rd, uint16_t Imm) {
  return 0xD2800000u | uint32_t(Imm) << 5 | Rd;
}
uint32_t bl(va_t At, va_t Target) {
  return 0x94000000u | (static_cast<uint32_t>((Target - At) / 4) & 0x3FFFFFF);
}

/// An AArch64 ELF image of \p Functions, each placed at its address, and of
/// the read-only \p Strings.
BinaryImage
makeImage(const std::vector<std::pair<va_t, std::vector<uint32_t>>> &Code,
          const std::vector<std::pair<va_t, const char *>> &Functions,
          const std::vector<std::pair<va_t, std::string>> &Strings = {}) {
  BinaryImage Img;
  Img.Arch = Arch::AArch64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Text;
  Seg.Size = Seg.FileSz = RoData - Text;
  Seg.Data.assign(Seg.Size, 0);
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  for (const auto &[At, Words] : Code)
    for (size_t I = 0; I < Words.size(); ++I)
      for (unsigned B = 0; B < 4; ++B)
        Seg.Data[At - Text + I * 4 + B] =
            static_cast<uint8_t>(Words[I] >> (8 * B));
  Img.Segments.push_back(std::move(Seg));
  if (!Strings.empty()) {
    Segment Data;
    Data.Name = ".rodata";
    Data.VA = RoData;
    Data.Size = Data.FileSz = 0x100;
    Data.Data.assign(Data.Size, 0);
    Data.Flags = SegmentFlags::Readable;
    for (const auto &[At, Chars] : Strings)
      std::copy(Chars.begin(), Chars.end(), Data.Data.begin() + (At - RoData));
    Img.Segments.push_back(std::move(Data));
  }
  for (const auto &[Entry, Name] : Functions) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    Img.Symbols.push_back(std::move(Function));
  }
  return Img;
}

/// HighC for the functions at \p Entries of \p Img.
std::string highC(const BinaryImage &Img, std::set<va_t> Entries) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = std::move(Entries);
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
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

/// The arguments of the call of \p Name in a function body of \p Source.
std::vector<std::string> callArguments(const std::string &Source,
                                       const std::string &Name) {
  for (size_t At = Source.find(Name + "("); At != std::string::npos;
       At = Source.find(Name + "(", At + 1)) {
    const size_t Begin = Source.rfind('\n', At) + 1;
    if (Begin == At || (Source[Begin] != ' ' && Source[Begin] != '\t'))
      continue;
    std::vector<std::string> Arguments;
    std::string Current;
    int Depth = 0;
    for (size_t I = At + Name.size() + 1; I < Source.size(); ++I) {
      const char C = Source[I];
      if (Depth == 0 && (C == ',' || C == ')')) {
        if (!Current.empty() || C == ',')
          Arguments.push_back(Current);
        Current.clear();
        if (C == ')')
          return Arguments;
        if (I + 1 < Source.size() && Source[I + 1] == ' ')
          ++I;
        continue;
      }
      Depth += C == '(' ? 1 : C == ')' ? -1 : 0;
      Current += C;
    }
  }
  return {};
}

/// The definition line of function \p Name.
std::string signatureOf(const std::string &Source, const std::string &Name) {
  for (size_t At = Source.find(" " + Name + "("); At != std::string::npos;
       At = Source.find(" " + Name + "(", At + 1)) {
    const size_t Begin = Source.rfind('\n', At) + 1;
    const std::string Line =
        Source.substr(Begin, Source.find('\n', At) - Begin);
    if (Line.find('{') != std::string::npos)
      return Line;
  }
  return {};
}

TEST(AArch64SystemCallSetup, SystemInstructionsKeepRegisterArguments) {
  constexpr va_t Callee = Text + 0x100;
  // `x0 + x1 + x2`: three arguments.
  const std::vector<uint32_t> Sum{0x8B010000u, 0x8B020000u, Ret};
  const struct {
    const char *Name;
    uint32_t Word;
    bool SetsThird;
  } Cases[] = {
      {"mrs", 0xD53BD042u, true},  // mrs x2, tpidr_el0
      {"msr", 0xD51BD040u, false}, // msr tpidr_el0, x0
      {"isb", 0xD5033FDFu, false},
      {"dmb", 0xD5033BBFu, false}, // dmb ish
  };
  for (const auto &Case : Cases) {
    SCOPED_TRACE(Case.Name);
    std::vector<uint32_t> Caller{PushFrame, SetFrame, 0x52800201u /* w1=16 */,
                                 movX(0, 5)};
    if (!Case.SetsThird)
      Caller.push_back(movX(2, 7));
    Caller.push_back(Case.Word);
    Caller.push_back(bl(Text + 4 * Caller.size(), Callee));
    Caller.insert(Caller.end(), {PopFrame, Ret});
    const auto Image = makeImage({{Text, Caller}, {Callee, Sum}},
                                 {{Text, "caller"}, {Callee, "callee"}});
    const std::string Source = highC(Image, {Text, Callee});
    const auto Arguments = callArguments(Source, "callee");
    EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
    ASSERT_EQ(Arguments.size(), 3u) << Source;
    EXPECT_EQ(Arguments[0], "5") << Source;
    EXPECT_EQ(Arguments[1], "0x10") << Source;
    if (!Case.SetsThird)
      EXPECT_EQ(Arguments[2], "7") << Source;
  }
}

TEST(AArch64SystemCallSetup, VariadicCallTakesTheArgumentsItsFormatNames) {
  constexpr va_t Logger = Text + 0x100;
  constexpr va_t Sink = Text + 0x200;
  constexpr va_t Plain = RoData + 0x10;
  constexpr va_t OneInt = RoData + 0x20;
  // `logger(const char *fmt, ...)` saves x1-x7 for its va_list alone, as
  // code built without the floating-point registers does, and hands the
  // format and the va_list to `sink`.
  std::vector<uint32_t> LoggerCode{
      0xD101C3FFu, // sub sp, sp, #0x70
      0xA9067BFDu, // stp x29, x30, [sp, #0x60]
      0x910183FDu, // add x29, sp, #0x60
      0xA9000BE1u, // stp x1, x2, [sp]
      0xA90113E3u, // stp x3, x4, [sp, #16]
      0xA9021BE5u, // stp x5, x6, [sp, #32]
      0xF9001BE7u, // str x7, [sp, #48]
      0x128006E9u, // mov w9, #-56
      0x910043A8u, // add x8, x29, #16
      0x9100E3EAu, // add x10, sp, #56
      0xA903ABE8u, // stp x8, x10, [sp, #56]
      0xF9002BE9u, // str x9, [sp, #80]
      0x9100E3E1u, // add x1, sp, #56
  };
  LoggerCode.push_back(bl(Logger + 4 * LoggerCode.size(), Sink));
  LoggerCode.insert(LoggerCode.end(),
                    {0xA9467BFDu /* ldp x29, x30, [sp, #0x60] */,
                     0x9101C3FFu /* add sp, sp, #0x70 */, Ret});
  const std::vector<uint32_t> SinkCode{0xF9400022u /* ldr x2, [x1] */,
                                       0x8B020000u, Ret};
  for (const auto &[Format, Named] :
       {std::pair{Plain, size_t(0)}, std::pair{OneInt, size_t(1)}}) {
    SCOPED_TRACE(Format);
    // x1 and x2 are set before the branch, the format only on the way to
    // the call.
    const std::vector<uint32_t> Caller{
        PushFrame,
        SetFrame,
        movX(1, 5),
        movX(2, 7),
        0xB4000080u, // cbz x0, .+16
        0xB0000000u, // adrp x0, RoData
        0x91000000u | static_cast<uint32_t>(Format - RoData) << 10,
        bl(Text + 28, Logger),
        PopFrame,
        Ret};
    const auto Image =
        makeImage({{Text, Caller}, {Logger, LoggerCode}, {Sink, SinkCode}},
                  {{Text, "caller"}, {Logger, "logger"}, {Sink, "sink"}},
                  {{Plain, std::string("hello\n") + '\0'},
                   {OneInt, std::string("%d\n") + '\0'}});
    const std::string Source = highC(Image, {Text, Logger, Sink});
    const auto Arguments = callArguments(Source, "logger");
    // The format, what it names, and nothing else the registers hold: a
    // prototype that declares every saved register gets zero in the rest.
    ASSERT_GT(Arguments.size(), Named) << Source;
    EXPECT_NE(Arguments[0].find("\\n"), std::string::npos) << Source;
    if (Named)
      EXPECT_EQ(Arguments[1], "5") << Source;
    for (size_t I = 1 + Named; I < Arguments.size(); ++I)
      EXPECT_EQ(Arguments[I], "0") << Source;
    EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
  }
}

TEST(AArch64SystemCallSetup, HypervisorCallTakesTheArgumentsItsCallerSets) {
  constexpr va_t Wrapper = Text + 0x100;
  constexpr uint32_t Hvc = 0xD4000002u; // hvc #0
  // `__hyp_reset_vectors`: the function identifier alone.
  const auto Image =
      makeImage({{Text, {movX(0, 2), Hvc, Ret}}, {Wrapper, {Hvc, Ret}}},
                {{Text, "reset_vectors"}, {Wrapper, "smccc_call"}});
  const std::string Source = highC(Image, {Text, Wrapper});
  EXPECT_NE(signatureOf(Source, "reset_vectors").find("(void)"),
            std::string::npos)
      << Source;
  // A wrapper passes on its own identifier and every argument register.
  EXPECT_NE(signatureOf(Source, "smccc_call").find("arg7"), std::string::npos)
      << Source;
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
}

TEST(AArch64SystemCallSetup, ScratchPastACalleesArgumentsMakesNoParameter) {
  constexpr va_t Pair = Text + 0x100;
  constexpr va_t None = Text + 0x200;
  // `x0 + x1`, and nothing.
  const std::vector<uint32_t> Add{0x8B010000u, Ret};
  for (const va_t Callee : {Pair, None}) {
    SCOPED_TRACE(Callee);
    // x3 is scratch the block sets beside the call; x2 lies below it.
    const std::vector<uint32_t> Caller{
        PushFrame, SetFrame, movX(1, 4), movX(3, 9), bl(Text + 16, Callee),
        PopFrame,  Ret};
    const auto Image =
        makeImage({{Text, Caller}, {Pair, Add}, {None, {Ret}}},
                  {{Text, "caller"}, {Pair, "pair"}, {None, "none"}});
    const std::string Source = highC(Image, {Text, Pair, None});
    const std::string Signature = signatureOf(Source, "caller");
    EXPECT_EQ(Signature.find("arg2"), std::string::npos) << Source;
    if (Callee == None)
      EXPECT_NE(Signature.find("(void)"), std::string::npos) << Source;
    EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
  }
}

} // namespace
