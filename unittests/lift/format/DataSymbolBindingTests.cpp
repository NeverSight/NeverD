//===- DataSymbolBindingTests.cpp - Named dynamic data pointers -----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
#include "NeverDLiftFixture.h"

#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/loader/DataSymbolBinding.h"
#include "neverd/object/ELFLayout.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/support/BinaryLoading.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"

#include <cctype>
#include <cstring>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace neverd;
namespace {
#if defined(__linux__) && defined(__x86_64__)
constexpr bool CanRunX64ELF = true;
#else
constexpr bool CanRunX64ELF = false;
#endif

class DataSymbolBindingTest : public NeverDLiftTest {
protected:
  fs::path library(llvm::StringRef Target, bool Relro = true,
                   bool StandardIO = false, bool Sectionless = false,
                   bool LoopCall = false) {
    auto Source = tmpFile(Target.str() + ".c");
    auto Object = tmpFile(Target.str() + ".o");
    auto Library = tmpFile(Target.str() + ".so");
    std::ofstream Out(Source);
    Out << "extern int shared_counter; int counter_def = 5;\n"
           "int bump(void) { return ++shared_counter; }\n"
           "int readdef(void) { return counter_def; }\n";
    if (StandardIO)
      Out << "#include <stdio.h>\n"
             "int put(const char *s) { return fputs(s, stdout); }\n";
    if (LoopCall)
      Out << "extern int abs(int);\n"
             "__attribute__((no_builtin(\"abs\"))) int call_loop(void) {\n"
             "  int result=0;\n"
             "  for(int i=0;i<4;++i) result += abs(shared_counter+i);\n"
             "  return result;\n}\n";
    Out.close();
    auto Compile =
        exec(NEVERD_TEST_CLANG, {"--target=" + Target.str(), "-fPIC", "-O1",
                                 "-c", Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Compile.ok()) << Compile.err;
    // A portable immutable slot must fit the largest supported Arm page,
    // including when this cross-target fixture is built on an x86 host.
    const auto Page = std::to_string(kELFArmMaxProtectionPageSize);
    auto Link = exec("ld.lld",
                     {"-shared", "-z", Relro ? "relro" : "norelro", "-z",
                      "max-page-size=" + Page, "-z", "common-page-size=" + Page,
                      Object.string(), "-o", Library.string()});
    EXPECT_TRUE(Link.ok()) << Link.err;
    if (Sectionless) {
      std::ifstream Input(Library, std::ios::binary);
      std::vector<char> Bytes(std::istreambuf_iterator<char>(Input), {});
      Input.close();
      auto Strip = [&]<typename Header>() {
        Header H;
        std::memcpy(&H, Bytes.data(), sizeof(H));
        H.e_shoff = 0;
        H.e_shnum = 0;
        H.e_shstrndx = 0;
        std::memcpy(Bytes.data(), &H, sizeof(H));
      };
      if (Bytes[llvm::ELF::EI_CLASS] == llvm::ELF::ELFCLASS64)
        Strip.template operator()<llvm::ELF::Elf64_Ehdr>();
      else
        Strip.template operator()<llvm::ELF::Elf32_Ehdr>();
      std::ofstream Output(Library, std::ios::binary | std::ios::trunc);
      Output.write(Bytes.data(), Bytes.size());
    }
    return Library;
  }

  /// The shared library \p Name built from \p Code for \p Target, linked
  /// against the libraries \p Inputs names.
  fs::path sharedLibrary(llvm::StringRef Target, llvm::StringRef Name,
                         llvm::StringRef Code,
                         std::vector<std::string> Inputs = {}) {
    auto Source = tmpFile(Target.str() + "-" + Name.str() + ".c");
    auto Object = tmpFile(Target.str() + "-" + Name.str() + ".o");
    auto Library = tmpFile(Target.str() + "-" + Name.str() + ".so");
    std::ofstream(Source) << Code.str();
    auto Compile =
        exec(NEVERD_TEST_CLANG,
             {"--target=" + Target.str(), "-fPIC", "-O1", "-fno-builtin", "-c",
              Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Compile.ok()) << Compile.err;
    Inputs.insert(Inputs.begin(), Object.string());
    Inputs.insert(Inputs.begin(), "-shared");
    Inputs.push_back("-o");
    Inputs.push_back(Library.string());
    auto Link = exec("ld.lld", Inputs);
    EXPECT_TRUE(Link.ok()) << Link.err;
    return Library;
  }

  // Pointer variables a library starts at `pick`, which a second library
  // defines, so the dynamic symbol is an undefined function.  The calls are
  // not tail calls, and neither passes an argument: what they test is which
  // function they reach.
  fs::path hookLibrary(llvm::StringRef Target) {
    const auto Pick =
        sharedLibrary(Target, "pick", "int pick(void) { return 5; }\n");
    return sharedLibrary(
        Target, "hook",
        "int pick(void);\n"
        "static int (*hook)(void) = pick;\n"
        "static int (*volatile spare)(void) = pick;\n"
        "int call_hook(void) { return hook() + 1; }\n"
        "int call_spare(void) { return spare() + 1; }\n"
        "void set_hooks(int (*f)(void)) { hook = f; spare = f; }\n",
        {Pick.string()});
  }

  // Pointer variables a library starts at routines the prototype list knows
  // (a stub library stands in for libc) and at one of its own functions, each
  // called with the arguments its caller received; `plain` starts at a
  // routine nothing describes.
  fs::path argumentLibrary(llvm::StringRef Target) {
    const auto Stub = sharedLibrary(
        Target, "cstub",
        "int puts(const char *s) { return s ? 1 : 0; }\n"
        "int strcmp(const char *a, const char *b) { return a != b; }\n"
        "void exit(int code) { for (;;) (void)code; }\n"
        "int plain_target(int v) { return -v; }\n");
    return sharedLibrary(
        Target, "arguments",
        "int puts(const char *);\n"
        "int strcmp(const char *, const char *);\n"
        "void exit(int);\n"
        "int plain_target(int);\n"
        "static void (*volatile sink)(int) = exit;\n"
        "static int (*volatile put)(const char *) = puts;\n"
        "static int (*volatile cmp)(const char *, const char *) = strcmp;\n"
        "static int (*volatile plain)(int) = plain_target;\n"
        "static __attribute__((noinline)) int add2(int a, int b) {\n"
        "  return a + 3 * b;\n"
        "}\n"
        "static int (*volatile local_hook)(int, int) = add2;\n"
        "static __attribute__((noinline)) int take4(int a, int b, int c,\n"
        "                                           void *p) {\n"
        "  return a + b + c + (p != 0);\n"
        "}\n"
        "static int (*self_hook)(int, int, int, void *) = take4;\n"
        "__attribute__((noinline)) void die(int code) { sink(code); }\n"
        "__attribute__((noinline)) int say(const char *s) { return put(s); }\n"
        "__attribute__((noinline)) int same(const char *a, const char *b) {\n"
        "  return cmp(a, b) == 0;\n"
        "}\n"
        "__attribute__((noinline)) int tail_local(int a, int b) {\n"
        "  return local_hook(a, b);\n"
        "}\n"
        "__attribute__((noinline)) int call_local(int a, int b) {\n"
        "  return local_hook(a, b) + 1;\n"
        "}\n"
        "__attribute__((noinline)) int call_plain(int v) {\n"
        "  return plain(v) + 1;\n"
        "}\n"
        "__attribute__((noinline)) int call_self(int a, int b, int c) {\n"
        "  return self_hook(a, b, c, (void *)self_hook) + 1;\n"
        "}\n"
        "void set_hooks(int (*f)(int, int)) { local_hook = f; }\n"
        "void set_self(int (*f)(int, int, int, void *)) { self_hook = f; }\n",
        {Stub.string()});
  }

  // Variadic calls made after a join of two calls, which leave their scratch
  // registers behind: to a routine of the library whose save area starts
  // past its named parameter, to an import nothing describes, and as a tail
  // call that sets every argument register.
  fs::path variadicLibrary(llvm::StringRef Target) {
    const auto Stub =
        sharedLibrary(Target, "vstub",
                      "int puts(const char *s) { return s != 0; }\n"
                      "int vprintf(const char *f, __builtin_va_list ap) {\n"
                      "  (void)ap;\n"
                      "  return f != 0;\n"
                      "}\n"
                      "int hook(const char *s) { return s == 0; }\n"
                      "int ext_note(const char *f, ...) { return f != 0; }\n");
    return sharedLibrary(
        Target, "variadic",
        "int puts(const char *);\n"
        "int vprintf(const char *, __builtin_va_list);\n"
        "int hook(const char *);\n"
        "int ext_note(const char *, ...);\n"
        "__attribute__((noinline, visibility(\"hidden\")))\n"
        "int note(const char *fmt, ...) {\n"
        "  __builtin_va_list ap;\n"
        "  __builtin_va_start(ap, fmt);\n"
        "  int n = vprintf(fmt, ap);\n"
        "  __builtin_va_end(ap);\n"
        "  return n;\n"
        "}\n"
        "__attribute__((noinline)) int fmt_only(int c, const char *s) {\n"
        "  if (c) puts(s); else hook(s);\n"
        "  return note(\"plain\\n\");\n"
        "}\n"
        "__attribute__((noinline)) int fmt_one(int c, const char *s) {\n"
        "  if (c) puts(s); else hook(s);\n"
        "  return note(\"%d\\n\", c);\n"
        "}\n"
        "__attribute__((noinline)) int fmt_import(int c, const char *s) {\n"
        "  if (c) puts(s); else hook(s);\n"
        "  return ext_note(\"plain\\n\");\n"
        "}\n"
        "__attribute__((noinline)) int fwd3(int c) {\n"
        "  return note(\"%d %d %d\\n\", c, c + 1, c + 2);\n"
        "}\n",
        {Stub.string()});
  }

  // A routine that reads four argument registers, and callers that declare
  // it with one, so a call sets only the first: after another call, after a
  // join of two calls, and in a destructor that runs on both a normal and an
  // exceptional path.  The routine has a unit of its own, where the callers'
  // compiler cannot see that it reads the others and set them.
  fs::path leftoverLibrary(llvm::StringRef Target) {
    const std::string Prefix = Target.str() + "-leftover";
    const auto Callee = tmpFile(Prefix + "-reads4.c");
    const auto Callers = tmpFile(Prefix + "-callers.cpp");
    const auto CalleeObject = tmpFile(Prefix + "-reads4.o");
    const auto CallersObject = tmpFile(Prefix + "-callers.o");
    const auto Library = tmpFile(Prefix + ".so");
    std::ofstream(Callee) << "volatile long Sink;\n"
                             "__attribute__((visibility(\"hidden\")))\n"
                             "long reads4(long a, long b, long c, long d) {\n"
                             "  Sink = a + b + c + d;\n"
                             "  return Sink;\n"
                             "}\n";
    std::ofstream(Callers)
        << "extern \"C\" {\n"
           "__attribute__((visibility(\"hidden\"))) long reads4(long);\n"
           "void step(int);\n"
           "void other(int);\n"
           "void thrower(int);\n"
           "__attribute__((noinline)) long after_call(long v) {\n"
           "  step(1);\n"
           "  return reads4(v) + 1;\n"
           "}\n"
           "__attribute__((noinline)) long after_branch(int c, long v) {\n"
           "  if (c)\n"
           "    step(1);\n"
           "  else\n"
           "    other(2);\n"
           "  return reads4(v) + 1;\n"
           "}\n"
           "}\n"
           "struct Guard {\n"
           "  long V;\n"
           "  ~Guard() { reads4(V); }\n"
           "};\n"
           "extern \"C\" __attribute__((noinline)) long guarded(long v) {\n"
           "  Guard G{v};\n"
           "  thrower(1);\n"
           "  return 0;\n"
           "}\n";
    for (const auto &[Source, Object] :
         {std::pair{Callee, CalleeObject}, std::pair{Callers, CallersObject}}) {
      auto Compile =
          exec(NEVERD_TEST_CLANG,
               {"--target=" + Target.str(), "-fPIC", "-O1", "-fexceptions",
                "-c", Source.string(), "-o", Object.string()});
      EXPECT_TRUE(Compile.ok()) << Compile.err;
    }
    auto Link = exec("ld.lld", {"-shared", CallersObject.string(),
                                CalleeObject.string(), "-o", Library.string()});
    EXPECT_TRUE(Link.ok()) << Link.err;
    return Library;
  }

  // An image's own copy of the unwinder's _Unwind_Resume, whose body reads
  // argument registers no caller sets, and a routine that calls it after
  // another call.  libgcc's saves the exception-data registers as it enters
  // for the __builtin_eh_return that ends it (x0-x3, RAX and RDX); LLVM
  // libunwind's passes them on to __unw_getcontext, which stores them all.
  fs::path unwinderLibrary(llvm::StringRef Target) {
    if (Target.starts_with("aarch64"))
      return assembledLibrary(Target, "unwinder", R"(.text
.globl _Unwind_Resume
.hidden _Unwind_Resume
.type _Unwind_Resume,%function
_Unwind_Resume:
  stp x29, x30, [sp, #-64]!
  mov x29, sp
  stp x19, x20, [sp, #16]
  stp x0, x1, [sp, #32]
  stp x2, x3, [sp, #48]
  mov x19, x0
  add x1, x29, #64
  bl install
  mov x4, x0
  ldp x0, x1, [sp, #32]
  ldp x2, x3, [sp, #48]
  ldp x19, x20, [sp, #16]
  ldp x29, x30, [sp], #64
  add sp, sp, x4
  ret
.size _Unwind_Resume, .-_Unwind_Resume
.globl resume_after
.type resume_after,%function
resume_after:
  stp x29, x30, [sp, #-32]!
  mov x29, sp
  str x19, [sp, #16]
  mov x19, x0
  mov w0, #1
  bl step
  mov x0, x19
  bl _Unwind_Resume
.size resume_after, .-resume_after
)");
    if (Target.starts_with("armv7"))
      return assembledLibrary(Target, "unwinder", R"(.syntax unified
.arm
.text
.globl __unw_getcontext
.hidden __unw_getcontext
.type __unw_getcontext,%function
__unw_getcontext:
  stm r0, {r0-r12}
  str sp, [r0, #52]
  str lr, [r0, #56]
  str lr, [r0, #60]
  mov r0, #0
  bx lr
.size __unw_getcontext, .-__unw_getcontext
.globl _Unwind_Resume
.hidden _Unwind_Resume
.type _Unwind_Resume,%function
_Unwind_Resume:
  push {r4, lr}
  sub sp, sp, #64
  mov r4, r0
  mov r0, sp
  bl __unw_getcontext
  mov r0, r4
  mov r1, sp
  bl phase2
  add sp, sp, #64
  pop {r4, pc}
.size _Unwind_Resume, .-_Unwind_Resume
.globl resume_after
.type resume_after,%function
resume_after:
  push {r4, lr}
  mov r4, r0
  mov r0, #1
  bl step
  mov r0, r4
  bl _Unwind_Resume
  pop {r4, pc}
.size resume_after, .-resume_after
)");
    return sharedLibrary(
        Target, "unwinder",
        "long install(void *exc, void *frame);\n"
        "void step(int);\n"
        "__attribute__((noinline, visibility(\"hidden\")))\n"
        "void _Unwind_Resume(void *exc) {\n"
        "  __builtin_unwind_init();\n"
        "  long offset = install(exc, __builtin_frame_address(0));\n"
        "  __builtin_eh_return(offset, __builtin_return_address(0));\n"
        "}\n"
        "__attribute__((noinline)) void resume_after(void *exc) {\n"
        "  step(1);\n"
        "  _Unwind_Resume(exc);\n"
        "}\n");
  }

  /// The shared library \p Name assembled from \p Assembly for \p Target.
  fs::path assembledLibrary(llvm::StringRef Target, llvm::StringRef Name,
                            llvm::StringRef Assembly) {
    auto Source = tmpFile(Target.str() + "-" + Name.str() + ".s");
    auto Object = tmpFile(Target.str() + "-" + Name.str() + ".o");
    auto Library = tmpFile(Target.str() + "-" + Name.str() + ".so");
    std::ofstream(Source) << Assembly.str();
    auto Assemble =
        exec(NEVERD_TEST_CLANG, {"--target=" + Target.str(), "-c",
                                 Source.string(), "-o", Object.string()});
    EXPECT_TRUE(Assemble.ok()) << Assemble.err;
    auto Link =
        exec("ld.lld", {"-shared", Object.string(), "-o", Library.string()});
    EXPECT_TRUE(Link.ok()) << Link.err;
    return Library;
  }

  /// The definition of \p Name in \p C, through its last statement.
  static std::string functionBody(const std::string &C,
                                  const std::string &Name) {
    const std::string Needle = " " + Name + "(";
    for (size_t At = C.find(Needle); At != std::string::npos;
         At = C.find(Needle, At + 1)) {
      const size_t Close = C.find(')', At);
      if (Close != std::string::npos && C.compare(Close, 3, ") {") == 0)
        return C.substr(At, C.find("\n}", Close) - At);
    }
    return {};
  }

  /// The parameter names of the definition \p Body begins with.
  static std::vector<std::string> parameterNames(const std::string &Body) {
    const size_t Open = Body.find('(');
    const size_t Close = Body.find(')');
    llvm::SmallVector<llvm::StringRef> Parts;
    llvm::StringRef(Body).slice(Open + 1, Close).split(Parts, ',');
    std::vector<std::string> Names;
    for (llvm::StringRef Part : Parts) {
      Part = Part.trim();
      if (!Part.empty() && Part != "void")
        Names.push_back(Part.rsplit(' ').second.ltrim('*').str());
    }
    return Names;
  }

  /// How many arguments each call of \p Callee in \p Body passes, in order.
  static std::vector<int> callArgumentCounts(const std::string &Body,
                                             const std::string &Callee) {
    std::vector<int> Counts;
    for (size_t At = Body.find(Callee + "("); At != std::string::npos;
         At = Body.find(Callee + "(", At + 1)) {
      if (At > 0 && (std::isalnum(static_cast<unsigned char>(Body[At - 1])) ||
                     Body[At - 1] == '_'))
        continue;
      int Depth = 0, Count = 0;
      bool Quoted = false, Any = false;
      for (size_t I = At + Callee.size(); I < Body.size(); ++I) {
        const char C = Body[I];
        if (Quoted) {
          if (C == '\\')
            ++I;
          else if (C == '"')
            Quoted = false;
          continue;
        }
        if (C == '"')
          Quoted = true;
        else if (C == '(' && ++Depth == 1)
          continue;
        else if (C == ')' && --Depth == 0) {
          Counts.push_back(Any ? Count + 1 : 0);
          break;
        } else if (C == ',' && Depth == 1)
          ++Count;
        if (Depth >= 1 && !std::isspace(static_cast<unsigned char>(C)))
          Any = true;
      }
    }
    return Counts;
  }

  std::string source(const BinaryImage &Image, bool LLVM,
                     std::initializer_list<const char *> Functions,
                     bool Only = false) {
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.SourceProjection = LLVM;
    Options.LiftMode = LLVM;
    Options.EmitDumpOutput = false;
    for (const auto *Name : Functions) {
      auto *Symbol = Image.findSymbol(Name);
      EXPECT_NE(Symbol, nullptr);
      if (Symbol)
        Options.OnlyFunctionEntries.insert(Symbol->Addr);
    }
    auto Result = Pipeline().run(Image, Context, Options);
    EXPECT_TRUE(Result.Success) << Result.Error;
    CEmitterOptions Emit;
    Emit.TheArch = Image.Arch;
    Emit.Format = Image.Format;
    Emit.Image = &Image;
    Emit.EmitIncludes = Image.Arch == Arch::X64;
    std::string C = Emit.EmitIncludes ? "" : "#include <stdint.h>\n";
    llvm::raw_string_ostream OS(C);
    if (LLVM) {
      EXPECT_NE(Result.LlvmModule, nullptr);
      if (Result.LlvmModule) {
        const llvm::Function *Function = nullptr;
        if (Only)
          for (const auto &F : *Result.LlvmModule)
            if (!F.isDeclaration()) {
              Function = &F;
              break;
            }
        EXPECT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, OS, Emit, nullptr,
                                        &Image, Function));
      }
    } else {
      EXPECT_EQ(Result.HighFuncs.size(), Functions.size());
      EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Emit));
    }
    return C;
  }
};

TEST_F(DataSymbolBindingTest, PICObjectsKeepTheirIdentityAcrossArchitectures) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  for (const char *Target : {"x86_64-linux-gnu", "i386-linux-gnu",
                             "armv7-linux-gnueabi", "aarch64-linux-gnu"}) {
    SCOPED_TRACE(Target);
    for (bool Sectionless : {false, true}) {
      SCOPED_TRACE(Sectionless);
      auto Loaded = loadBinary(library(Target, true, false, Sectionless));
      ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
      const auto Bindings = collectDataSymbolBindings(*Loaded);
      ASSERT_EQ(Bindings.size(), 2u);
      for (const auto &[Slot, Binding] : Bindings) {
        EXPECT_TRUE(Binding.Immutable);
        EXPECT_EQ(Binding.Definition.has_value(),
                  Binding.Name == "counter_def");
      }
      for (bool LLVM : {false, true}) {
        SCOPED_TRACE(LLVM);
        const auto C = source(*Loaded, LLVM, {"bump", "readdef"});
        EXPECT_NE(C.find("shared_counter"), std::string::npos) << C;
        // LLVM may fold a private rebuilt definition whose only use is a read.
        EXPECT_TRUE(C.find("counter_def") != std::string::npos ||
                    (LLVM && C.find("return 5;") != std::string::npos))
            << C;
        EXPECT_EQ(C.find("unknown value"), std::string::npos) << C;
        const auto Output =
            tmpFile(std::string(Target) + (LLVM ? "-llvm.c" : "-high.c"));
        std::ofstream(Output) << C;
        const auto Compile = exec(
            NEVERD_TEST_CLANG,
            {"--target=" + std::string(Target), "-ffreestanding",
             "-fno-strict-aliasing", "-Werror=implicit-function-declaration",
             "-c", Output.string(), "-o", Output.string() + ".o"});
        EXPECT_TRUE(Compile.ok()) << Compile.err << C;
      }
    }
  }
}

TEST_F(DataSymbolBindingTest, ExternalDataRunsAndDoesNotConflictWithHeaders) {
  if (!CanRunX64ELF)
    GTEST_SKIP() << "ELF stdio linkage execution requires a Linux x64 host";
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  auto Loaded = loadBinary(library("x86_64-linux-gnu", true, true));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const auto Harness = tmpFile("harness.c");
  std::ofstream(Harness)
      << "int shared_counter=40; int bump(void); int put(const char*);\n"
         "int main(void) { if(bump()!=41 || shared_counter!=41) return 1;"
         "return put(\"data-binding\\n\")<0; }\n";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto C = source(*Loaded, LLVM, {"bump", "put"});
    const auto Output = tmpFile(LLVM ? "llvm.c" : "high.c");
    std::ofstream(Output) << "#include <stdio.h>\n" << C;
    const auto Program = tmpFile(LLVM ? "llvm" : "high");
    auto Compile =
        exec(NEVERD_TEST_CLANG, {"-O2", "-fno-strict-aliasing", Output.string(),
                                 Harness.string(), "-o", Program.string()});
    ASSERT_TRUE(Compile.ok()) << Compile.err << C;
    const auto Run = exec(Program.string(), {});
    EXPECT_TRUE(Run.ok()) << Run.err;
    EXPECT_EQ(Run.out, "data-binding\n");
  }
  // Single-function LLVM C exports need the same declarations as a module.
  const auto Single = source(*Loaded, true, {"bump"}, true);
  EXPECT_NE(Single.find("extern unsigned char neverd_data_shared_counter[]"),
            std::string::npos)
      << Single;
}

TEST_F(DataSymbolBindingTest, PICLoopCallsKeepTheirStackArguments) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  auto Loaded = loadBinary(library("i386-linux-gnu", true, false, false, true));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const auto C = source(*Loaded, false, {"call_loop"});
  EXPECT_NE(C.find("shared_counter"), std::string::npos) << C;
  EXPECT_NE(C.find("abs("), std::string::npos) << C;
  EXPECT_EQ(C.find("unknown value"), std::string::npos) << C;
  EXPECT_EQ(C.find("__builtin_trap"), std::string::npos) << C;
  // PIC loads can populate the shared definition index before the loop's
  // stack PHIs are visited. The abs prototype verifies that its pushed
  // argument survives either lookup order.
  const auto Output = tmpFile("pic-loop.c");
  std::ofstream(Output) << "extern int abs(int);\n" << C;
  const auto Compile =
      exec(NEVERD_TEST_CLANG, {"--target=i386-linux-gnu", "-ffreestanding",
                               "-Werror=implicit-function-declaration", "-c",
                               Output.string(), "-o", Output.string() + ".o"});
  EXPECT_TRUE(Compile.ok()) << Compile.err << C;
}

TEST_F(DataSymbolBindingTest, WritableSlotsRetainStorageAndSymbolInitializer) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  for (const char *Target : {"x86_64-linux-gnu", "i386-linux-gnu",
                             "armv7-linux-gnueabi", "aarch64-linux-gnu"}) {
    SCOPED_TRACE(Target);
    auto Loaded = loadBinary(library(Target, false));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    for (const auto &[Slot, Binding] : collectDataSymbolBindings(*Loaded))
      EXPECT_FALSE(Binding.Immutable);
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      const auto C = source(*Loaded, LLVM, {"bump", "readdef"});
      if (!LLVM) {
        EXPECT_NE(C.find("got_shared_counter"), std::string::npos) << C;
        EXPECT_NE(C.find("= (uintptr_t)neverd_data_shared_counter"),
                  std::string::npos)
            << C;
      }
      const auto Output = tmpFile(LLVM ? "writable-llvm.c" : "writable-high.c");
      std::ofstream(Output) << C
                            << "\nint shared_counter=40;\n"
                               "int main(void){ return bump()!=41 || "
                               "shared_counter!=41 || readdef()!=5; }\n";
      const auto Program = tmpFile(LLVM ? "writable-llvm" : "writable-high");
      std::vector<std::string> Args = {"--target=" + std::string(Target),
                                       "-O2",
                                       "-ffreestanding",
                                       "-fno-strict-aliasing",
                                       Output.string(),
                                       "-o",
                                       Program.string()};
      const bool RunNative = CanRunX64ELF && Loaded->Arch == Arch::X64;
      if (!RunNative)
        Args.push_back("-c");
      else {
        Args.push_back("-fsanitize=undefined");
        Args.push_back("-fno-sanitize-recover=undefined");
      }
      auto Compile = exec(NEVERD_TEST_CLANG, Args);
      ASSERT_TRUE(Compile.ok()) << Compile.err << C;
      if (RunNative)
        EXPECT_TRUE(exec(Program.string(), {}).ok());
    }
  }
}

TEST_F(DataSymbolBindingTest, PointerVariablesStartAtExternalFunctions) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  for (const char *Target : {"x86_64-linux-gnu", "i386-linux-gnu",
                             "armv7-linux-gnueabi", "aarch64-linux-gnu"}) {
    SCOPED_TRACE(Target);
    auto Loaded = loadBinary(hookLibrary(Target));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    // The variables hold `pick` without being its import slots.
    const auto Pointers = collectExternalFunctionPointers(*Loaded);
    ASSERT_EQ(Pointers.size(), 2u);
    const auto Imports = Loaded->collectImportStorageSlots();
    for (const auto &[Slot, Name] : Pointers) {
      EXPECT_EQ(Name, "pick");
      EXPECT_EQ(Imports.Slots.count(Slot), 0u);
    }
    // Each call goes through what the variable holds then: `pick` at first,
    // and the function stored there afterwards.
    const auto C =
        source(*Loaded, true, {"call_hook", "call_spare", "set_hooks"});
    EXPECT_EQ(C.find("__builtin_trap"), std::string::npos) << C;
    const auto Output = tmpFile(std::string(Target) + "-hooks.c");
    std::ofstream(Output) << C;
    const auto Program = tmpFile(std::string(Target) + "-hooks");
    std::vector<std::string> Args = {"--target=" + std::string(Target), "-O2",
                                     "-ffreestanding", "-fno-strict-aliasing",
                                     Output.string()};
    const bool RunNative = CanRunX64ELF && Loaded->Arch == Arch::X64;
    if (RunNative) {
      const auto Harness = tmpFile("hooks-main.c");
      std::ofstream(Harness)
          << "int call_hook(void); int call_spare(void);\n"
             "void set_hooks(int (*)(void));\n"
             "int pick(void) { return 5; }\n"
             "static int other(void) { return 41; }\n"
             "int main(void) {\n"
             "  if (call_hook() != 6 || call_spare() != 6)\n"
             "    return 1;\n"
             "  set_hooks(other);\n"
             "  return call_hook() != 42 || call_spare() != 42;\n"
             "}\n";
      Args.push_back(Harness.string());
    } else {
      Args.push_back("-c");
    }
    Args.push_back("-o");
    Args.push_back(Program.string());
    auto Compile = exec(NEVERD_TEST_CLANG, Args);
    EXPECT_TRUE(Compile.ok()) << Compile.err << C;
    if (RunNative && Compile.ok())
      EXPECT_TRUE(exec(Program.string(), {}).ok()) << C;
  }
}

// A call through a function-pointer variable passes the arguments of the
// routine the variable starts at: a prototyped routine of another module, or
// one of this image's.  Its caller takes them as parameters and passes them on
// in order, in tail calls too, and no register that only holds a saved value,
// the return address or the call target is passed.
TEST_F(DataSymbolBindingTest, CallsThroughPointerVariablesPassTheirArguments) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  const std::pair<const char *, size_t> Passing[] = {{"die", 1},
                                                     {"say", 1},
                                                     {"same", 2},
                                                     {"tail_local", 2},
                                                     {"call_local", 2}};
  for (const char *Target : {"x86_64-linux-gnu", "i386-linux-gnu",
                             "armv7-linux-gnueabi", "aarch64-linux-gnu"}) {
    SCOPED_TRACE(Target);
    auto Loaded = loadBinary(argumentLibrary(Target));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      const auto C =
          source(*Loaded, LLVM,
                 {"die", "say", "same", "tail_local", "call_local",
                  "call_plain", "call_self", "set_hooks", "set_self"});
      for (const auto &[Name, Count] : Passing) {
        SCOPED_TRACE(Name);
        const std::string Body = functionBody(C, Name);
        ASSERT_FALSE(Body.empty()) << C;
        const std::vector<std::string> Parameters = parameterNames(Body);
        ASSERT_EQ(Parameters.size(), Count) << Body;
        EXPECT_NE(Body.find("(" + llvm::join(Parameters, ", ") + ")"),
                  std::string::npos)
            << Body;
      }
      // `call_self` passes the variable it calls through as the fourth
      // argument, which the routine takes, though it holds the target too.
      const std::string Self = functionBody(C, "call_self");
      ASSERT_FALSE(Self.empty()) << C;
      EXPECT_NE(Self.find("(" + llvm::join(parameterNames(Self), ", ") + ", "),
                std::string::npos)
          << Self;
      // Nothing describes `plain`'s routine; its call invents no argument.
      const std::string Plain = functionBody(C, "call_plain");
      ASSERT_FALSE(Plain.empty()) << C;
      EXPECT_EQ(Plain.find("(0)"), std::string::npos) << Plain;
      EXPECT_EQ(Plain.find("return_address"), std::string::npos) << Plain;
    }
  }
}

// LLVM declares a callee nothing describes from the module's calls.  Where
// they pass it different numbers of arguments, a declaration made from the
// first call's would cut a longer later call's.
TEST_F(DataSymbolBindingTest, UndescribedCalleesKeepEveryCallsArguments) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  for (const char *Target : {"x86_64-linux-gnu", "i386-linux-gnu"}) {
    SCOPED_TRACE(Target);
    const auto Stub =
        sharedLibrary(Target, "nstub",
                      "int ext_note(const char *f, ...) { return f != 0; }\n");
    auto Loaded = loadBinary(
        sharedLibrary(Target, "twice",
                      "int ext_note(const char *, ...);\n"
                      "__attribute__((noinline)) int twice(int c) {\n"
                      "  ext_note(\"start\\n\");\n"
                      "  return ext_note(\"%d %d\\n\", c, c + 1);\n"
                      "}\n",
                      {Stub.string()}));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    const auto C = source(*Loaded, /*LLVM=*/true, {"twice"});
    const std::string Body = functionBody(C, "twice");
    ASSERT_FALSE(Body.empty()) << C;
    EXPECT_EQ(callArgumentCounts(Body, "ext_note"), (std::vector<int>{1, 3}))
        << Body;
  }
}

// A variadic call passes what its caller sets, past its callee's named
// parameters: never a register that only holds what earlier calls left in it
// (which HighC would trap on before the call), nor, in a tail call, the
// caller's own incoming stack, for which a variadic routine's open arity
// says nothing.
TEST_F(DataSymbolBindingTest, VariadicCallsPassOnlyWhatTheirCallerSets) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  const std::tuple<const char *, const char *, int> Calls[] = {
      {"fmt_only", "note", 1},
      {"fmt_one", "note", 2},
      {"fmt_import", "ext_note", 1},
      {"fwd3", "note", 4}};
  for (const char *Target : {"x86_64-linux-gnu", "i386-linux-gnu",
                             "armv7-linux-gnueabi", "aarch64-linux-gnu"}) {
    SCOPED_TRACE(Target);
    auto Loaded = loadBinary(variadicLibrary(Target));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      const auto C =
          source(*Loaded, LLVM, {"fmt_only", "fmt_one", "fmt_import", "fwd3"});
      for (const auto &[Caller, Callee, Count] : Calls) {
        SCOPED_TRACE(Caller);
        const std::string Body = functionBody(C, Caller);
        ASSERT_FALSE(Body.empty()) << C;
        // Structuring may copy the tail call into each branch.
        const std::vector<int> Counts = callArgumentCounts(Body, Callee);
        EXPECT_FALSE(Counts.empty()) << Body;
        for (int Passed : Counts)
          EXPECT_EQ(Passed, Count) << Body;
        EXPECT_EQ(Body.find("__builtin_trap"), std::string::npos) << Body;
      }
    }
  }
}

// A variadic callee says nothing of the stack slots past its named
// parameters.  Where a register below them holds only what an earlier call
// left, they hold no argument either, but what -O0 code spilled there
// before the call (`str x0, [sp]`).
TEST_F(DataSymbolBindingTest, VariadicCallsTakeNoSpillAboveAnUnsetRegister) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  auto Loaded = loadBinary(assembledLibrary("aarch64-linux-gnu", "vspill",
                                            R"(.text
.globl note
.hidden note
.type note,%function
note:
  sub sp, sp, #288
  stp x29, x30, [sp, #256]
  str x28, [sp, #272]
  add x29, sp, #256
  mov x8, #-56
  mov x9, sp
  sub x10, x29, #120
  movk x8, #65408, lsl #32
  add x9, x9, #128
  stp x1, x2, [x29, #-120]
  stp x9, x8, [x29, #-16]
  add x8, x10, #56
  add x9, x29, #32
  stp x9, x8, [x29, #-32]
  sub x1, x29, #64
  stp x3, x4, [x29, #-104]
  stp q0, q1, [sp]
  ldur q0, [x29, #-32]
  ldur q1, [x29, #-16]
  stp x5, x6, [x29, #-88]
  stur x7, [x29, #-72]
  stp q2, q3, [sp, #32]
  stp q4, q5, [sp, #64]
  stp q6, q7, [sp, #96]
  stp q0, q1, [x29, #-64]
  bl vreport
  ldp x29, x30, [sp, #256]
  ldr x28, [sp, #272]
  add sp, sp, #288
  ret
.size note, .-note
.globl open_or_note
.type open_or_note,%function
open_or_note:
  sub sp, sp, #0x40
  stp x29, x30, [sp, #48]
  add x29, sp, #0x30
  adrp x9, fmt
  add x9, x9, :lo12:fmt
  stur x0, [x29, #-8]
  ldur x0, [x29, #-8]
  str x9, [sp, #24]
  bl openit
  stur x0, [x29, #-16]
  ldur x8, [x29, #-16]
  cbnz x8, 1f
  mov w0, #-1
  ldur x1, [x29, #-8]
  str x1, [sp, #8]
  bl errstr
  ldr x1, [sp, #24]
  str x0, [sp]
  mov x0, x1
  ldr x1, [sp, #8]
  ldr x2, [sp]
  bl note
1:
  ldur x0, [x29, #-16]
  ldp x29, x30, [sp, #48]
  add sp, sp, #0x40
  ret
.size open_or_note, .-open_or_note
.section .rodata
fmt:
  .asciz "Can't open '%s': %s"
)"));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto C = source(*Loaded, LLVM, {"note", "open_or_note"});
    const std::string Note = functionBody(C, "note");
    ASSERT_FALSE(Note.empty()) << C;
    // LLVM C spells the variadic tail `...`, which names no parameter.
    EXPECT_LE(
        llvm::count_if(parameterNames(Note),
                       [](const std::string &Name) { return !Name.empty(); }),
        8)
        << Note;
    const std::string Caller = functionBody(C, "open_or_note");
    ASSERT_FALSE(Caller.empty()) << C;
    const std::vector<int> Counts = callArgumentCounts(Caller, "note");
    EXPECT_FALSE(Counts.empty()) << Caller;
    for (int Passed : Counts)
      EXPECT_LE(Passed, 8) << Caller;
  }
}

// A call passes no register an earlier call may have changed as its caller's
// incoming value, though its callee reads it: the register holds what that
// call left, and the caller takes no parameter for it.  An exception landing
// pad, which the unwinder enters with the caller-saved registers
// unspecified, holds none of them either.
TEST_F(DataSymbolBindingTest, CallsTakeNoIncomingRegisterAnEarlierCallChanged) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  const std::pair<const char *, size_t> Callers[] = {
      {"after_call", 1}, {"after_branch", 2}, {"guarded", 1}};
  for (const char *Target : {"x86_64-linux-gnu", "i386-linux-gnu",
                             "armv7-linux-gnueabi", "aarch64-linux-gnu"}) {
    SCOPED_TRACE(Target);
    auto Loaded = loadBinary(leftoverLibrary(Target));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      const auto C =
          source(*Loaded, LLVM, {"after_call", "after_branch", "guarded"});
      for (const auto &[Caller, Params] : Callers) {
        SCOPED_TRACE(Caller);
        const std::string Body = functionBody(C, Caller);
        ASSERT_FALSE(Body.empty()) << C;
        EXPECT_EQ(parameterNames(Body).size(), Params) << Body;
      }
    }
  }
}

// An image's copy of a routine of the Itanium C++ ABI's runtime or unwinder
// takes the parameters the ABI declares, however its body reads argument
// registers: what _Unwind_Resume saves or passes on of them carries no
// argument of its callers.
TEST_F(DataSymbolBindingTest, ItaniumRuntimeCopiesTakeTheirDeclaredParameters) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  for (const char *Target :
       {"x86_64-linux-gnu", "armv7-linux-gnueabi", "aarch64-linux-gnu"}) {
    SCOPED_TRACE(Target);
    auto Loaded = loadBinary(unwinderLibrary(Target));
    ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
    for (bool LLVM : {false, true}) {
      SCOPED_TRACE(LLVM);
      const auto C = source(*Loaded, LLVM, {"_Unwind_Resume", "resume_after"});
      const std::string Resume = functionBody(C, "_Unwind_Resume");
      ASSERT_FALSE(Resume.empty()) << C;
      EXPECT_EQ(parameterNames(Resume).size(), 1u) << Resume;
      const std::string Caller = functionBody(C, "resume_after");
      ASSERT_FALSE(Caller.empty()) << C;
      EXPECT_EQ(callArgumentCounts(Caller, "_Unwind_Resume"),
                (std::vector<int>{1}))
          << Caller;
    }
  }
}

// A call whose first argument register holds only its target passes nothing
// there: neither the table slot the target was loaded through, as a C++
// vtable call's object would be, nor a stale write left before the loads.
// Nor does a call take its caller's prologue saves above a gap in its
// outgoing stores as arguments.
TEST_F(DataSymbolBindingTest, IndirectCallsPassNoTargetOrSavedRegisters) {
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  auto Loaded = loadBinary(assembledLibrary("armv7-linux-gnueabi", "targets",
                                            R"(.syntax unified
.arm
.text
.globl call_table
.type call_table,%function
call_table:
  push {r4, lr}
  ldr r0, .Lslot0
.Lpc0:
  add r0, pc, r0
  ldr r0, [r0]
  ldr r0, [r0, #24]
  blx r0
  pop {r4, pc}
.Lslot0:
  .word table_ptr - (.Lpc0 + 8)
.size call_table, .-call_table
.globl call_spills
.type call_spills,%function
call_spills:
  push {r4, r6, r7, lr}
  add r7, sp, #8
  sub sp, sp, #24
  mov r2, r1
  mov r3, r0
  str r0, [sp, #20]
  add r0, sp, #16
  strb r1, [r0]
  ldr r1, .Lslot1
.Lpc1:
  add r1, pc, r1
  ldr r1, [r1]
  str r0, [sp, #8]
  str r3, [sp, #4]
  str r2, [sp]
  blx r1
  add sp, sp, #24
  pop {r4, r6, r7, pc}
.Lslot1:
  .word hook - (.Lpc1 + 8)
.size call_spills, .-call_spills
.globl target_fn
.type target_fn,%function
target_fn:
  mov r0, #7
  bx lr
.size target_fn, .-target_fn
.data
.p2align 2
.globl table_ptr
.hidden table_ptr
table_ptr:
  .word ops
ops:
  .word 0, 0, 0, 0, 0, 0, target_fn
.globl hook
.hidden hook
hook:
  .word target_fn
)"));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto C = source(*Loaded, LLVM, {"call_table", "call_spills"});
    const std::string Table = functionBody(C, "call_table");
    ASSERT_FALSE(Table.empty()) << C;
    EXPECT_NE(Table.find("))()"), std::string::npos) << Table;
    const std::string Spills = functionBody(C, "call_spills");
    ASSERT_FALSE(Spills.empty()) << C;
    EXPECT_EQ(Spills.find("__builtin_trap"), std::string::npos) << Spills;
    EXPECT_EQ(Spills.find("unknown register"), std::string::npos) << Spills;
  }
}

// On the host the rebuilt calls reach the routines with their arguments.
TEST_F(DataSymbolBindingTest, PointerVariableCallsRunWithTheirArguments) {
  if (!CanRunX64ELF)
    GTEST_SKIP() << "ELF stdio linkage execution requires a Linux x64 host";
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  auto Loaded = loadBinary(argumentLibrary("x86_64-linux-gnu"));
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  // The program links `add2` and `take4`, which `local_hook` and `self_hook`
  // start at, too.
  const auto C = source(*Loaded, true,
                        {"die", "say", "same", "tail_local", "call_local",
                         "call_plain", "set_hooks", "add2", "take4"});
  const auto Output = tmpFile("arguments-llvm.c");
  std::ofstream(Output) << C;
  const auto Harness = tmpFile("arguments-main.c");
  std::ofstream(Harness)
      << "#include <stdio.h>\n"
         "int say(const char *); int same(const char *, const char *);\n"
         "int tail_local(int, int); int call_local(int, int);\n"
         "int call_plain(int); void set_hooks(int (*)(int, int));\n"
         "void die(int);\n"
         "int plain_target(int v) { return -v; }\n"
         "static int product(int a, int b) { return a * b; }\n"
         "int main(void) {\n"
         "  if (say(\"through put\") < 0) return 1;\n"
         "  if (!same(\"x\", \"x\") || same(\"x\", \"y\")) return 2;\n"
         "  if (tail_local(2, 5) != 17 || call_local(2, 5) != 18) return 3;\n"
         "  if (call_plain(4) != -3) return 4;\n"
         "  set_hooks(product);\n"
         "  if (tail_local(2, 5) != 10 || call_local(2, 5) != 11) return 5;\n"
         "  fflush(stdout);\n"
         "  die(7);\n"
         "  return 6;\n"
         "}\n";
  const auto Program = tmpFile("arguments");
  auto Compile =
      exec(NEVERD_TEST_CLANG, {"-O2", "-fno-strict-aliasing", Output.string(),
                               Harness.string(), "-o", Program.string()});
  ASSERT_TRUE(Compile.ok()) << Compile.err << C;
  const auto Run = exec(Program.string(), {});
  EXPECT_EQ(Run.exitCode, 7) << Run.err << C;
  EXPECT_EQ(Run.out, "through put\n");
}

TEST_F(DataSymbolBindingTest, OpaqueAliasesKeepByteLoadAndStoreSemantics) {
  if (!CanRunX64ELF)
    GTEST_SKIP() << "ELF alias linkage execution requires a Linux x64 host";
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires Clang";
  llvm::LLVMContext Context;
  llvm::SMDiagnostic Diagnostic;
  auto Module = llvm::parseAssemblyString(R"(
@stored_byte = external global i8, !neverd.data-symbol !0
define i8 @bump_byte() {
  %value = load i8, ptr @stored_byte
  %next = add i8 %value, 1
  store i8 %next, ptr @stored_byte
  ret i8 %next
}
!0 = !{}
)",
                                          Diagnostic, Context);
  ASSERT_TRUE(Module) << Diagnostic.getMessage().str();
  CEmitterOptions Emit;
  Emit.PreserveLLVMFunctionTypes = true;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  ASSERT_TRUE(LLVMCEmitter().emit(*Module, OS, Emit));
  const auto File = tmpFile("byte.c");
  std::ofstream(File)
      << Source
      << "\nunsigned char stored_byte=255;\n"
         "int main(void){return bump_byte()!=0 || stored_byte!=0;}\n";
  const auto Program = tmpFile("byte");
  auto Compile =
      exec(NEVERD_TEST_CLANG,
           {"-O2", "-fsanitize=undefined", "-fno-sanitize-recover=undefined",
            File.string(), "-o", Program.string()});
  ASSERT_TRUE(Compile.ok()) << Compile.err << Source;
  EXPECT_TRUE(exec(Program.string(), {}).ok());
}

TEST(DataSymbolBindings, ExactSymbolKindAddendAndPermissionsOwnTheBinding) {
  BinaryImage Image;
  Image.Format = BinaryFormat::ELF;
  Image.Arch = Arch::AArch64;
  Image.Bits = Bitness::Bits64;
  Segment Data;
  Data.VA = 0x10000 - 8;
  Data.Size = 32;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.assign(Data.Size, 0xa5); // File bytes are not a RELA addend.
  Image.Segments.push_back(Data);
  RelocationEntry Relocation;
  Relocation.Address = Data.VA;
  Relocation.Type = llvm::ELF::R_AARCH64_GLOB_DAT;
  Relocation.Addend = -8;
  Relocation.HasExplicitAddend = true;
  Relocation.SymbolName =
      "wrong_name"; // The exact r_sym record is authoritative.
  Relocation.ELF.emplace();
  auto &Symbol = Relocation.ELF->Symbol.emplace();
  Symbol.Name = "weak_data";
  Symbol.Binding = llvm::ELF::STB_WEAK;
  Symbol.Type = llvm::ELF::STT_OBJECT;
  Image.Relocations.push_back(Relocation);
  auto Binding = [&] { return collectDataSymbolBindings(Image).at(Data.VA); };
  EXPECT_EQ(Binding().Name, "weak_data");
  EXPECT_TRUE(Binding().Weak);
  EXPECT_FALSE(Binding().Definition);
  EXPECT_EQ(Binding().Addend, -8);
  EXPECT_FALSE(Binding().Immutable);
  Image.ELFMetadata.emplace();
  Image.ELFMetadata->Type = llvm::ELF::ET_DYN;
  Image.ELFMetadata->ProgramHeaders.push_back(
      {llvm::ELF::PT_GNU_RELRO, 0, 0, Data.VA, 7, 7, 1});
  EXPECT_FALSE(Binding().Immutable); // A partial pointer is not read-only.
  Image.ELFMetadata->ProgramHeaders.back().MemorySize = 8;
  EXPECT_TRUE(Binding().Immutable);
  for (unsigned Kind :
       {llvm::ELF::STT_FUNC, llvm::ELF::STT_GNU_IFUNC, llvm::ELF::STT_TLS}) {
    Image.Relocations[0].ELF->Symbol->Type = Kind;
    EXPECT_TRUE(collectDataSymbolBindings(Image).empty());
  }
  Image.Relocations[0].ELF->Symbol->Type = llvm::ELF::STT_NOTYPE;
  EXPECT_EQ(Binding().Name, "weak_data");
  Image.Relocations[0].HasExplicitAddend = false;
  EXPECT_TRUE(collectDataSymbolBindings(Image).empty());
  Image.Arch = Arch::X64;
  Image.Relocations[0].Type = llvm::ELF::R_X86_64_GLOB_DAT;
  EXPECT_EQ(Binding().Addend, 0); // x86 GLOB_DAT ignores the in-place bytes.
  Image.Relocations.push_back(Image.Relocations[0]);
  Image.Relocations.back().ELF->Symbol->Name = "conflicting_data";
  EXPECT_THROW(collectDataSymbolBindings(Image), std::invalid_argument);
}

TEST(DataSymbolBindings, ExternalFunctionPointersNeedAnExactAbsoluteSymbol) {
  struct Target {
    Arch TheArch;
    Bitness Bits;
    uint32_t Absolute;
    uint32_t Binding;
    bool Rela;
  };
  for (const Target &T :
       {Target{Arch::X64, Bitness::Bits64, llvm::ELF::R_X86_64_64,
               llvm::ELF::R_X86_64_GLOB_DAT, true},
        Target{Arch::X86, Bitness::Bits32, llvm::ELF::R_386_32,
               llvm::ELF::R_386_GLOB_DAT, false},
        Target{Arch::ARM, Bitness::Bits32, llvm::ELF::R_ARM_ABS32,
               llvm::ELF::R_ARM_GLOB_DAT, false},
        Target{Arch::AArch64, Bitness::Bits64, llvm::ELF::R_AARCH64_ABS64,
               llvm::ELF::R_AARCH64_GLOB_DAT, true}}) {
    SCOPED_TRACE(static_cast<int>(T.TheArch));
    BinaryImage Image;
    Image.Format = BinaryFormat::ELF;
    Image.Arch = T.TheArch;
    Image.Bits = T.Bits;
    Segment Data;
    Data.VA = 0x4000;
    Data.Size = 16;
    Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Data.Data.assign(Data.Size, 0);
    Image.Segments.push_back(Data);
    RelocationEntry Relocation;
    Relocation.Address = Data.VA;
    Relocation.Type = T.Absolute;
    Relocation.HasExplicitAddend = T.Rela;
    Relocation.ELF.emplace();
    auto &Symbol = Relocation.ELF->Symbol.emplace();
    Symbol.Name = "exit";
    Symbol.Binding = llvm::ELF::STB_GLOBAL;
    Symbol.Type = llvm::ELF::STT_FUNC;
    Image.Relocations.push_back(Relocation);
    auto &Record = Image.Relocations[0];
    auto &Word = Image.Segments[0].Data;
    const std::map<va_t, std::string> Exit{{Data.VA, "exit"}};
    EXPECT_EQ(collectExternalFunctionPointers(Image), Exit);

    // An addend points past the entry: RELA states it, REL keeps it in the
    // word.
    if (T.Rela)
      Record.Addend = 8;
    else
      Word[0] = 8;
    EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
    Record.Addend = 0;
    Word[0] = 0;
    if (T.Rela) {
      Word[0] = 0xa5; // RELA ignores the file's bytes.
      EXPECT_EQ(collectExternalFunctionPointers(Image), Exit);
      Word[0] = 0;
    }

    // A GOT binding is import storage, not a pointer variable.
    Record.Type = T.Binding;
    EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
    Record.Type = T.Absolute;

    // Data, untyped symbols, weak functions the loader may leave null, and
    // functions the file defines are not external functions.
    for (unsigned Type : {llvm::ELF::STT_OBJECT, llvm::ELF::STT_NOTYPE}) {
      Record.ELF->Symbol->Type = Type;
      EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
    }
    Record.ELF->Symbol->Type = llvm::ELF::STT_FUNC;
    Record.ELF->Symbol->Binding = llvm::ELF::STB_WEAK;
    EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
    Record.ELF->Symbol->Binding = llvm::ELF::STB_GLOBAL;
    Record.ELF->Symbol->SectionIndex = 1;
    EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
    Record.ELF->Symbol->SectionIndex = llvm::ELF::SHN_UNDEF;
    EXPECT_EQ(collectExternalFunctionPointers(Image), Exit);

    // A word two relocations start at different functions is no one's.
    RelocationEntry Abort = Record;
    Abort.ELF->Symbol->Name = "abort";
    Image.Relocations.push_back(std::move(Abort));
    EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
    Image.Relocations.pop_back();

    // A relocatable object's relocations are not the dynamic loader's.
    Image.IsRelocatable = true;
    EXPECT_TRUE(collectExternalFunctionPointers(Image).empty());
  }
}

// FILD of an integer lowers to integer operations that build the binary80
// exponent as 0x403E minus the leading zeros. In a shared library whose first,
// read-only segment at VA 0 spans 0x403E, that bias is still a number: LLVM C
// read it as an address into the library's own bytes.
TEST_F(DataSymbolBindingTest, IntegerX87LoadsKeepTheirExponentBias) {
  if (!CanRunX64ELF)
    GTEST_SKIP() << "ELF execution requires a Linux x64 host";
  if (!hasCrossTargetClang())
    GTEST_SKIP() << "requires cross-target Clang and ld.lld";
  const auto Library =
      sharedLibrary("x86_64-linux-gnu", "widen",
                    "const unsigned char blob[0x5000] = {1, 2, 3};\n"
                    "long double widen(long x) { return x; }\n");
  auto Loaded = loadBinary(Library);
  ASSERT_TRUE(bool(Loaded)) << llvm::toString(Loaded.takeError());
  const Segment *Seg = Loaded->getSegmentFor(0x403e);
  ASSERT_NE(Seg, nullptr);
  ASSERT_EQ(Seg->VA, 0u);
  const auto Harness = tmpFile("widen-main.c");
  std::ofstream(Harness)
      << "long double widen(long x);\n"
         "int main(void) {\n"
         "  return widen(0) != 0.0L || widen(-5) != -5.0L ||\n"
         "         widen(1L << 40) != 1099511627776.0L;\n"
         "}\n";
  for (bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const auto C = source(*Loaded, LLVM, {"widen"});
    EXPECT_EQ(C.find("unk_0"), std::string::npos) << C;
    const auto Output = tmpFile(LLVM ? "widen-llvm.c" : "widen-high.c");
    std::ofstream(Output) << C;
    const auto Program = tmpFile(LLVM ? "widen-llvm" : "widen-high");
    auto Compile =
        exec(NEVERD_TEST_CLANG, {"-O2", "-fno-strict-aliasing", Output.string(),
                                 Harness.string(), "-o", Program.string()});
    ASSERT_TRUE(Compile.ok()) << Compile.err << C;
    const auto Run = exec(Program.string(), {});
    EXPECT_TRUE(Run.ok()) << Run.err << C;
  }
}
} // namespace
