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
#include "neverd/debug/DebugContext.h"
#include "neverd/pipeline/Limits.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/Verifier.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
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

BinaryImage nestedFixture(unsigned Depth, bool ObserveMissing = false,
                          bool Recursive = false, bool StackAlias = false,
                          bool Shared = false) {
  BinaryImage Image;
  Image.Arch = Arch::X64;
  Image.Bits = Bitness::Bits64;
  Image.Format = BinaryFormat::COFF;
  Image.Base = Image.Entry = 0x1000;
  Segment Code;
  Code.VA = Image.Entry;
  Code.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  const auto Call = [&](va_t Target) {
    const uint32_t Delta = Target - (Code.VA + Code.Data.size() + 5);
    Code.Data.push_back(0xe8);
    for (unsigned I = 0; I != 4; ++I)
      Code.Data.push_back(Delta >> (8 * I));
  };
  const auto Symbol = [&](va_t Entry, llvm::StringRef Name) {
    auto Function = neverd::Symbol::makeFunc(Entry);
    Function.Name = Name.str();
    Function.Size = Code.VA + Code.Data.size() - Entry;
    Image.Symbols.push_back(std::move(Function));
  };
  Code.Data = {0x48, 0x83, 0xec, 0x38,        // sub rsp,56
               0x48, 0x89, 0x54, 0x24, 0x30}; // mov [rsp+48],rdx
  if (StackAlias) {
    const uint8_t Alias[] = {0x48, 0x8d, 0x4c, 0x24, 0x30};
    Code.Data.insert(Code.Data.end(), std::begin(Alias), std::end(Alias));
  }
  Call(0x1100);
  if (StackAlias) {
    const uint8_t Read[] = {0x48, 0x33, 0x44, 0x24, 0x30};
    Code.Data.insert(Code.Data.end(), std::begin(Read), std::end(Read));
  }
  const uint8_t Return[] = {0x48, 0x83, 0xc4, 0x38, 0xc3};
  Code.Data.insert(Code.Data.end(), std::begin(Return), std::end(Return));
  Symbol(Image.Entry, "nested_caller");
  for (unsigned D = 0; D != Depth; ++D) {
    const va_t Entry = Image.Entry + (D + 1) * 0x100;
    Code.Data.resize(Entry - Image.Entry, 0xcc);
    const uint8_t Body[] = {0x48, 0x8b, 0x44, 0x24, 0x38, // mov rax,[rsp+56]
                            0x48, 0x33, 0x01,             // xor rax,[rcx]
                            0x48, 0x83, 0x01, 0x01};      // add qword [rcx],1
    Code.Data.insert(Code.Data.end(), std::begin(Body), std::end(Body));
    if (D + 1 == Depth && ObserveMissing) {
      const uint8_t Read[] = {0x48, 0x33, 0x44, 0x24, 0x30};
      Code.Data.insert(Code.Data.end(), std::begin(Read), std::end(Read));
    }
    if (D + 1 != Depth || Recursive) {
      for (unsigned I = 0; I != (Shared && !D ? 2u : 1u); ++I) {
        const uint8_t Setup[] = {0x48, 0x83, 0xec, 0x38, 0x48, 0x89, 0x4c,
                                 0x24, 0x20, 0x48, 0x89, 0x44, 0x24, 0x30};
        Code.Data.insert(Code.Data.end(), std::begin(Setup), std::end(Setup));
        Call(Recursive && D + 1 == Depth ? 0x1100 : Entry + 0x100);
        const uint8_t Restore[] = {0x48, 0x8b, 0x4c, 0x24, 0x20};
        Code.Data.insert(Code.Data.end(), std::begin(Restore),
                         std::end(Restore));
        Code.Data.insert(Code.Data.end(), std::begin(Return),
                         std::end(Return) - 1);
      }
    }
    Code.Data.push_back(0xc3);
    Symbol(Entry, "nested_body_" + std::to_string(D));
  }
  Code.Size = Code.FileSz = Code.Data.size();
  Image.Segments.push_back(std::move(Code));
  return Image;
}

std::string source(llvm::Module &Module) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  EXPECT_TRUE(LLVMCEmitter().emit(Module, OS));
  return Text;
}

std::string source(const SourceInlineHighView &View, const BinaryImage &Image,
                   DebugContext *Debug = nullptr) {
  std::string Text;
  llvm::raw_string_ostream OS(Text);
  CEmitterOptions Options;
  Options.Image = &Image;
  Options.TheArch = Image.Arch;
  Options.Format = Image.abiFormat();
  Options.SourceBodyOrigins = &View.BodyOrigins;
  CSourceMap Map;
  Options.SourceMap = &Map;
  HighCEmitter().emit(View.Functions, OS, Options, Debug);
  for (const auto &[Key, Variable] : Map.Variables)
    EXPECT_NE(Variable.Function, 0u);
  return Text;
}

TEST(SourceInlineCalls, KeepsActualPointerReadsWritesAndOutgoingAliases) {
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
      OS << "\n#if defined(__linux__) && defined(__x86_64__)\n"
            "#include <sys/mman.h>\n#include <string.h>\n"
            "static const unsigned char original[] = {";
      for (uint8_t Byte : Image.Segments.front().Data)
        OS << unsigned(Byte) << ',';
      OS << "};\n#endif\n#define MODE " << Mode << "\n";
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
#if defined(__linux__) && defined(__x86_64__)
    void *code = mmap(0, sizeof(original), PROT_READ|PROT_WRITE|PROT_EXEC,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 1;
    memcpy(code, original, sizeof(original));
    typedef uint64_t (__attribute__((ms_abi)) *native_fn)(uint64_t*, uint64_t);
#endif
    for (uint64_t i = 0; i != 1024; ++i) {
        uint64_t a = i * UINT64_C(0x9e3779b97f4a7c15), b = a;
        uint64_t value = i ^ UINT64_C(0xfedcba9876543200);
        uint64_t expected = MODE == 2 ? ((value + 23) ^ value) : (value ^ a);
        uint64_t generated = caller_probe(SOURCE_POINTER(&b), value);
        if (generated != expected || b != (MODE == 1 ? expected : a)) return 2;
#if defined(__linux__) && defined(__x86_64__)
        uint64_t native = ((native_fn)code)(&a, value);
        if (native != expected || a != b) return 3;
#endif
    }
#if defined(__linux__) && defined(__x86_64__)
    return munmap(code, sizeof(original)) != 0;
#else
    return 0;
#endif
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

TEST(SourceInlineCalls, NestedUnusedInputsKeepEveryMemoryEffect) {
  for (bool StackAlias : {false, true})
    for (bool Shared : {false, true})
      for (unsigned Depth = 1; Depth <= limits::pipeline::kSourceInlineCallees;
           ++Depth) {
        if (Shared && Depth != 2)
          continue;
        SCOPED_TRACE(Depth);
        SCOPED_TRACE(StackAlias);
        SCOPED_TRACE(Shared);
        auto Image = nestedFixture(Depth, false, false, StackAlias, Shared);
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
        const auto BeforeHigh = source(High, Image);
        auto Specialized = specializeUnavailableSourceCalls(
            Image, Original, *Original.LlvmModule, nullptr, {}, {}, &High);
        ASSERT_TRUE(Specialized.Module);
        ASSERT_TRUE(Specialized.High);
        ASSERT_TRUE(Specialized.PreparedHigh);
        EXPECT_EQ(source(*Original.LlvmModule), Before);
        EXPECT_EQ(source(High, Image), BeforeHigh);
        EXPECT_EQ(Specialized.High->BodyOrigins.size(), Depth);
        auto Replayed = projectSpecializedHighSourceCalls(
            Image, *Specialized.PreparedHigh, HighOptions, nullptr, High);
        ASSERT_TRUE(Replayed);
        EXPECT_EQ(source(*Replayed, Image), source(*Specialized.High, Image));
        if (Shared) {
          PipelineResult Reversed;
          Reversed.CallPrototypeArguments = Original.CallPrototypeArguments;
          auto Omitted =
              std::make_shared<OmittedCalleeBodies>(*Original.OmittedCallees);
          std::reverse(Omitted->Bodies.begin(), Omitted->Bodies.end());
          Reversed.OmittedCallees = std::move(Omitted);
          auto Reordered = specializeUnavailableSourceCalls(
              Image, Reversed, *Original.LlvmModule, nullptr, {}, {}, &High);
          ASSERT_TRUE(Reordered.Module);
          ASSERT_TRUE(Reordered.High);
          EXPECT_EQ(source(*Reordered.Module), source(*Specialized.Module));
          EXPECT_EQ(source(*Reordered.High, Image),
                    source(*Specialized.High, Image));
        }
        for (bool UseHigh : {false, true}) {
          SCOPED_TRACE(UseHigh);
          auto Text = "#define _GNU_SOURCE\n" +
                      (UseHigh ? source(*Specialized.High, Image)
                               : source(*Specialized.Module));
          ASSERT_EQ(Text.find("__builtin_trap"), std::string::npos) << Text;
          llvm::raw_string_ostream OS(Text);
          const bool Pointer =
              UseHigh ? Specialized.High->Functions.front()
                                .Params.front()
                                .Type->Kind == NdTypeKind::Ptr
                      : Specialized.Module->getFunction("nested_caller")
                            ->getArg(0)
                            ->getType()
                            ->isPointerTy();
          OS << "\n#define SOURCE_POINTER(p) ("
             << (Pointer ? "void*" : "uint64_t") << ")(p)\n";
          OS << "\n#define STACK_ALIAS " << StackAlias << "\n#define STEPS "
             << (Shared ? Depth * 2 - 1 : Depth) << R"C(
#if defined(__linux__) && defined(__x86_64__)
#include <sys/mman.h>
#include <string.h>
static const unsigned char original[] = {
)C";
          for (uint8_t Byte : Image.Segments.front().Data)
            OS << unsigned(Byte) << ',';
          OS << R"C(
};
#endif
int main(void) {
#if defined(__linux__) && defined(__x86_64__)
    void *code = mmap(0, sizeof(original), PROT_READ|PROT_WRITE|PROT_EXEC,
                      MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) return 1;
    memcpy(code, original, sizeof(original));
    typedef uint64_t (__attribute__((ms_abi)) *native_fn)(uint64_t*, uint64_t);
#endif
    for (uint64_t i = 0; i != 1024; ++i) {
        uint64_t initial = i * UINT64_C(0x9e3779b97f4a7c15);
        uint64_t input = i ^ UINT64_C(0xfedcba9876543200);
        uint64_t memory = STACK_ALIAS ? input : initial, expected = input;
        for (unsigned d = 0; d != STEPS; ++d) {
            expected ^= memory;
            ++memory;
        }
        if (STACK_ALIAS) expected ^= memory;
        uint64_t actual_memory = initial;
        uint64_t actual = nested_caller(SOURCE_POINTER(&actual_memory), input);
        uint64_t expected_memory = STACK_ALIAS ? initial : memory;
        if (actual != expected || actual_memory != expected_memory) return 2;
#if defined(__linux__) && defined(__x86_64__)
        uint64_t native_memory = initial;
        uint64_t native = ((native_fn)code)(&native_memory, input);
        if (native != expected || native_memory != expected_memory) return 3;
#endif
    }
#if defined(__linux__) && defined(__x86_64__)
    return munmap(code, sizeof(original)) != 0;
#else
    return 0;
#endif
}
)C";
          for (const char *Optimization : {"-O0", "-O2"})
            source_call_execution_test::compileAndRun(
                Text, {Optimization, "-fsanitize=undefined",
                       "-fno-sanitize-recover=all"});
        }
      }
}

TEST(SourceInlineCalls, NestedObserversCyclesAndLimitsRetainTheOriginalView) {
  for (unsigned Case = 0; Case != 3; ++Case) {
    SCOPED_TRACE(Case);
    auto Image = nestedFixture(
        Case == 2 ? limits::pipeline::kSourceInlineCallees + 1 : 2,
        /*ObserveMissing=*/Case == 0, /*Recursive=*/Case == 1);
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = Options.SourceProjection = true;
    Options.OnlyFunctionEntries = {Image.Entry};
    auto Original = Pipeline().run(Image, Context, Options);
    ASSERT_TRUE(Original.Success) << Original.Error;
    ASSERT_TRUE(Original.LlvmModule);
    const auto Before = source(*Original.LlvmModule);
    auto Specialized = specializeUnavailableSourceCalls(
        Image, Original, *Original.LlvmModule, nullptr, {}, {});
    EXPECT_FALSE(Specialized.Module);
    EXPECT_EQ(source(*Original.LlvmModule), Before);
    EXPECT_NE(Before.find("__builtin_trap"), std::string::npos);
  }
}

TEST(SourceInlineCalls, HelperOriginsDoNotBindNativeDebugAtAddressZero) {
  class ZeroDebug : public NullDebugContext {
  public:
    bool hasInfo() const override { return true; }
    std::optional<FunctionSym> resolveFunction(va_t Entry) const override {
      if (Entry == 0x3000) {
        FunctionSym Function;
        Function.Name = "typed_sink";
        Function.Addr = Entry;
        Function.ReturnType = NdType::makeVoid();
        Function.Params.emplace_back(
            "pointer", NdType::makePtr(NdType::makeInt(8, false)));
        return Function;
      }
      if (Entry)
        return std::nullopt;
      FunctionSym Function;
      Function.Name = "native_zero_abi";
      Function.ReturnType = NdType::makeInt(4, false);
      return Function;
    }
  } Debug;
  auto Image = fixture(1);
  llvm::LLVMContext Context;
  PipelineOptions Options;
  Options.EmitDumpOutput = false;
  Options.LiftMode = Options.SourceProjection = true;
  Options.OnlyFunctionEntries = {Image.Entry};
  auto Original = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Original.Success) << Original.Error;
  PipelineOptions HighOptions;
  HighOptions.EmitDumpOutput = false;
  HighOptions.OnlyFunctionEntries = {Image.Entry};
  auto HighOriginal = Pipeline().run(Image, Context, HighOptions);
  ASSERT_TRUE(HighOriginal.Success) << HighOriginal.Error;
  SourceInlineHighView High{
      HighOriginal.HighFuncs, HighOriginal.HighSources, {}};
  auto Specialized = specializeUnavailableSourceCalls(
      Image, Original, *Original.LlvmModule, nullptr, {}, {}, &High);
  ASSERT_TRUE(Specialized.High);
  for (auto &Function : Specialized.High->Functions)
    if (Specialized.High->BodyOrigins.count(Function.Name)) {
      ASSERT_FALSE(Function.Body.empty());
      ASSERT_EQ(Function.Body.back().Kind, StmtKind::Return);
      MedVar Value;
      Value.Kind = MedVar::Param;
      Value.Id = Function.Params.size() - 1;
      Value.Size = 8;
      HighStmt Call;
      Call.Kind = StmtKind::Call;
      Call.CallExpr = HighExpr::makeCall(
          "typed_sink", 0x3000,
          {HighExpr::makeVar(Value, NdType::makeInt(8, false))});
      Call.CallExpr->Type = NdType::makeVoid();
      Function.Body.insert(Function.Body.end() - 1, std::move(Call));
    }
  // Address zero can also identify an actual native definition. Only the
  // explicit source-body origin distinguishes the helper from that function.
  HighFunc Native;
  Native.Name = "native_zero_abi";
  Native.ReturnType = NdType::makeInt(8, false);
  HighStmt Return;
  Return.Kind = StmtKind::Return;
  Return.RetVal = HighExpr::makeConst(0x12345678, 4);
  Native.Body.push_back(std::move(Return));
  Specialized.High->Functions.push_back(std::move(Native));
  auto Text = source(*Specialized.High, Image, &Debug);
  EXPECT_NE(Text.find("uint32_t native_zero_abi("), std::string::npos) << Text;
  const bool Pointer =
      Specialized.High->Functions.front().Params.front().Type->Kind ==
      NdTypeKind::Ptr;
  llvm::raw_string_ostream OS(Text);
  OS << "\n#define SOURCE_POINTER(p) (" << (Pointer ? "void*" : "uint64_t")
     << ")(p)\n";
  OS << R"C(
static uint64_t observed_pointer;
void typed_sink(uint64_t *pointer) {
    observed_pointer = (uintptr_t)pointer;
}
int main(void) {
    if (native_zero_abi() != UINT32_C(0x12345678)) return 1;
    for (uint64_t i = 0; i != 1024; ++i) {
        uint64_t initial = i * UINT64_C(0x9e3779b97f4a7c15);
        uint64_t input = i ^ UINT64_C(0xfedcba9876543200);
        uint64_t memory = initial, expected = initial ^ input;
        uint64_t actual = caller_probe(SOURCE_POINTER(&memory), input);
        if (actual != expected || memory != expected ||
            observed_pointer != input) return 2;
    }
    return 0;
}
)C";
  for (const char *Optimization : {"-O0", "-O2"})
    source_call_execution_test::compileAndRun(
        Text,
        {Optimization, "-fsanitize=undefined", "-fno-sanitize-recover=all"});
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
