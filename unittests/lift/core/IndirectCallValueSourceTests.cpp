//===- IndirectCallValueSourceTests.cpp - Callable values in source C ----===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// A register call retains its value after an address crosses a direct edge.
//===----------------------------------------------------------------------===//

#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/TargetRegInfo.h"
#include "neverd/loader/BinaryImage.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

BinaryImage callImage(Arch Architecture, BinaryFormat Format) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Bits = Bitness::Bits64;
  Image.Format = Format;
  Image.Base = Image.Entry = 0x1000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  if (Architecture == Arch::X64) {
    Text.Data = {0x48, 0x83, 0xec, 0x28,          // sub rsp,40
                 0x48, 0x8d, 0x05, 0xf5, 0, 0, 0, // lea rax,0x1100
                 0xeb, 0x13};                     // jmp 0x1020
    Text.Data.resize(0x20, 0xcc);
    Text.Data.insert(Text.Data.end(), {0xff, 0xd0, // call rax
                                       0x48, 0x83, 0xc4, 0x28, 0xc3});
    Text.Data.resize(0x100, 0xcc);
    Text.Data.insert(Text.Data.end(), {0xb8, 77, 0, 0, 0, 0xc3});
  } else {
    Text.Data = {0xfd, 0x7b, 0xbf, 0xa9,  // stp x29,x30,[sp,#-16]!
                 0xfd, 0x03, 0x00, 0x91,  // mov x29,sp
                 0xd0, 0x07, 0x00, 0x10,  // adr x16,0x1100
                 0x05, 0x00, 0x00, 0x14}; // b 0x1020
    Text.Data.resize(0x20, 0);
    Text.Data.insert(Text.Data.end(),
                     {0x00, 0x02, 0x3f, 0xd6, // blr x16
                      0xfd, 0x7b, 0xc1, 0xa8, 0xc0, 0x03, 0x5f, 0xd6});
    Text.Data.resize(0x100, 0);
    Text.Data.insert(Text.Data.end(), {0xa0, 0x09, 0x80, 0x52, // mov w0,77
                                       0xc0, 0x03, 0x5f, 0xd6});
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Segments.push_back(std::move(Text));
  auto Caller = Symbol::makeFunc(0x1000);
  Caller.Name = "call_value_probe";
  Caller.Size = Architecture == Arch::X64 ? 0x27 : 0x2c;
  Image.Symbols.push_back(std::move(Caller));
  auto Callee = Symbol::makeFunc(0x1100);
  Callee.Name = "value_callee";
  Callee.Size = Architecture == Arch::X64 ? 6 : 8;
  Image.Symbols.push_back(std::move(Callee));
  return Image;
}

TEST(IndirectCallValueSource, DirectEdgeDoesNotTurnCodeIntoPointerStorage) {
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (unsigned Route : {0U, 1U, 2U})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << unsigned(Architecture) << ':' << unsigned(Format)
                       << ':' << Route << ':' << NoOpt);
          const bool LLVM = Route != 0;
          const bool SourceView = Route == 2;
          auto Image = callImage(Architecture, Format);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.LiftMode = LLVM;
          Options.SourceProjection = SourceView;
          Options.NoOpt = NoOpt;
          Options.OnlyFunctionEntries = {0x1000, 0x1100};
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
            ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            // The fixture takes a code address but reads no image storage.
            // This remains true in the original-image source view.
            for (const auto &Function : *Result.LlvmModule)
              for (const auto &Block : Function)
                for (const auto &Instruction : Block)
                  if (const auto *Load =
                          llvm::dyn_cast<llvm::LoadInst>(&Instruction))
                    EXPECT_FALSE(llvm::isa<llvm::GlobalVariable>(
                        llvm::getUnderlyingObject(Load->getPointerOperand())));
            ASSERT_TRUE(
                LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
          } else
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Output, COptions));
          EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
          if (SourceView) {
            source_call_execution_test::checkSyntax(Source, {});
            continue;
          }
          for (const auto *Optimization : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(
                Source +
                    "\nint main(void) { return call_value_probe() != 77; }\n",
                {Optimization, "-fsanitize=undefined",
                 "-fno-sanitize-recover=all"});
        }
}

BinaryImage forwardingImage(Arch Architecture, BinaryFormat Format, bool Edge) {
  auto Image = callImage(Architecture, Format);
  auto &Bytes = Image.Segments.front().Data;
  if (!Edge) {
    const size_t Start = Architecture == Arch::X64 ? 11 : 12;
    const size_t Length = Architecture == Arch::X64 ? 7 : 12;
    std::copy_n(Bytes.begin() + 0x20, Length, Bytes.begin() + Start);
    Image.Symbols.front().Size = Start + Length;
  }
  if (Architecture == Arch::X64) {
    const uint8_t Operand = Format == BinaryFormat::COFF ? 0x41 : 0x47;
    const uint8_t Callee[] = {0x48, 0x8d, Operand, 1, 0xc3};
    std::copy(std::begin(Callee), std::end(Callee), Bytes.begin() + 0x100);
    Image.Symbols.back().Size = sizeof(Callee);
  } else {
    const uint8_t Callee[] = {0x00, 0x04, 0x00, 0x91, 0xc0, 0x03, 0x5f, 0xd6};
    std::copy(std::begin(Callee), std::end(Callee), Bytes.begin() + 0x100);
  }
  return Image;
}

void checkForwardedInput(bool Edge) {
  for (auto Architecture : {Arch::X64, Arch::AArch64})
    for (auto Format :
         {BinaryFormat::COFF, BinaryFormat::ELF, BinaryFormat::MachO})
      for (bool Selected : {true, false})
        for (bool LLVM : {false, true})
          for (bool NoOpt : {false, true}) {
            SCOPED_TRACE(::testing::Message()
                         << unsigned(Architecture) << ':' << unsigned(Format)
                         << ':' << Selected << ':' << LLVM << ':' << NoOpt);
            auto Image = forwardingImage(Architecture, Format, Edge);
            llvm::LLVMContext Context;
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.LiftMode = LLVM;
            Options.NoOpt = NoOpt;
            Options.OnlyFunctionEntries = Selected
                                              ? std::set<va_t>{0x1000}
                                              : std::set<va_t>{0x1000, 0x1100};
            auto Result = Pipeline().run(Image, Context, Options);
            ASSERT_TRUE(Result.Success) << Result.Error;
            std::string Source = "#include <stdint.h>\n#include <string.h>\n";
            std::string Provider = "value_callee";
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
              const auto *Caller =
                  Result.LlvmModule->getFunction("call_value_probe");
              ASSERT_TRUE(Caller);
              ASSERT_EQ(Caller->arg_size(), 1u);
              if (Selected) {
                unsigned Calls = 0;
                for (const auto &Block : *Caller)
                  for (const auto &Instruction : Block)
                    if (const auto *Call =
                            llvm::dyn_cast<llvm::CallBase>(&Instruction))
                      if (const auto *Target = Call->getCalledFunction();
                          Target && !Target->isIntrinsic()) {
                        ASSERT_EQ(Target->arg_size(), 1u);
                        ASSERT_TRUE(Target->isDeclaration());
                        Provider = Target->getName().str();
                        ++Calls;
                      }
                ASSERT_EQ(Calls, 1u);
              }
              ASSERT_TRUE(
                  LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
            } else {
              ASSERT_FALSE(Result.MedFuncs.empty());
              ASSERT_EQ(Result.MedFuncs.front().Params.size(), 1u);
              ASSERT_TRUE(
                  HighCEmitter().emit(Result.HighFuncs, Output, COptions));
            }
            ASSERT_EQ(Source.find("__builtin_trap"), std::string::npos)
                << Source;
            if (Selected)
              Source += "\n__typeof__(" + Provider + "(0)) " + Provider +
                        "(__typeof__(" + Provider +
                        "(0)) x) { return (uint64_t)x + 1; }\n";
            Source += R"c(
int main(void) {
  uint64_t values[] = {0, 17, UINT64_C(0x100000002), UINT64_MAX};
  for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i)
    if ((uint64_t)call_value_probe(values[i]) != values[i] + 1) return 1;
  return 0;
}
)c";
            for (const auto *Optimization : {"-O0", "-O2"})
              source_call_execution_test::compileAndRun(
                  Source, {Optimization, "-fsanitize=undefined",
                           "-fno-sanitize-recover=all"});
          }
}
TEST(IndirectCallValueSource, ExactTargetKeepsForwardedInputBeforeSSA) {
  checkForwardedInput(false);
}

TEST(IndirectCallValueSource, DirectEdgeKeepsForwardedInputBeforeSSA) {
  checkForwardedInput(true);
}
BinaryImage dynamicForwardingImage(Arch Architecture, BinaryFormat Format,
                                   bool Tail) {
  BinaryImage Image;
  Image.Arch = Architecture;
  Image.Format = Format;
  Image.Bits = getTargetRegInfo(Architecture).PointerSize == 4
                   ? Bitness::Bits32
                   : Bitness::Bits64;
  Image.Entry = Image.Base = 0x1000;
  Segment Text;
  Text.VA = Image.Entry;
  Text.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  if (Architecture == Arch::X64) {
    Text.Data = {
        0x48, 0x83, 0xec, 0x28,
        0x48, 0x8b, 0x05, 0xf5,
        2,    0,    0,    uint8_t(Format == BinaryFormat::COFF ? 0xba : 0xbe),
        37,   0,    0,    0};
    if (Tail)
      Text.Data.insert(Text.Data.end(), {0x48, 0x83, 0xc4, 0x28, 0xff, 0xe0});
    else
      Text.Data.insert(Text.Data.end(),
                       {0xff, 0xd0, 0x48, 0x83, 0xc4, 0x28, 0xc3});
  } else if (Architecture == Arch::X86) {
    Text.Data = {0x6a, 37, 0xff, 0x74, 0x24, 8, // push 37; push [esp+8]
                 0xa1, 0,  0x13, 0,    0,    0xff, 0xd0, 0x83, 0xc4, 8, 0xc3};
  } else {
    std::vector<uint32_t> Words;
    if (Architecture == Arch::AArch64) {
      Words = {0xa9bf7bfd, 0x910003fd, 0xd2826010, 0xf9400210, 0xd28004a1};
      if (Tail)
        Words.insert(Words.end(), {0xa8c17bfd, 0xd61f0200});
      else
        Words.insert(Words.end(), {0xd63f0200, 0xa8c17bfd, 0xd65f03c0});
    } else {
      const bool Wide = isMIPS64(Architecture);
      Words = {Wide ? 0x67bdffe0u : 0x27bdffe0u,
               Wide ? 0xffbf0018u : 0xafbf001cu,
               0x24191300,
               Wide && !isMIPSN32(Architecture) ? 0xdf390000u : 0x8f390000u,
               0x0320f809,
               0x24050025,
               Wide ? 0xdfbf0018u : 0x8fbf001cu,
               0x03e00008,
               Wide ? 0x67bd0020u : 0x27bd0020u};
      Image.MIPS.emplace();
      Image.MIPS->HasDSP = true;
      Image.MIPS->ABI = isMIPSN32(Architecture) ? MIPSABI::N32
                        : Wide                  ? MIPSABI::N64
                                                : MIPSABI::O32;
    }
    Text.Data.resize(4 * Words.size());
    for (size_t I = 0; I < Words.size(); ++I)
      llvm::support::endian::write32(Text.Data.data() + 4 * I, Words[I],
                                     archLittleEndian(Architecture)
                                         ? llvm::endianness::little
                                         : llvm::endianness::big);
  }
  Text.Size = Text.FileSz = Text.Data.size();
  Image.Symbols.push_back({"dynamic_probe", Image.Entry, Text.Size, true});
  Image.Segments.push_back(std::move(Text));
  Segment Data;
  Data.VA = 0x1300;
  Data.Size = Data.FileSz = 8;
  Data.Data.resize(8);
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Image.Segments.push_back(std::move(Data));
  return Image;
}

TEST(IndirectCallValueSource,
     DynamicPointerForwardingAgreesAcrossSourceRoutes) {
  for (Arch Architecture :
       {Arch::X86, Arch::X64, Arch::AArch64, Arch::MIPS32, Arch::MIPS32EL,
        Arch::MIPS64N32, Arch::MIPS64N32EL, Arch::MIPS64, Arch::MIPS64EL})
    for (bool Tail : {false, true}) {
      if (Tail && Architecture != Arch::X64 && Architecture != Arch::AArch64)
        continue;
      for (bool LLVM : {false, true})
        for (bool NoOpt : {false, true}) {
          SCOPED_TRACE(::testing::Message()
                       << unsigned(Architecture) << ':' << Tail << ':' << LLVM
                       << ':' << NoOpt);
          auto Image =
              dynamicForwardingImage(Architecture, BinaryFormat::ELF, Tail);
          llvm::LLVMContext Context;
          PipelineOptions Options;
          Options.EmitDumpOutput = false;
          Options.OnlyFunctionEntries = {Image.Entry};
          Options.LiftMode = LLVM;
          Options.NoOpt = NoOpt;
          auto Result = Pipeline().run(Image, Context, Options);
          ASSERT_TRUE(Result.Success) << Result.Error;
          ASSERT_EQ(Result.MedFuncs.size(), 1u);
          const auto &Function = Result.MedFuncs.front();
          ASSERT_EQ(Function.Params.size(), 1u);
          ASSERT_EQ(Function.CallInfos.size(), 1u);
          const auto &Call = Function.CallInfos.front();
          EXPECT_TRUE(Call.IsIndirect);
          ASSERT_EQ(Call.Args.size(), 2u);
          if (Call.Args.back().isConst())
            EXPECT_EQ(Call.Args.back().ConstVal, 37u);
          EXPECT_FALSE(Function.Blocks.front().Ops.empty());
          std::string Source;
          llvm::raw_string_ostream Output(Source);
          CEmitterOptions COptions;
          COptions.TheArch = Architecture;
          COptions.Format = Image.Format;
          COptions.Image = &Image;
          if (LLVM) {
            ASSERT_TRUE(Result.LlvmModule);
            ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
            const auto *Caller =
                Result.LlvmModule->getFunction("dynamic_probe");
            ASSERT_TRUE(Caller);
            EXPECT_EQ(Caller->arg_size(), 1u);
            unsigned Indirect = 0;
            for (const auto &Block : *Caller)
              for (const auto &Instruction : Block)
                if (const auto *Call =
                        llvm::dyn_cast<llvm::CallBase>(&Instruction);
                    Call && !Call->getCalledFunction()) {
                  EXPECT_EQ(Call->arg_size(), 2u);
                  if (isMIPS64(Architecture) && Call->arg_size() == 2)
                    EXPECT_EQ(Call->getArgOperand(0), Caller->getArg(0));
                  ++Indirect;
                }
            EXPECT_EQ(Indirect, 1u);
            ASSERT_TRUE(
                LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
          } else {
            ASSERT_TRUE(
                HighCEmitter().emit(Result.HighFuncs, Output, COptions));
          }
          EXPECT_EQ(Source.find("incoming register has no definition"),
                    std::string::npos)
              << Source;
          EXPECT_EQ(Source.find("no recovered machine state"),
                    std::string::npos)
              << Source;
          source_call_execution_test::checkSyntax(Source, {});
          if (Architecture == Arch::X64 || Architecture == Arch::AArch64) {
            // The slot has no target or signature evidence. Change it between
            // calls so an accidentally devirtualized call cannot pass.
            ASSERT_NE(Source.find("off_1300"), std::string::npos) << Source;
            Source += R"c(
#include <string.h>
static uint64_t first_callback(uint64_t x, uint64_t y) { return x ^ y; }
static uint64_t second_callback(uint64_t x, uint64_t y) { return x + y; }
int main(void) {
  uint64_t values[] = {0, 17, UINT64_C(0x100000002), UINT64_MAX};
  for (unsigned variant = 0; variant != 2; ++variant) {
    uintptr_t pointer = (uintptr_t)(variant ? second_callback : first_callback);
    memcpy(&off_1300, &pointer, sizeof(pointer));
    for (unsigned i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
      uint64_t expected = variant ? values[i] + 37 : values[i] ^ 37;
      if ((uint64_t)dynamic_probe(values[i]) != expected) return 1;
    }
  }
  return 0;
}
)c";
            for (const char *Optimization : {"-O0", "-O2"})
              source_call_execution_test::compileAndRun(
                  Source, {Optimization, "-fsanitize=undefined",
                           "-fno-sanitize-recover=all"});
          }
        }
    }
}
} // namespace
