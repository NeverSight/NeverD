//===- X86SkinitCallTests.cpp - Physical address call transport ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A process-memory use must not rebase a physical integer at a caller
/// boundary. Record the callee's input instead of executing its privileged
/// body.
//===----------------------------------------------------------------------===//

#include "../x86_64/X86FPStateAccuracyFixture.h"

#include <regex>

namespace {
class X86SkinitCallSource : public X86FPStateFixture {};

TEST_F(X86SkinitCallSource, ImageValuedPhysicalArgumentRetainsItsOriginalBits) {
  if (!nativeX64() || hostFormat() != BinaryFormat::ELF)
    GTEST_SKIP() << "the executed C caller fixture requires Linux x64";
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Format = BinaryFormat::ELF;
  Image.Bits = Bitness::Bits64;
  Image.Base = Image.Entry = Entry;
  Segment Text;
  Text.VA = Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  // SUB RSP,8; MOV EDI,0x4000; CALL 0x1100. The callee never returns.
  Text.Data = {0x48, 0x83, 0xec, 8, 0xbf, 0, 0x40, 0,
               0,    0xe8, 0xf2, 0, 0,    0, 0x0f, 0x0b};
  Text.Data.resize(0x100, 0x90);
  // MOV EAX,EDI; MOV EDX,[RAX]; MOV [0x4040],EDX; SKINIT.
  const std::vector<uint8_t> Callee = {0x89, 0xf8, 0x8b, 0x10, 0x89,
                                       0x14, 0x25, 0x40, 0x40, 0,
                                       0,    0x0f, 0x01, 0xde};
  Text.Data.insert(Text.Data.end(), Callee.begin(), Callee.end());
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  Segment Data;
  Data.VA = 0x4000;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(128, 0x31);
  Data.Size = Data.FileSz = Data.Data.size();
  Image.Segments.push_back(std::move(Data));
  auto CallerSymbol = Symbol::makeFunc(Entry, 16);
  CallerSymbol.Name = "secure_caller";
  Image.Symbols.push_back(std::move(CallerSymbol));
  auto CalleeSymbol = Symbol::makeFunc(Entry + 0x100, Callee.size());
  CalleeSymbol.Name = "secure_launch";
  Image.Symbols.push_back(std::move(CalleeSymbol));
  unsigned Executions = 0;
  for (bool NoOpt : {false, true})
    for (bool High : {false, true}) {
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.LiftMode = !High;
      Options.SourceProjection = !High;
      Options.NoOpt = NoOpt;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Entry, Entry + 0x100};
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      CEmitterOptions Emit;
      Emit.TheArch = Arch::X64;
      Emit.Format = BinaryFormat::ELF;
      Emit.Image = &Image;
      Emit.PreserveLLVMFunctionTypes = true;
      std::string Source;
      llvm::raw_string_ostream OS(Source);
      ASSERT_TRUE(High ? HighCEmitter().emit(Result.HighFuncs, OS, Emit)
                       : LLVMCEmitter().emit(*Result.LlvmModule, OS, Emit,
                                             nullptr, &Image));
      retain(High ? "high-original.c" : "llvm-c-original.c", Source);
      // Keep the generated prototype and caller unchanged. Replace only the
      // callee body with a nonreturning observer of its actual ABI input.
      std::smatch Definition;
      ASSERT_TRUE(std::regex_search(
          Source, Definition, std::regex(R"(secure_launch\([^;{}]*\)\s*\{)")))
          << Source;
      const size_t Start = Definition.position() + Definition.length() - 1;
      size_t End = Start;
      unsigned Depth = 0;
      for (; End < Source.size(); ++End) {
        if (Source[End] == '{')
          ++Depth;
        else if (Source[End] == '}' && --Depth == 0)
          break;
      }
      ASSERT_LT(End, Source.size());
      Source.replace(Start, End - Start + 1,
                     "{ record_launch((uintptr_t)arg0); }");
      ASSERT_EQ(Source.find("__asm__ volatile(\"skinit"), std::string::npos)
          << "the recorder must remove every launch before host execution";
      ASSERT_EQ(Source.find("__builtin_ia32_skinit("), std::string::npos)
          << "the recorder must remove every launch before host execution";
      Source = "#include <stdint.h>\n#include <stdlib.h>\n"
               "static _Noreturn void record_launch(uintptr_t physical) {\n"
               " _Exit(physical == UINT32_C(0x4000) ? 0 : 93);\n}\n" +
               Source + "\nint main(void) { secure_caller(); return 94; }\n";
      const auto Input = file("physical-caller.c");
      write(Input, Source);
      retain(High ? "high-recorder.c" : "llvm-c-recorder.c", Source);
      for (const auto *Opt : {"-O0", "-O2"}) {
        const auto Executable = file(std::string("physical-caller") + Opt);
        const auto Built =
            command(Compiler, {"-std=gnu23", "-fno-strict-aliasing", "-fno-pie",
                               "-no-pie", "-fsanitize=address,undefined", Opt,
                               Input, "-o", Executable});
        ASSERT_EQ(Built.Status, 0) << Built.Error << Source;
        const auto Actual = command(Executable, {});
        EXPECT_EQ(Actual.Status, 0) << Actual.Error << Source;
        ++Executions;
      }
      ++ProbeNumber;
    }
  EXPECT_EQ(Executions, 8u);
  RecordProperty("sanitized_caller_argument_observations", Executions);
}
} // namespace
