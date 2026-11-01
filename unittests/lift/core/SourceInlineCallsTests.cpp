//===- SourceInlineCallsTests.cpp - Execute specialized call bodies -------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Missing formal operands do not authorize erasing physical memory effects.
//===----------------------------------------------------------------------===//

#include "../../../lib/pipeline/PipelineSourceCallObservations.h"
#include "../../../lib/pipeline/PipelineSourceInline.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CSourceMap.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <cstdint>
#include <iterator>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

BinaryImage fixture(unsigned Mode) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = Image.Entry = 0x1000;
  Segment Code;
  Code.VA = Image.Entry;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Code.Data = {0x48, 0x83, 0xec, 0x38,        // sub rsp,56
               0x48, 0x89, 0x54, 0x24, 0x30}; // mov [rsp+48],rdx
  if (Mode == 2) {
    const uint8_t Alias[] = {0x48, 0x89, 0x54, 0x24, 0x28, // mov [rsp+40],rdx
                             0x48, 0x8d, 0x4c, 0x24, 0x28, // lea rcx,[rsp+40]
                             0x48, 0x83, 0x44, 0x24, 0x30,
                             0x17}; // add [rsp+48],23
    Code.Data.insert(Code.Data.end(), std::begin(Alias), std::end(Alias));
  }
  const uint32_t Delta = 0x80 - Code.Data.size() - 5;
  Code.Data.push_back(0xe8);
  for (unsigned I = 0; I != 4; ++I)
    Code.Data.push_back(Delta >> (8 * I));
  const uint8_t Return[] = {0x48, 0x83, 0xc4, 0x38, 0xc3};
  Code.Data.insert(Code.Data.end(), std::begin(Return), std::end(Return));
  const size_t CallerBytes = Code.Data.size();
  Code.Data.resize(0x80, 0xcc);
  const uint8_t Callee[] = {0x48, 0x8b, 0x44, 0x24, 0x38, // mov rax,[rsp+56]
                            0x48, 0x33, 0x01};            // xor rax,[rcx]
  Code.Data.insert(Code.Data.end(), std::begin(Callee), std::end(Callee));
  if (Mode == 1) {
    const uint8_t Store[] = {0x48, 0x89, 0x01}; // mov [rcx],rax
    Code.Data.insert(Code.Data.end(), std::begin(Store), std::end(Store));
  }
  if (Mode == 3) {
    const uint8_t Reads[] = {0x48, 0x33, 0x44, 0x24, 0x28,  // xor rax,[rsp+40]
                             0x48, 0x33, 0x44, 0x24, 0x30}; // xor rax,[rsp+48]
    Code.Data.insert(Code.Data.end(), std::begin(Reads), std::end(Reads));
  }
  if (Mode == 4) {
    // The outer helper ignores its own missing slots, but forwards a result
    // to a nested helper that actually reads uninitialized outgoing slots.
    const uint8_t Nested[] = {0x48, 0x83, 0xec, 0x38, 0x48,
                              0x89, 0x44, 0x24, 0x30};
    Code.Data.insert(Code.Data.end(), std::begin(Nested), std::end(Nested));
    const uint32_t NestedDelta = 0x100 - Code.Data.size() - 5;
    Code.Data.push_back(0xe8);
    for (unsigned I = 0; I != 4; ++I)
      Code.Data.push_back(NestedDelta >> (8 * I));
    Code.Data.insert(Code.Data.end(), std::begin(Return), std::end(Return) - 1);
  }
  Code.Data.push_back(0xc3);
  const size_t CalleeBytes = Code.Data.size() - 0x80;
  if (Mode == 4) {
    Code.Data.resize(0x100, 0xcc);
    const uint8_t Nested[] = {0x48, 0x8b, 0x44, 0x24, 0x38, 0x48, 0x33, 0x44,
                              0x24, 0x28, 0x48, 0x33, 0x44, 0x24, 0x30, 0xc3};
    Code.Data.insert(Code.Data.end(), std::begin(Nested), std::end(Nested));
    auto NestedSymbol = Symbol::makeFunc(0x1100);
    NestedSymbol.Name = "nested_probe";
    NestedSymbol.Size = sizeof(Nested);
    Image.Symbols.push_back(std::move(NestedSymbol));
  }
  Code.Size = Code.FileSz = Code.Data.size();
  for (const auto &[Entry, Name, Bytes] :
       {std::tuple{va_t(0x1000), "caller_probe", CallerBytes},
        std::tuple{va_t(0x1080), "callee_probe", CalleeBytes}}) {
    auto Symbol = Symbol::makeFunc(Entry);
    Symbol.Name = Name;
    Symbol.Size = Bytes;
    Image.Symbols.push_back(std::move(Symbol));
  }
  Image.Segments.push_back(std::move(Code));
  if (Mode == 1) {
    Symbol Collision;
    Collision.Name = "neverd_source_body_1080";
    Collision.Addr = 0x2000;
    Image.Symbols.push_back(std::move(Collision));
  }
  return Image;
}

std::string source(llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  EXPECT_TRUE(LLVMCEmitter().emit(Module, OS));
  return Text;
}

std::string source(const SourceInlineHighView &View, const BinaryImage &Image) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  Options.Image = &Image;
  Options.TheArch = Image.Arch;
  Options.Format = Image.abiFormat();
  Options.SourceBodyOrigins = &View.BodyOrigins;
  CSourceMap Map;
  Options.SourceMap = &Map;
  HighCEmitter().emit(View.Functions, OS, Options);
  for (const auto &[Key, Variable] : Map.Variables)
    EXPECT_NE(Variable.Function, 0u);
  return Text;
}

TEST(SourceInlineCalls, KeepsActualPointerReadsWritesAndOutgoingAliases) {
#if !defined(__x86_64__) || !defined(__linux__)
  GTEST_SKIP() << "the original Win64 bytes require Linux x64 mmap";
#endif
  for (unsigned Mode = 0; Mode != 3; ++Mode) {
    SCOPED_TRACE(Mode);
    auto Image = fixture(Mode);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = Options.SourceProjection = true;
    Options.OnlyFunctionEntries = {Image.Entry};
    auto Original = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Original.Success) << Original.Error;
    ASSERT_TRUE(Original.LlvmModule);
    const auto Before = source(*Original.LlvmModule);
    ASSERT_NE(Before.find("__builtin_trap"), std::string::npos) << Before;
    PipelineOptions HighOptions;
    HighOptions.EmitDumpOutput = false;
    HighOptions.OnlyFunctionEntries = {Image.Entry};
    auto HighOriginal = Pipeline().run(Image, Context, HighOptions);
    ASSERT_TRUE(HighOriginal.Success) << HighOriginal.Error;
    SourceInlineHighView High{
        HighOriginal.HighFuncs, HighOriginal.HighSources, {}};
    ASSERT_TRUE(hasUnavailableSourceStackCall(High.Functions));
    ASSERT_TRUE(hasUnavailableSourceStackCall(High.Functions, &Original));
    const auto BeforeHigh = source(High, Image);
    auto Specialized = specializeUnavailableSourceCalls(
        Image, Original, *Original.LlvmModule, nullptr, {}, {}, &High);
    ASSERT_TRUE(Specialized.Module);
    ASSERT_TRUE(Specialized.High);
    ASSERT_TRUE(Specialized.PreparedHigh);
    auto Replayed = projectSpecializedHighSourceCalls(
        Image, *Specialized.PreparedHigh, HighOptions, nullptr, High);
    ASSERT_TRUE(Replayed);
    EXPECT_EQ(source(*Replayed, Image), source(*Specialized.High, Image));
    ASSERT_EQ(Specialized.High->BodyOrigins.size(), 1u);
    if (Mode == 1)
      EXPECT_NE(Specialized.High->BodyOrigins.begin()->first,
                "neverd_source_body_1080");
    ASSERT_FALSE(llvm::verifyModule(*Specialized.Module, &llvm::errs()));
    EXPECT_EQ(source(*Original.LlvmModule), Before);
    EXPECT_EQ(source(High, Image), BeforeHigh);
    for (bool UseHigh : {false, true}) {
      SCOPED_TRACE(UseHigh);
      auto Text =
          "#define _GNU_SOURCE\n" + (UseHigh ? source(*Specialized.High, Image)
                                             : source(*Specialized.Module));
      ASSERT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
      if (const auto *Callee = Specialized.Module->getFunction("callee_probe"))
        EXPECT_TRUE(Callee->isDeclaration());
      llvm::raw_string_ostream OS(Text);
      OS << "\n#include <sys/mman.h>\n#include <string.h>\n"
            "static const unsigned char original[] = {";
      for (uint8_t Byte : Image.Segments.front().Data)
        OS << unsigned(Byte) << ',';
      OS << "};\n#define MODE " << Mode << "\n";
      const auto *Caller = Specialized.Module->getFunction("caller_probe");
      ASSERT_TRUE(Caller);
      ASSERT_EQ(Caller->arg_size(), 2u);
      OS << "#define SOURCE_POINTER(p) ("
         << ((UseHigh ? Specialized.High->Functions.front()
                                .Params.front()
                                .Type->Kind == NdTypeKind::Ptr
                      : Caller->getArg(0)->getType()->isPointerTy())
                 ? "void*"
                 : "uint64_t")
         << ")(p)\n";
      OS << R"C(
int main(void) {
    void *code = mmap(0, sizeof(original), PROT_READ|PROT_WRITE|PROT_EXEC,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 1;
    memcpy(code, original, sizeof(original));
    typedef uint64_t (__attribute__((ms_abi)) *native_fn)(uint64_t*, uint64_t);
    for (uint64_t i = 0; i != 1024; ++i) {
        uint64_t a = i * UINT64_C(0x9e3779b97f4a7c15), b = a;
        uint64_t value = i ^ UINT64_C(0xfedcba9876543200);
        uint64_t expected = MODE == 2 ? ((value + 23) ^ value) : (value ^ a);
        uint64_t native = ((native_fn)code)(&a, value);
        uint64_t generated = caller_probe(SOURCE_POINTER(&b), value);
        if (native != expected || generated != expected || a != b) return 2;
        if (MODE == 1 && a != expected) return 3;
    }
    return munmap(code, sizeof(original)) != 0;
}
)C";
      for (const char *Optimization : {"-O0", "-O2"})
        source_call_execution_test::compileAndRun(
            Text, {Optimization, "-fsanitize=undefined",
                   "-fno-sanitize-recover=all"});
    }
  }
}

TEST(SourceInlineCalls, ObservedMissingSlotsRemainUnavailable) {
  auto Image = fixture(3);
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.LiftMode = Options.SourceProjection = true;
  Options.OnlyFunctionEntries = {Image.Entry};
  auto Original = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Original.Success) << Original.Error;
  ASSERT_TRUE(Original.LlvmModule);
  auto Specialized = specializeUnavailableSourceCalls(
      Image, Original, *Original.LlvmModule, nullptr, {}, {});
  EXPECT_FALSE(Specialized.Module);
  EXPECT_NE(source(*Original.LlvmModule).find("__builtin_trap"),
            std::string::npos);
}

TEST(SourceInlineCalls, NestedMissingInputsDoNotBecomeNewFailures) {
  auto Image = fixture(4);
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.LiftMode = Options.SourceProjection = true;
  Options.OnlyFunctionEntries = {Image.Entry};
  auto Original = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Original.Success) << Original.Error;
  ASSERT_TRUE(Original.LlvmModule);
  auto Specialized = specializeUnavailableSourceCalls(
      Image, Original, *Original.LlvmModule, nullptr, {}, {});
  EXPECT_FALSE(Specialized.Module);
  EXPECT_NE(source(*Original.LlvmModule).find("__builtin_trap"),
            std::string::npos);
}

TEST(SourceInlineCalls, DeadUnknownPhisNeedNoInputButReadersDo) {
  llvm::LLVMContext Context;
  llvm::Module Module("observations", Context);
  llvm::IRBuilder<> Builder(Context);
  auto *Function = llvm::Function::Create(
      llvm::FunctionType::get(Builder.getInt64Ty(), {Builder.getInt1Ty()},
                              false),
      llvm::GlobalValue::ExternalLinkage, "body", Module);
  auto *Entry = llvm::BasicBlock::Create(Context, "entry", Function);
  auto *Left = llvm::BasicBlock::Create(Context, "left", Function);
  auto *Right = llvm::BasicBlock::Create(Context, "right", Function);
  auto *Join = llvm::BasicBlock::Create(Context, "join", Function);
  Builder.SetInsertPoint(Entry);
  auto *Unknown = llvm_source_unknown::create(Builder, Builder.getInt64Ty());
  Builder.CreateCondBr(Function->getArg(0), Left, Right);
  Builder.SetInsertPoint(Left);
  Builder.CreateBr(Join);
  Builder.SetInsertPoint(Right);
  Builder.CreateBr(Join);
  Builder.SetInsertPoint(Join);
  auto *Phi = Builder.CreatePHI(Builder.getInt64Ty(), 2);
  Phi->addIncoming(Unknown, Left);
  Phi->addIncoming(Builder.getInt64(7), Right);
  auto *Copy = Builder.CreateZExt(Builder.CreateTrunc(Phi, Builder.getInt8Ty()),
                                  Builder.getInt64Ty());
  auto *Return = Builder.CreateRet(Builder.getInt64(0));
  ASSERT_FALSE(llvm::verifyModule(Module, &llvm::errs()));
  EXPECT_FALSE(source_inline::hasUnavailableObservations(*Function));
  Return->setOperand(0, Copy);
  EXPECT_TRUE(source_inline::hasUnavailableObservations(*Function));
  Return->setOperand(0, Builder.getInt64(0));
  Builder.SetInsertPoint(Return);
  Builder.CreateLoad(Builder.getInt8Ty(),
                     Builder.CreateIntToPtr(Phi, Builder.getPtrTy()));
  EXPECT_TRUE(source_inline::hasUnavailableObservations(*Function));
}
} // namespace
