//===- RecordCallContractTests.cpp - Header-declared record calls --------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Compile linked ELF/PE import fixtures, then execute both C emitters against
/// independent host providers that observe arguments and hidden result writes.
///
//===----------------------------------------------------------------------===//

#include "LocalTypesDebugContext.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/backend/llvm/LLVMFloatingCall.h"
#include "neverd/ir/med/MedABIPass.h"
#include "neverd/libc/LibCRecordArguments.h"
#include "neverd/loader/Loader.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/types/TypeParse.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"

#include <fstream>
#include <utility>

using namespace neverd;

namespace {
class EmptyDebug final : public DebugContext {
public:
  std::optional<FunctionSym> resolveFunction(va_t) const override { return {}; }
  std::optional<VariableSym> resolveVariable(va_t, int64_t) const override {
    return {};
  }
  std::optional<TypeSym> resolveType(uint64_t) const override { return {}; }
  std::optional<SourceLoc> sourceLocation(va_t) const override { return {}; }
  std::vector<FunctionSym> allFunctions() const override { return {}; }
  bool hasInfo() const override { return false; }
};

TEST(RecordCallContract, HeaderRecordsUseTheirTargetsMachinePositions) {
  struct Target {
    types::HeaderLibrary Header;
    Arch Architecture;
    BinaryFormat Format;
    libc::LibCVariant Variant;
  };
  for (const auto &Target :
       {Target{types::HeaderLibrary::LinuxX64, Arch::X64, BinaryFormat::ELF,
               libc::LibCVariant::GNU},
        Target{types::HeaderLibrary::LinuxX86, Arch::X86, BinaryFormat::ELF,
               libc::LibCVariant::GNU},
        Target{types::HeaderLibrary::LinuxARM64, Arch::AArch64,
               BinaryFormat::ELF, libc::LibCVariant::GNU},
        Target{types::HeaderLibrary::MuslX64, Arch::X64, BinaryFormat::ELF,
               libc::LibCVariant::Musl},
        Target{types::HeaderLibrary::MacOSX64, Arch::X64, BinaryFormat::MachO,
               libc::LibCVariant::Darwin}}) {
    SCOPED_TRACE(types::headerLibraryName(Target.Header).str());
    const auto *Header = types::headerLibrary(Target.Header);
    ASSERT_NE(Header, nullptr);
    auto Types = std::make_shared<types::TypeLibrary>(Header->dataModel());
    Types->setRecordABI(Header->recordABI());
    types::mergeHeaderTypes(*Types, *Header);
    EmptyDebug Empty;
    sdk::LocalTypesDebugContext Debug(Empty, Types, {}, nullptr,
                                      {Target.Header});
    const auto Search = Debug.resolvePrototype("hsearch");
    ASSERT_TRUE(Search);
    const auto Entry = libc::recordCallArguments(*Search, Target.Architecture,
                                                 Target.Format, Target.Variant);
    ASSERT_TRUE(Entry);
    EXPECT_EQ(Entry->Positions, 3u);
    EXPECT_EQ(Entry->Registers, Target.Architecture == Arch::X86 ? 0u : 3u);
    EXPECT_EQ(Entry->Parameters[1].Pieces[0].Argument, 2u);
    if (const auto Cookie = Debug.resolvePrototype("fopencookie")) {
      const auto Layout = libc::recordCallArguments(
          *Cookie, Target.Architecture, Target.Format, Target.Variant);
      ASSERT_TRUE(Layout);
      const bool I386 = Target.Architecture == Arch::X86;
      const bool Indirect = Target.Architecture == Arch::AArch64;
      EXPECT_EQ(Layout->Positions, I386 ? 6u : Indirect ? 3u : 10u);
      EXPECT_EQ(Layout->Registers, I386 ? 0u : Indirect ? 3u : 2u);
      EXPECT_EQ(Layout->Parameters[2].Indirect, Indirect);
    }
  }
}

const char *Original = R"(
#define _GNU_SOURCE 1
#include <stdio.h>
#include <stdint.h>
#include <search.h>
#include <arpa/inet.h>
uintptr_t probe_cookie(void) {
  cookie_io_functions_t io = {
    (cookie_read_function_t *)(uintptr_t)0x1111222233334444ULL,
    (cookie_write_function_t *)(uintptr_t)0x5555666677778888ULL,
    (cookie_seek_function_t *)(uintptr_t)0x9999AAAABBBBCCCCULL,
    (cookie_close_function_t *)(uintptr_t)0xDDDDEEEEFFFF0000ULL
  };
  return (uintptr_t)fopencookie((void *)(uintptr_t)0x1122334455667788ULL,
                              (char *)(uintptr_t)0x13579BDF2468ACE0ULL, io);
}
int probe_entry(void) {
  ENTRY item = {(char *)(uintptr_t)0x1021324354657687ULL,
                (void *)(uintptr_t)0x98A9BACBDCEDFE0FULL};
  return hsearch_r(item, ENTER, (ENTRY **)(uintptr_t)0x8877665544332211ULL,
                   (struct hsearch_data *)(uintptr_t)0x1020304050607080ULL);
}
uintptr_t probe_search(void) {
  ENTRY item = {(char *)(uintptr_t)0x1234567890ABCDEFULL,
                (void *)(uintptr_t)0xFEDCBA0987654321ULL};
  return (uintptr_t)hsearch(item, FIND);
}
uintptr_t probe_inet(void) {
  struct in_addr address = {0x89ABCDEF};
  return (uintptr_t)inet_ntoa(address);
}
)";

// No supplied pointer is dereferenced or called. Every bit, including high
// pointer bits, the action, and the pointers after ENTRY are observable.
const char *Provider = R"(
#include <stdio.h>
#include <stdint.h>
#include <search.h>
#include <arpa/inet.h>
FILE *fopencookie(void *cookie, const char *mode, cookie_io_functions_t io) {
  return (uintptr_t)cookie == 0x1122334455667788ULL &&
         (uintptr_t)mode == 0x13579BDF2468ACE0ULL &&
         (uintptr_t)io.read == 0x1111222233334444ULL &&
         (uintptr_t)io.write == 0x5555666677778888ULL &&
         (uintptr_t)io.seek == 0x9999AAAABBBBCCCCULL &&
         (uintptr_t)io.close == 0xDDDDEEEEFFFF0000ULL
             ? (FILE *)(uintptr_t)0x12345678 : 0;
}
int hsearch_r(ENTRY item, ACTION action, ENTRY **out, struct hsearch_data *table) {
  return (uintptr_t)item.key == 0x1021324354657687ULL &&
         (uintptr_t)item.data == 0x98A9BACBDCEDFE0FULL && action == ENTER &&
         (uintptr_t)out == 0x8877665544332211ULL &&
         (uintptr_t)table == 0x1020304050607080ULL ? 37 : 0;
}
ENTRY *hsearch(ENTRY item, ACTION action) {
  return (uintptr_t)item.key == 0x1234567890ABCDEFULL &&
         (uintptr_t)item.data == 0xFEDCBA0987654321ULL && action == FIND
             ? (ENTRY *)(uintptr_t)0x23456789 : 0;
}
char *inet_ntoa(struct in_addr address) {
  return address.s_addr == 0x89ABCDEF ? (char *)(uintptr_t)0x3456789A : 0;
}
int main(void) {
  return (uintptr_t)probe_cookie() == 0x12345678 && probe_entry() == 37 &&
         (uintptr_t)probe_search() == 0x23456789 &&
         (uintptr_t)probe_inet() == 0x3456789A ? 0 : 1;
}
)";

void write(const std::string &Path, const std::string &Text) {
  std::ofstream Stream(Path);
  ASSERT_TRUE(Stream.good());
  Stream << Text;
}

bool execute(llvm::StringRef Program, std::vector<llvm::StringRef> Arguments,
             const std::string &ErrorPath) {
  const std::optional<llvm::StringRef> Redirects[] = {std::nullopt,
                                                      std::nullopt, ErrorPath};
  std::string Error;
  const int Status = llvm::sys::ExecuteAndWait(Program, Arguments, std::nullopt,
                                               Redirects, 60, 0, &Error);
  if (Status == 0)
    return true;
  const auto Log = llvm::MemoryBuffer::getFile(ErrorPath);
  ADD_FAILURE() << Error << '\n'
                << (Log ? (*Log)->getBuffer().str() : "") << '\n'
                << "exit status " << Status;
  return false;
}

TEST(RecordCallContract, LinkedLibCRecordsExecuteThroughBothCEmitters) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "Native glibc x86-64 headers and execution are required";
#else
  const std::string Compiler = NEVERD_TEST_CLANG;
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-record-call", Directory));
  const auto Cleanup = llvm::make_scope_exit(
      [&] { llvm::sys::fs::remove_directories(Directory); });
  const std::string Root = Directory.str().str();
  const std::string Input = Root + "/original.c",
                    LibraryPath = Root + "/input.so",
                    Generated = Root + "/generated.c",
                    Oracle = Root + "/oracle.c", Product = Root + "/run",
                    Errors = Root + "/errors.txt";
  write(Input, Original);
  write(Oracle,
        std::string(
            "#define _GNU_SOURCE 1\n#include <stdint.h>\n"
            "uintptr_t probe_cookie(void); int probe_entry(void);\n"
            "uintptr_t probe_search(void); uintptr_t probe_inet(void);\n") +
            Provider);
  const auto *Header = types::headerLibrary(types::HeaderLibrary::LinuxX64);
  ASSERT_NE(Header, nullptr);
  auto Types = std::make_shared<types::TypeLibrary>(Header->dataModel());
  types::mergeHeaderTypes(*Types, *Header);
  for (const char *Optimization : {"-O0", "-O2"}) {
    SCOPED_TRACE(Optimization);
    ASSERT_TRUE(execute(
        Compiler,
        {Compiler, "-std=gnu11", Optimization, Input, Oracle, "-o", Product},
        Errors));
    ASSERT_TRUE(execute(Product, {Product}, Errors));
    ASSERT_TRUE(
        execute(Compiler,
                {Compiler, "-std=gnu11", Optimization, "-fPIC", "-shared",
                 "-nostdlib", "-fuse-ld=lld", Input, "-o", LibraryPath},
                Errors));
    auto Reader = Loader::create(LibraryPath);
    ASSERT_TRUE(bool(Reader));
    auto Loaded = Reader->load(LibraryPath);
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    BinaryImage Image = std::move(*Loaded);
    EmptyDebug Empty;
    sdk::LocalTypesDebugContext Debug(Empty, Types, {}, &Image,
                                      {types::HeaderLibrary::LinuxX64});
    for (bool Restricted : {false, true}) {
      SCOPED_TRACE(Restricted ? "selected functions" : "whole module");
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM ? "LLVM-C" : "HighC");
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.LiftMode = LLVM;
        Options.SourceProjection = LLVM;
        if (Restricted) {
          for (const auto &Symbol : Image.Symbols)
            if (llvm::StringRef(Symbol.Name).starts_with("probe_"))
              Options.OnlyFunctionEntries.insert(Symbol.Addr);
          ASSERT_EQ(Options.OnlyFunctionEntries.size(), 4u);
        }
        auto Result = Pipeline().run(Image, Context, Options, &Debug);
        ASSERT_TRUE(Result.Success) << Result.Error;
        EXPECT_FALSE(Result.CallPrototypeArguments.empty());
        EXPECT_TRUE(Result.CallUserPrototypes.empty());
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        COptions.RecordDefinitions = [&](const std::vector<TypeRef> &Records) {
          return Debug.definitions(Records);
        };
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
        write(Generated, Source + Provider);
        for (const char *COptimization : {"-O0", "-O2"}) {
          SCOPED_TRACE(COptimization);
          // Whole modules retain separately named import-slot aliases for
          // the SDK's image linker. Verify their complete declarations and
          // calls; native provider linkage below exercises the selected
          // functions, which call the actual header-declared routine names.
          if (!Restricted) {
            ASSERT_TRUE(
                execute(Compiler,
                        {Compiler, "-std=gnu11", COptimization, "-fsyntax-only",
                         "-Werror=int-conversion",
                         "-Werror=incompatible-pointer-types", Generated},
                        Errors))
                << Source;
            continue;
          }
          ASSERT_TRUE(execute(
              Compiler,
              {Compiler, "-std=gnu11", COptimization,
               "-fsanitize=address,undefined", "-Werror=int-conversion",
               "-Werror=incompatible-pointer-types", Generated, "-o", Product},
              Errors))
              << Source;
          ASSERT_TRUE(execute(Product, {Product}, Errors)) << Source;
        }
      }
    }
  }
#endif
}

// This original declaration has Microsoft's documented complex record ABI.
// It is also the independent host provider's header: no host _Complex ABI is
// substituted for the two-double Windows record.
constexpr const char *WindowsComplexHeader = R"(
#pragma once
typedef struct _C_double_complex { double _Val[2]; } _Dcomplex;
_Dcomplex cpow(_Dcomplex, _Dcomplex);
)";

class DeclaredHeaderDebug final : public NullDebugContext {
  const sdk::LocalTypesDebugContext &Inner;
  static std::optional<FunctionSym> asHeader(std::optional<FunctionSym> F) {
    if (F && (F->Name == "cpow" || F->Name == "fixture_hfa")) {
      F->PrototypeFromHeader = true;
      F->PrototypeLibrary = "test-windows-header";
    }
    return F;
  }

public:
  explicit DeclaredHeaderDebug(const sdk::LocalTypesDebugContext &Context)
      : Inner(Context) {}
  std::optional<FunctionSym> resolveFunction(va_t Address) const override {
    return asHeader(Inner.resolveFunction(Address));
  }
  std::optional<FunctionSym>
  resolvePrototype(llvm::StringRef Name) const override {
    return asHeader(Inner.resolvePrototype(Name));
  }
};

TEST(RecordCallContract, WindowsRecordResultsPreserveEveryByte) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "Host execution of the generated C requires Linux x86-64";
#else
  for (const auto &[ARM64, Chain] :
       {std::pair{false, false}, {true, false}, {false, true}, {true, true}}) {
    SCOPED_TRACE(ARM64 ? "ARM64 HFA" : "x64 hidden result");
    SCOPED_TRACE(Chain ? "chained result" : "single call");
    const std::string Target = ARM64 ? "--target=aarch64-pc-windows-msvc"
                                     : "--target=x86_64-pc-windows-msvc";
    const std::string Compiler = NEVERD_TEST_CLANG;
    const auto Linker = llvm::sys::findProgramByName("lld-link");
    if (!Linker)
      GTEST_SKIP()
          << "lld-link is required to build the Windows import fixture";
    llvm::SmallString<128> Directory;
    ASSERT_FALSE(
        llvm::sys::fs::createUniqueDirectory("neverd-win-record", Directory));
    const auto Cleanup = llvm::make_scope_exit(
        [&] { llvm::sys::fs::remove_directories(Directory); });
    const std::string Root = Directory.str().str();
    const std::string Header = Root + "/complex.h", Input = Root + "/input.c",
                      ProviderPath = Root + "/provider.c",
                      Object = Root + "/input.obj",
                      ProviderObject = Root + "/provider.obj",
                      ImportLibrary = Root + "/provider.lib",
                      ProviderDLL = "/out:" + Root + "/provider.dll",
                      ImportOption = "/implib:" + ImportLibrary,
                      DLL = Root + "/input.dll", DLLOption = "/out:" + DLL,
                      Generated = Root + "/generated.c",
                      Product = Root + "/run", Include = "-I" + Root,
                      Errors = Root + "/errors.txt";
    write(Header, WindowsComplexHeader);
    write(ProviderPath, R"(
#include "complex.h"
int _fltused;
__declspec(dllexport) _Dcomplex cpow(_Dcomplex x, _Dcomplex y) { return x; }
)");
    std::string Fixture = ARM64 ? R"(
#include "complex.h"
int _fltused;
__declspec(dllexport) void record_probe(_Dcomplex *out, _Dcomplex x, _Dcomplex y) {
  *out = cpow(x, y);
}
)"
                                : R"(
#include "complex.h"
int _fltused;
__declspec(dllexport) _Dcomplex record_probe(_Dcomplex x, _Dcomplex y) {
  return cpow(x, y);
}
)";
    if (Chain)
      Fixture.replace(Fixture.find("cpow(x, y)"),
                      std::string("cpow(x, y)").size(), "cpow(cpow(x, y), y)");
    write(Input, Fixture);
    ASSERT_TRUE(execute(Compiler,
                        {Compiler, Target, "-O1", "-ffreestanding", "-c",
                         ProviderPath, "-o", ProviderObject},
                        Errors));
    ASSERT_TRUE(execute(*Linker,
                        {*Linker, "/dll", "/noentry", "/nodefaultlib",
                         ProviderDLL, ImportOption, ProviderObject},
                        Errors));
    auto Types = std::make_shared<types::TypeLibrary>(types::DataModel::LLP64);
    Types->setRecordABI(types::RecordABI::Microsoft);
    ASSERT_TRUE(types::parseCTypes(WindowsComplexHeader, *Types).ok());
    // The host provider observes the original bits, including signed zero and
    // NaN payloads. Sentinels prove the hidden buffer write is exactly 16
    // bytes.
    constexpr const char *Oracle = R"(
static unsigned calls;
_Dcomplex cpow(_Dcomplex x, _Dcomplex y) {
  uint64_t xb[2], yb[2], rb[2] = {0xFEDCBA9876543210ULL, 0x0123456789ABCDEFULL};
  __builtin_memcpy(xb, &x, 16); __builtin_memcpy(yb, &y, 16);
  if (xb[0] != (calls ? rb[0] : 0x7FF8123456789ABCULL) ||
      xb[1] != (calls ? rb[1] : 0x8000000000000000ULL) ||
      yb[0] != 0x3FF0000000000000ULL || yb[1] != 0xFFF8DEADBEEF0123ULL)
    __builtin_trap();
  ++calls;
  _Dcomplex r; __builtin_memcpy(&r, rb, 16); return r;
}
int main(void) {
  uint64_t x[2] = {0x7FF8123456789ABCULL, 0x8000000000000000ULL};
  uint64_t y[2] = {0x3FF0000000000000ULL, 0xFFF8DEADBEEF0123ULL};
  uint64_t out[4] = {0x1122334455667788ULL, 0, 0, 0x8877665544332211ULL};
  uintptr_t result = record_probe((uintptr_t)&out[1], x, y);
  return result != (uintptr_t)&out[1] || calls != EXPECTED_CALLS ||
      out[0] != 0x1122334455667788ULL || out[3] != 0x8877665544332211ULL ||
      out[1] != 0xFEDCBA9876543210ULL || out[2] != 0x0123456789ABCDEFULL;
}
)";
    for (const char *Optimization : {"-O0", "-O2"}) {
      SCOPED_TRACE(Optimization);
      ASSERT_TRUE(execute(Compiler,
                          {Compiler, Target, Optimization, "-ffreestanding",
                           "-fno-stack-protector", "-c", Input, "-o", Object},
                          Errors));
      ASSERT_TRUE(execute(*Linker,
                          {*Linker, "/dll", "/noentry", "/nodefaultlib",
                           DLLOption, Object, ImportLibrary},
                          Errors));
      auto Reader = Loader::create(DLL);
      ASSERT_TRUE(bool(Reader));
      auto Loaded = Reader->load(DLL);
      ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
      BinaryImage Image = std::move(*Loaded);
      EmptyDebug Empty;
      sdk::LocalTypesDebugContext Context(Empty, Types, {}, &Image);
      DeclaredHeaderDebug Debug(Context);
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM ? "LLVM-C" : "HighC");
        llvm::LLVMContext LLVMContext;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.LiftMode = LLVM;
        Options.SourceProjection = LLVM;
        for (const auto &Export : Image.Exports)
          if (Export.Name == "record_probe")
            Options.OnlyFunctionEntries.insert(Export.Addr);
        ASSERT_EQ(Options.OnlyFunctionEntries.size(), 1u);
        auto Result = Pipeline().run(Image, LLVMContext, Options, &Debug);
        ASSERT_TRUE(Result.Success) << Result.Error;
        CEmitterOptions COptions;
        COptions.TheArch = Image.Arch;
        COptions.Format = Image.Format;
        COptions.Image = &Image;
        COptions.RecordDefinitions = [&](const std::vector<TypeRef> &Records) {
          return Context.definitions(Records);
        };
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
        std::string NativeOracle = Oracle;
        if (ARM64) {
          NativeOracle.resize(NativeOracle.find("int main(void)"));
          NativeOracle += R"(
int main(void) {
  uint64_t input[4] = {0x7FF8123456789ABCULL, 0x8000000000000000ULL,
                       0x3FF0000000000000ULL, 0xFFF8DEADBEEF0123ULL};
  typedef uint64_t carrier __attribute__((vector_size(16)));
  carrier vectors[4] = {{0,0},{0,0},{0,0},{0,0}};
  double scalars[4];
  for (unsigned i = 0; i < 4; ++i) {
    __builtin_memcpy(&vectors[i], &input[i], 8);
    __builtin_memcpy(&scalars[i], &input[i], 8);
  }
  uint64_t output[4] = {0x1122334455667788ULL, 0, 0, 0x8877665544332211ULL};
  record_probe()";
          NativeOracle += Source.find("record_probe(void*") == std::string::npos
                              ? "(uintptr_t)&output[1], "
                              : "(void*)&output[1], ";
          NativeOracle +=
              LLVM ? "vectors[0], vectors[1], vectors[2], vectors[3]"
                   : "scalars[0], scalars[1], scalars[2], scalars[3]";
          NativeOracle += R"();
  return calls != EXPECTED_CALLS || output[1] != 0xFEDCBA9876543210ULL ||
         output[2] != 0x0123456789ABCDEFULL ||
         output[0] != 0x1122334455667788ULL || output[3] != 0x8877665544332211ULL;
}
)";
        }
        write(Generated, Source + "\n#define EXPECTED_CALLS " +
                             (Chain ? "2" : "1") + "\n" + NativeOracle);
        for (const char *COptimization : {"-O0", "-O2"}) {
          SCOPED_TRACE(COptimization);
          ASSERT_TRUE(execute(Compiler,
                              {Compiler, "-std=gnu11", COptimization,
                               "-fno-builtin", "-fsanitize=address,undefined",
                               Include, Generated, "-o", Product},
                              Errors))
              << Source;
          ASSERT_TRUE(execute(Product, {Product}, Errors)) << Source;
        }
      }
    }
  }
#endif
}

TEST(RecordCallContract,
     WindowsStackedHFAsPreserveGroupsAndFollowingArguments) {
#if !defined(__linux__) || !defined(__x86_64__)
  GTEST_SKIP() << "Host execution of the generated C requires Linux x86-64";
#else
  const std::string Compiler = NEVERD_TEST_CLANG;
  const auto Linker = llvm::sys::findProgramByName("lld-link");
  if (!Linker)
    GTEST_SKIP() << "lld-link is required for the linked ARM64 PE fixture";
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-stacked-hfa", Directory));
  const auto Cleanup = llvm::make_scope_exit(
      [&] { llvm::sys::fs::remove_directories(Directory); });
  const std::string Root = Directory.str().str();
  const std::string Header = Root + "/hfa.h", Input = Root + "/input.c",
                    Provider = Root + "/provider.c",
                    Object = Root + "/input.obj",
                    ProviderObject = Root + "/provider.obj",
                    ImportLibrary = Root + "/provider.lib",
                    ProviderDLL = "/out:" + Root + "/provider.dll",
                    ImportOption = "/implib:" + ImportLibrary,
                    DLL = Root + "/input.dll", DLLOption = "/out:" + DLL,
                    Generated = Root + "/generated.c",
                    Oracle = Root + "/oracle.c", Product = Root + "/run",
                    Include = "-I" + Root, Errors = Root + "/errors.txt";
  constexpr const char *Target = "--target=aarch64-pc-windows-msvc";
  for (bool Double : {false, true}) {
    for (unsigned Count : {1u, 2u, 3u, 4u}) {
      SCOPED_TRACE(Count);
      SCOPED_TRACE(Double ? "double HFA" : "float HFA");
      const std::string Scalar = Double ? "double" : "float";
      const std::string Declaration =
          "typedef struct HFA { " + Scalar + " lane[" + std::to_string(Count) +
          "]; } HFA;\nHFA fixture_hfa(" + Scalar + ", " + Scalar + ", " +
          Scalar + ", " + Scalar + ", " + Scalar + ", " + Scalar + ", " +
          Scalar + ", HFA, " + Scalar + ", unsigned, HFA);\n";
      write(Header, Declaration);
      write(Provider,
            "#include \"hfa.h\"\nint _fltused;\n__declspec(dllexport) "
            "HFA fixture_hfa(" +
                Scalar + " a, " + Scalar + " b, " + Scalar + " c, " + Scalar +
                " d, " + Scalar + " e, " + Scalar + " f, " + Scalar +
                " g, HFA x, " + Scalar +
                " h, unsigned n, HFA y) { return x; }\n");
      ASSERT_TRUE(execute(Compiler,
                          {Compiler, Target, "-O2", "-ffreestanding", "-c",
                           Provider, "-o", ProviderObject},
                          Errors));
      ASSERT_TRUE(execute(*Linker,
                          {*Linker, "/dll", "/noentry", "/nodefaultlib",
                           ProviderDLL, ImportOption, ProviderObject},
                          Errors));
      auto Types =
          std::make_shared<types::TypeLibrary>(types::DataModel::LLP64);
      Types->setRecordABI(types::RecordABI::Microsoft);
      ASSERT_TRUE(types::parseCTypes(Declaration, *Types).ok());
      const std::string Word = Double ? "unsigned long long" : "unsigned";
      const std::string X0 = Double ? "0x7FF8123456789ABCULL" : "0x7FC12345U";
      const std::string X1 = Double ? "0x8000000000000000ULL" : "0x80000000U";
      const std::string Y0 = Double ? "0x3FF0000000000000ULL" : "0x3F800000U";
      const std::string Y1 = Double ? "0xFFF8DEADBEEF0123ULL" : "0xFFC54321U";
      const std::string R0 = Double ? "0xFEDCBA9876543210ULL" : "0xBF123456U";
      const std::string R1 = Double ? "0x0123456789ABCDEFULL" : "0x40123456U";
      std::vector<std::string> X{X0, X1, Y1, R0}, Y{Y0, Y1, X0, R1},
          R{R0, R1, X1, Y0};
      const auto List = [&](const std::vector<std::string> &Values) {
        std::string Text;
        for (unsigned I = 0; I < Count; ++I)
          Text += (I ? ", " : "") + Values[I];
        return Text;
      };
      std::string Checks;
      for (unsigned I = 0; I < Count; ++I) {
        const auto Index = std::to_string(I);
        Checks += " || xb[" + Index + "] != (calls ? rb[" + Index +
                  "] : " + X[I] + ") || yb[" + Index + "] != " + Y[I];
      }
      write(Oracle,
            "#include \"hfa.h\"\nstatic unsigned calls;\nHFA fixture_hfa(" +
                Scalar + " a, " + Scalar + " b, " + Scalar + " c, " + Scalar +
                " d, " + Scalar + " e, " + Scalar + " f, " + Scalar +
                " g, HFA x, " + Scalar + " h, unsigned n, HFA y) {\n" + Word +
                " xb[" + std::to_string(Count) + "], yb[" +
                std::to_string(Count) + "], rb[] = {" + List(R) + "};\n" +
                "__builtin_memcpy(xb, &x, sizeof(x));\n" +
                "__builtin_memcpy(yb, &y, sizeof(y));\n" +
                "if (a != 1 || b != 2 || c != 3 || d != 4 || e != 5 || f != 6 "
                "|| g != 7 || " +
                "h != 9 || n != 0x12345678U" + Checks +
                ") __builtin_trap();\n" +
                "++calls; HFA r; __builtin_memcpy(&r, rb, sizeof(r)); return "
                "r; }\n" +
                "unsigned observed_calls(void) { return calls; }\n");
      for (bool Chain : {false, true}) {
        SCOPED_TRACE(Chain);
        const std::string Call = "fixture_hfa(1,2,3,4,5,6,7,x,9,0x12345678U,y)";
        write(Input, "#include \"hfa.h\"\nint _fltused;\n"
                     "__declspec(dllexport) void record_probe(HFA *out) {\n" +
                         Word + " xb[] = {" + List(X) + "}, yb[] = {" +
                         List(Y) +
                         "}; HFA x,y; __builtin_memcpy(&x,xb,sizeof(x));\n" +
                         "__builtin_memcpy(&y,yb,sizeof(y));\n" +
                         (Chain ? "x = " + Call + ";\n" : "") +
                         "*out = " + Call + "; }\n");
        for (const char *Optimization : {"-O0", "-O2"}) {
          SCOPED_TRACE(Optimization);
          ASSERT_TRUE(
              execute(Compiler,
                      {Compiler, Target, Optimization, "-ffreestanding",
                       "-fno-stack-protector", "-c", Input, "-o", Object},
                      Errors));
          ASSERT_TRUE(execute(*Linker,
                              {*Linker, "/dll", "/noentry", "/nodefaultlib",
                               DLLOption, Object, ImportLibrary},
                              Errors));
          auto Reader = Loader::create(DLL);
          ASSERT_TRUE(bool(Reader));
          auto Loaded = Reader->load(DLL);
          ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
          BinaryImage Image = std::move(*Loaded);
          EmptyDebug Empty;
          sdk::LocalTypesDebugContext Context(Empty, Types, {}, &Image);
          DeclaredHeaderDebug Debug(Context);
          for (bool LLVM : {false, true}) {
            SCOPED_TRACE(LLVM ? "LLVM-C" : "HighC");
            llvm::LLVMContext LLVMContext;
            PipelineOptions Options;
            Options.EmitDumpOutput = false;
            Options.LiftMode = LLVM;
            Options.SourceProjection = LLVM;
            for (const auto &Export : Image.Exports)
              if (Export.Name == "record_probe")
                Options.OnlyFunctionEntries.insert(Export.Addr);
            ASSERT_EQ(Options.OnlyFunctionEntries.size(), 1u);
            auto Result = Pipeline().run(Image, LLVMContext, Options, &Debug);
            ASSERT_TRUE(Result.Success) << Result.Error;
            unsigned BoundCalls = 0;
            for (const auto &Function : Result.MedFuncs)
              for (const auto &Block : Function.Blocks)
                for (const auto &Op : Block.Ops) {
                  if (!Op.ScalarCallSignature)
                    continue;
                  ++BoundCalls;
                  ASSERT_TRUE(
                      boundFloatingCallArguments(Op, Image.Arch, Image.Format));
                  auto Wrong = std::make_shared<FloatingCallArguments>(
                      *Op.ScalarCallSignature);
                  Wrong->Scalars[8].StackOffset += 4;
                  MedOp Broken = Op;
                  Broken.ScalarCallSignature = Wrong;
                  EXPECT_FALSE(boundFloatingCallArguments(Broken, Image.Arch,
                                                          Image.Format));
                  *Wrong = *Op.ScalarCallSignature;
                  Wrong->ParameterElements[7] = Count == 1 ? 2 : 1;
                  EXPECT_FALSE(boundFloatingCallArguments(Broken, Image.Arch,
                                                          Image.Format));
                }
            ASSERT_EQ(BoundCalls, Chain ? 2u : 1u);
            CEmitterOptions COptions;
            COptions.TheArch = Image.Arch;
            COptions.Format = Image.Format;
            COptions.Image = &Image;
            COptions.RecordDefinitions =
                [&](const std::vector<TypeRef> &Records) {
                  return Context.definitions(Records);
                };
            std::string Source;
            llvm::raw_string_ostream Out(Source);
            if (LLVM) {
              ASSERT_TRUE(Result.LlvmModule);
              const auto *Callee =
                  Result.LlvmModule->getFunction("fixture_hfa");
              ASSERT_NE(Callee, nullptr);
              ASSERT_EQ(Callee->arg_size(), 11u);
              EXPECT_TRUE(isAAPCS64HFAArray(
                  Callee->getFunctionType()->getParamType(7)));
              EXPECT_TRUE(isAAPCS64HFAArray(
                  Callee->getFunctionType()->getParamType(10)));
              ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Out, COptions,
                                              &Debug, &Image));
            } else {
              ASSERT_TRUE(
                  HighCEmitter().emit(Result.HighFuncs, Out, COptions, &Debug));
            }
            const std::string Pointer =
                Source.find("record_probe(void*") == std::string::npos
                    ? "(uintptr_t)"
                    : "(void*)";
            std::string OutputChecks =
                "out[0] != 23 || out[" + std::to_string(Count + 1) + "] != 79";
            for (unsigned I = 0; I < Count; ++I)
              OutputChecks +=
                  " || out[" + std::to_string(I + 1) + "] != " + R[I];
            write(Generated,
                  Source +
                      "\nextern unsigned observed_calls(void);\nint main(void) "
                      "{\n" +
                      Word + " out[" + std::to_string(Count + 2) +
                      "] = {23}; out[" + std::to_string(Count + 1) +
                      "] = 79; record_probe(" + Pointer + "&out[1]);\n" +
                      "return observed_calls() != " + (Chain ? "2" : "1") +
                      " || " + OutputChecks + "; }\n");
            for (const char *COptimization : {"-O0", "-O2"}) {
              SCOPED_TRACE(COptimization);
              ASSERT_TRUE(
                  execute(Compiler,
                          {Compiler, "-std=gnu11", COptimization,
                           "-fno-builtin", "-fsanitize=address,undefined",
                           Include, Generated, Oracle, "-o", Product},
                          Errors))
                  << Source;
              ASSERT_TRUE(execute(Product, {Product}, Errors)) << Source;
            }
          }
        }
      }
    }
  }
#endif
}
} // namespace
