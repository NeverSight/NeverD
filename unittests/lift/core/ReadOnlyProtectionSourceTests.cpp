//===- ReadOnlyProtectionSourceTests.cpp - Writable image values ---------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Conventional constant-section names cannot freeze a writable global.
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <string>

namespace {
using namespace neverd;

BinaryImage writableImage(Arch Architecture, BinaryFormat Format,
                          const char *Name) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Image.Format = Format;
  Image.Base = Image.Entry = 0x1000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  if (Architecture == Arch::X64) {
    Text.Data = {0x8b, 0x05, 0xfa, 0x0f, 0, 0, 0xc3};
    Text.Data.resize(0x10, 0xcc);
    const uint8_t Source = Format == BinaryFormat::COFF ? 0x0d : 0x3d;
    Text.Data.insert(Text.Data.end(),
                     {0x89, Source, 0xea, 0x0f, 0, 0, 0x31, 0xc0, 0xc3});
  } else {
    Text.Data = {0x01, 0x00, 0x00, 0xb0,  // adrp x1,0x2000
                 0x20, 0x00, 0x40, 0xb9,  // ldr w0,[x1]
                 0xc0, 0x03, 0x5f, 0xd6}; // ret
    Text.Data.resize(0x10, 0);
    Text.Data.insert(Text.Data.end(), {0x01, 0x00, 0x00, 0xb0, // adrp x1,0x2000
                                       0x20, 0x00, 0x00, 0xb9, // str w0,[x1]
                                       0x00, 0x00, 0x80, 0x52, // mov w0,0
                                       0xc0, 0x03, 0x5f, 0xd6});
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(Text);
  Section Code;
  Code.VA = Text.VA;
  Code.Size = Code.FileSz = Text.Size;
  Code.Flags = Text.Flags;
  Code.Type = llvm::MachO::S_ATTR_PURE_INSTRUCTIONS;
  Image.Sections.push_back(Code);

  Segment Data;
  Data.Name = Name;
  Data.VA = 0x2000;
  Data.Size = Data.FileSz = 4;
  Data.FileOff = Text.FileSz;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data = {17, 0, 0, 0};
  Image.Segments.push_back(Data);
  Section Value;
  Value.Name = Value.SegmentName = Name;
  Value.VA = Data.VA;
  Value.Size = Value.FileSz = Data.Size;
  Value.FileOff = Data.FileOff;
  Value.Flags = Data.Flags;
  Image.Sections.push_back(Value);
  auto Reader = Symbol::makeFunc(0x1000, Architecture == Arch::X64 ? 7 : 12);
  Reader.Name = "read_value";
  Image.Symbols.push_back(Reader);
  auto Writer = Symbol::makeFunc(0x1010, Architecture == Arch::X64 ? 9 : 16);
  Writer.Name = "write_value";
  Image.Symbols.push_back(Writer);
  Symbol Global;
  Global.Name = "shared_value";
  Global.Addr = Data.VA;
  Global.Size = Data.Size;
  Image.Symbols.push_back(Global);
  return Image;
}

TEST(ReadOnlyProtectionSource, WritableValuesSurviveBothSourceRoutes) {
  for (Arch Architecture : {Arch::X64, Arch::AArch64})
    for (BinaryFormat Format :
         {BinaryFormat::ELF, BinaryFormat::COFF, BinaryFormat::MachO})
      for (const char *Name : {".data.rel.ro", "__DATA_CONST"})
        for (bool LLVM : {false, true})
          for (bool NoOpt : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << unsigned(Architecture) << ':' << unsigned(Format)
                         << ':' << Name << ':' << LLVM << ':' << NoOpt);
            auto Image = writableImage(Architecture, Format, Name);
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.LiftMode = LLVM;
            Options.NoOpt = NoOpt;
            Options.OnlyFunctionEntries = {0x1000, 0x1010};
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_TRUE(Result.Success) << Result.Error;
            std::string Source = "#include <stdint.h>\n#include <string.h>\n";
            llvm::raw_string_ostream Output(Source);
            CEmitterOptions COptions;
            COptions.EmitIncludes = false;
            COptions.TheArch = Architecture;
            COptions.Format = Format;
            COptions.Image = &Image;
            if (LLVM) {
              ASSERT_TRUE(Result.LlvmModule);
              ASSERT_FALSE(
                  llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
              auto *Reader = Result.LlvmModule->getFunction("read_value");
              ASSERT_NE(Reader, nullptr);
              bool ReadsMutableGlobal = false;
              for (const auto &Block : *Reader)
                for (const auto &Instruction : Block)
                  if (const auto *Load =
                          llvm::dyn_cast<llvm::LoadInst>(&Instruction))
                    if (const auto *Global =
                            llvm::dyn_cast<llvm::GlobalVariable>(
                                llvm::getUnderlyingObject(
                                    Load->getPointerOperand()))) {
                      EXPECT_FALSE(Global->isConstant());
                      ReadsMutableGlobal = true;
                    }
              EXPECT_TRUE(ReadsMutableGlobal);
              ASSERT_TRUE(
                  LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
            } else
              ASSERT_TRUE(
                  HighCEmitter().emit(Result.HighFuncs, Output, COptions));
            EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos)
                << Source;
            Source += R"c(
int main(void) {
  uint32_t values[] = {0, 17, 43, 0xffffffffu};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
    write_value(values[i]);
    if ((uint32_t)read_value() != values[i]) return 1;
  }
  return 0;
}
)c";
            for (const auto *Optimization : {"-O0", "-O2"})
              source_call_execution_test::compileAndRun(
                  Source, {Optimization, "-fsanitize=undefined",
                           "-fno-sanitize-recover=all"});
          }
}
} // namespace
