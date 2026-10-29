//===- OwnInteriorCallTests.cpp - Calls into a function's own body ------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Calls to labels inside a function's own unwind range can return normally
// or abandon their invocation. Each admitted return retains its physical PC
// ownership; an unproved internal or parent return refuses the function.
//
//===----------------------------------------------------------------------===//

#include "../../../lib/ir/low/LowScalarTargets.h"
#include "../../../lib/ir/low/X86/FrameEffects.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/ir/low/CFGBuilder.h"
#include "neverd/loader/ExceptionFunction.h"
#include "neverd/pipeline/Pipeline.h"
#include "neverd/pipeline/X64ContinuationExport.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Entry = 0x140001000;

/// The image a function's own unwind record comes from: its architecture and
/// format, the record's encoding as that format's loader produces it, and
/// where the function starts.
struct Platform {
  Arch TheArch;
  BinaryFormat Format;
  ExceptionEncoding Encoding;
  va_t Entry;
};

/// Microsoft x64, with the function's `.pdata` entry.
constexpr Platform Win64 = {Arch::X64, BinaryFormat::COFF,
                            ExceptionEncoding::X64UnwindV1, Entry};
/// System V x86-64, with the function's DWARF FDE.
constexpr Platform SysV64 = {Arch::X64, BinaryFormat::ELF,
                             ExceptionEncoding::DwarfFDE, Entry};
/// i386 ELF, with the function's DWARF FDE.
constexpr Platform I386 = {Arch::X86, BinaryFormat::ELF,
                           ExceptionEncoding::DwarfFDE, 0x8049000};
/// i386 PE, whose range is recovered from the `_except_handler3`
/// registration the prologue installs.
constexpr Platform I386Registration = {Arch::X86, BinaryFormat::COFF,
                                       ExceptionEncoding::X86ScopeTableEH3,
                                       0x401000};

BinaryImage makeImage(std::vector<uint8_t> Code, const Platform &P) {
  const va_t End = P.Entry + Code.size();
  BinaryImage Img;
  Img.Arch = P.TheArch;
  Img.Bits = P.TheArch == Arch::X86 ? Bitness::Bits32 : Bitness::Bits64;
  Img.Format = P.Format;
  Img.Base = P.Entry - 0x1000;
  Img.Entry = P.Entry;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = P.Entry;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  ExceptionFunction Own;
  Own.CodeRange = {P.Entry, End};
  Own.FunctionEntry = P.Entry;
  Own.Kind = RuntimeFunctionKind::Primary;
  Own.Encoding = P.Encoding;
  Img.ExceptionMetadata.Functions.push_back(Own);
  Img.ExceptionMetadata.rebuildIndex();
  Img.KnownCodeRanges.push_back({P.Entry, End});
  return Img;
}

struct Lifted {
  std::string Source;
  std::vector<va_t> Unsupported;
  std::vector<va_t> UnprovenReturns;
};

Lifted lift(std::vector<uint8_t> Code, const Platform &P = Win64) {
  BinaryImage Img = makeImage(std::move(Code), P);
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(P.Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  Lifted Out;
  for (const PipelineFunctionAudit &Audit : Result.FunctionAudits)
    if (Audit.Entry == P.Entry) {
      Out.Unsupported = Audit.UnsupportedInstructions;
      Out.UnprovenReturns = Audit.UnprovenReturns;
    }
  for (const HighFunc &Func : Result.HighFuncs) {
    if (Func.Entry != P.Entry)
      continue;
    llvm::raw_string_ostream OS(Out.Source);
    CEmitterOptions Options;
    Options.TheArch = Img.Arch;
    Options.Format = Img.Format;
    Options.Image = &Img;
    EXPECT_TRUE(HighCEmitter().emit({Func}, OS, Options));
  }
  return Out;
}

void compileAndRun(const std::string &Source, bool Loops = false) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> SourcePath, BinaryPath, ErrorPath;
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-own-call", "c", SourcePath));
  llvm::FileRemover RemoveSource(SourcePath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-own-call", "exe", BinaryPath));
  llvm::FileRemover RemoveBinary(BinaryPath);
  ASSERT_FALSE(
      llvm::sys::fs::createTemporaryFile("neverd-own-call", "err", ErrorPath));
  llvm::FileRemover RemoveError(ErrorPath);
  std::error_code EC;
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    ASSERT_FALSE(EC);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
  for (const char *Optimization : {"-O0", "-O2"}) {
    const llvm::SmallVector<llvm::StringRef, 12> Arguments{
        Compiler,
        "-std=c11",
        Optimization,
        "-fsanitize=undefined,address",
        "-Werror=uninitialized",
        "-Werror=return-type",
        SourcePath,
        "-o",
        BinaryPath};
    std::string Error;
    int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 60, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, Loops ? 1 : 30, 0, &Error);
    Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    if (Loops) {
      ASSERT_EQ(Result, -2) << Error << '\n' << Source;
      ASSERT_NE(Error.find("timed out"), std::string::npos) << Error << '\n'
                                                            << Source;
      continue;
    }
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
  }
}

constexpr const char *ReturnsSeven = R"(
int main(void) { return sub_140001000() == 7 ? 0 : 1; }
)";

void checkComputedSource(const BinaryImage &Image, const std::string &Harness,
                         bool Loops = false, const std::string &Prelude = {}) {
  for (bool LLVM : {false, true})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(LLVM);
      SCOPED_TRACE(NoOpt);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.LiftMode = LLVM;
      Options.SourceProjection = true;
      Options.NoOpt = NoOpt;
      Options.OnlyFunctionEntries = {Image.Entry};
      auto Result = Pipeline().run(Image, Context, Options);
      ASSERT_TRUE(Result.Success) << Result.Error;
      std::string Source;
      llvm::raw_string_ostream Output(Source);
      CEmitterOptions COptions;
      COptions.TheArch = Image.Arch;
      COptions.Format = Image.Format;
      COptions.Image = &Image;
      if (LLVM) {
        ASSERT_TRUE(Result.LlvmModule);
        ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
        ASSERT_TRUE(LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
      } else
        ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, COptions));
      compileAndRun(Prelude + Source + Harness, Loops);
    }
}

TEST(NativeTimestampSource, CounterOnlyKeepsTheAuxiliaryOperandValid) {
  const auto Image = makeImage({0x0f, 0x01, 0xf9,       // rdtscp
                                0x48, 0xc1, 0xe2, 0x20, // shl rdx,32
                                0x48, 0x09, 0xd0, 0xc3},
                               SysV64); // or rax,rdx; ret
  checkComputedSource(Image, R"C(
int main(void) {
  _mm_lfence();
  uint64_t before=__rdtsc();
  _mm_lfence();
  uint64_t value=sub_140001000();
  _mm_lfence();
  uint64_t after=__rdtsc();
  return value<before || value>after;
}
)C");
}

TEST(SourceCallReturnWrites, ActualNextPCSurvivesAnOrdinarySourceCall) {
  // The second load observes the physical CALL's pushed next PC, not the
  // earlier explicit constant and not the source compiler's own return PC.
  std::vector<uint8_t> Code = {0x48, 0x83, 0xec, 0x28, // sub rsp,40
                               0x48, 0xc7, 0x44, 0x24, 0xf8,
                               19,   0,    0,    0,           // mov [rsp-8],19
                               0xe8, 0x2e, 0,    0,    0,     // call +64
                               0x48, 0x8b, 0x44, 0x24, 0xf8,  // mov rax,[rsp-8]
                               0x48, 0x83, 0xc4, 0x28, 0xc3}; // add rsp,40; ret
  const auto End = Entry + Code.size();
  Code.resize(64, 0xcc);
  Code.insert(Code.end(), {0xb8, 7, 0, 0, 0, 0xc3});
  auto Image = makeImage(Code, Win64);
  Image.ExceptionMetadata.Functions[0].CodeRange.End = End;
  Image.ExceptionMetadata.rebuildIndex();
  auto Callee = Symbol::makeFunc(Entry + 64);
  Callee.Name = "return_slot_helper";
  Image.Symbols.push_back(Callee);
  checkComputedSource(Image, R"C(
static unsigned calls;
__typeof__(return_slot_helper()) return_slot_helper(void) { ++calls; return 7; }
int main(void) {
  return (uint64_t)sub_140001000() != 0x140001012ULL || calls != 1;
}
)C");
}

TEST(SourceCallReturnWrites, PartialOutputKeepsAnEarlierPhysicalReturnPC) {
  std::vector<uint8_t> Code = {
      0x48, 0x83, 0xec, 0x28,        // sub rsp,40
      0xe8, 0x37, 0,    0,    0,     // call +64; return PC +9
      0x48, 0x83, 0xec, 0x10,        // sub rsp,16
      0x48, 0x8d, 0x4c, 0x24, 8,     // lea rcx,[rsp+8]
      0xe8, 0x39, 0,    0,    0,     // call +80
      0x48, 0x8b, 0x44, 0x24, 8,     // mov rax,[rsp+8]
      0x48, 0x83, 0xc4, 0x38, 0xc3}; // add rsp,56; ret
  const auto End = Entry + Code.size();
  Code.resize(64, 0xcc);
  Code.insert(Code.end(), {0xb8, 7, 0, 0, 0, 0xc3});
  Code.resize(80, 0xcc);
  Code.insert(Code.end(),
              {0xc7, 0x01, 0x78, 0x56, 0x34, 0x12, // mov dword[rcx],...
               0xb8, 7, 0, 0, 0, 0xc3});
  auto Image = makeImage(Code, Win64);
  Image.ExceptionMetadata.Functions[0].CodeRange.End = End;
  Image.ExceptionMetadata.rebuildIndex();
  for (const auto &[Offset, Name] : {std::pair{64, "return_slot_helper"},
                                     std::pair{80, "partial_output_helper"}}) {
    auto Callee = Symbol::makeFunc(Entry + Offset);
    Callee.Name = Name;
    Image.Symbols.push_back(Callee);
  }
  checkComputedSource(Image, R"C(
static unsigned calls;
__typeof__(return_slot_helper()) return_slot_helper(void) { ++calls; return 7; }
__typeof__(partial_output_helper(0)) partial_output_helper(void *output) {
  ++calls;
  uint32_t word = 0x12345678;
  __builtin_memcpy(output, &word, sizeof(word));
  return 7;
}
int main(void) {
  return (uint64_t)sub_140001000() != 0x112345678ULL || calls != 2;
}
)C");
}

TEST(WindowsKernelABI, ResumeThreadDoesNotTakeTheDispatchersAmbientRegisters) {
  std::vector<uint8_t> Code = {0x48, 0x83, 0xec, 0x28,    // sub rsp,40
                               0xba, 7,    0,    0,    0, // mov edx,7
                               0xe8, 0x12, 0,    0,    0, // call +32
                               0x48, 0x83, 0xc4, 0x28, 0xc3};
  Code.resize(32, 0xcc);
  // A service implementation can observe ambient registers. Its declared
  // two-argument interface still owns the caller's source operands.
  Code.insert(Code.end(), {0x4c, 0x89, 0x01,       // mov [rcx],r8
                           0x4c, 0x89, 0x49, 8,    // mov [rcx+8],r9
                           0x48, 0x8d, 0x04, 0x11, // lea rax,[rcx+rdx]
                           0xc3});
  auto Image = makeImage(Code, Win64);
  Symbol Service = Symbol::makeFunc(Entry + 32);
  Service.Name = "ZwResumeThread";
  Image.Symbols.push_back(Service);
  checkComputedSource(Image, R"(
int64_t ZwResumeThread(int64_t handle, int64_t previous) {
  return handle + previous * 3;
}
uint64_t sub_140001020(uint64_t handle, uint64_t previous) {
  return handle + previous * 3;
}
int main(void) { return sub_140001000(11) != 32; }
)",
                      false,
                      "#include <stdint.h>\n"
                      "int64_t ZwResumeThread(int64_t, int64_t);\n");
}

TEST(WindowsKernelABI, NativeServiceKeepsItsDeclaredStackArgument) {
  std::vector<uint8_t> Code = {
      0x48, 0x83, 0xec, 0x28,                    // sub rsp,40
      0xb9, 11,   0,    0,    0,                 // mov ecx,11
      0xba, 22,   0,    0,    0,                 // mov edx,22
      0x41, 0xb8, 33,   0,    0,    0,           // mov r8d,33
      0x41, 0xb9, 44,   0,    0,    0,           // mov r9d,44
      0x48, 0xc7, 0x44, 0x24, 32,                // mov qword [rsp+32],55
      55,   0,    0,    0,    0xe8, 24, 0, 0, 0, // call +64
      0x48, 0x83, 0xc4, 0x28, 0xc3};
  Code.resize(64, 0xcc);
  Code.insert(Code.end(), {0x31, 0xc0, 0xc3});
  auto Image = makeImage(Code, Win64);
  Symbol Service = Symbol::makeFunc(Entry + 64);
  Service.Name = "ZwQueryLicenseValue";
  Image.Symbols.push_back(Service);
  checkComputedSource(Image, R"(
int64_t ZwQueryLicenseValue(int64_t a, int64_t b, int64_t c,
                           uint32_t d, int64_t e) {
  return a + 2*b + 3*c + 4*d + 5*e;
}
uint64_t sub_140001040(uint64_t a, uint64_t b, uint64_t c,
                     uint32_t d, uint64_t e) {
  return a + 2*b + 3*c + 4*d + 5*e;
}
int main(void) { return sub_140001000() != 605; }
)",
                      false,
                      "#include <stdint.h>\n"
                      "int64_t ZwQueryLicenseValue(int64_t, int64_t, int64_t, "
                      "uint32_t, int64_t);\n");
}

TEST(WindowsKernelABI, RegisterOnlyServiceDoesNotTakeAdjacentFrameStores) {
  std::vector<uint8_t> Code = {
      0x48, 0x83, 0xec, 0x38,                   // sub rsp,56
      0xb9, 11,   0,    0,    0,                // mov ecx,11
      0xba, 22,   0,    0,    0,                // mov edx,22
      0x4c, 0x8d, 0x44, 0x24, 32,               // lea r8,[rsp+32]
      0x41, 0xb9, 44,   1,    0,   0,           // mov r9d,300 (BOOLEAN 44)
      0x48, 0xc7, 0x44, 0x24, 32,  55, 0, 0, 0, // mov qword [rsp+32],55
      0x48, 0xc7, 0x44, 0x24, 40,  66, 0, 0, 0, // mov qword [rsp+40],66
      0xe8, 48,   0,    0,    0,                // call +96
      0x48, 0x83, 0xc4, 0x38, 0xc3};
  Code.resize(96, 0xcc);
  Code.insert(Code.end(), {0x31, 0xc0, 0xc3});
  auto Image = makeImage(Code, Win64);
  Symbol Service = Symbol::makeFunc(Entry + 96);
  Service.Name = "ZwCreateMutant";
  Image.Symbols.push_back(Service);
  checkComputedSource(Image, R"(
int64_t ZwCreateMutant(int64_t a, uint32_t b, int64_t object, uint32_t d) {
  const uint64_t *p = (const uint64_t *)(uintptr_t)object;
  return a + 2*b + 3*p[0] + 4*d + 5*p[1];
}
uint64_t sub_140001060(uint64_t a, uint32_t b, uint64_t object, uint8_t d) {
  return ZwCreateMutant(a, b, object, d);
}
int main(void) { return sub_140001000() != 726; }
)",
                      false,
                      "#include <stdint.h>\n"
                      "int64_t ZwCreateMutant(int64_t, uint32_t, int64_t, "
                      "uint32_t);\n");
}

TEST(WindowsKernelABI, EmptyServiceContractDoesNotAcquireAmbientOperands) {
  std::vector<uint8_t> Code = {0x48, 0x83, 0xec, 0x28,    // sub rsp,40
                               0xb9, 13,   0,    0,    0, // mov ecx,13
                               0x48, 0xc7, 0x44, 0x24, 32,
                               55,   0,    0,    0, // mov qword [rsp+32],55
                               0xe8, 41,   0,    0,    0, // call +64
                               0x48, 0x83, 0xc4, 0x28, 0xc3};
  Code.resize(64, 0xcc);
  Code.insert(Code.end(), {0x48, 0x89, 0xc8, 0xc3}); // mov rax,rcx; ret
  auto Image = makeImage(Code, Win64);
  Symbol Service = Symbol::makeFunc(Entry + 64);
  Service.Name = "KeGetCurrentThread";
  Image.Symbols.push_back(Service);
  checkComputedSource(Image, R"(
int64_t KeGetCurrentThread(void) { return 7; }
uint64_t sub_140001040(void) { return 7; }
int main(void) { return sub_140001000() != 7; }
)",
                      false,
                      "#include <stdint.h>\n"
                      "int64_t KeGetCurrentThread(void);\n");
}

TEST(WindowsKernelABI, WdkServiceBoundsAmbientReadsThroughAnOrdinaryCaller) {
  // The native service's body observes R9. Its three-argument WDK contract
  // must also bound the summary of an ordinary wrapper; otherwise a caller
  // of that wrapper acquires an unavailable fourth argument transitively.
  std::vector<uint8_t> Code = {0x48, 0x83, 0xec, 0x28,         // sub rsp,40
                               0xba, 22,   0,    0,    0,      // mov edx,22
                               0x41, 0xb8, 33,   0,    0,   0, // mov r8d,33
                               0xe8, 44,   0,    0,    0,      // call +64
                               0x48, 0x83, 0xc4, 0x28, 0xc3};
  Code.resize(64, 0xcc);
  Code.insert(Code.end(), {0x48, 0x83, 0xec, 0x28, // sub rsp,40
                           0xe8, 55, 0, 0, 0,      // call +128
                           0x48, 0x83, 0xc4, 0x28, 0xc3});
  Code.resize(128, 0xcc);
  Code.insert(Code.end(), {0x4c, 0x89, 0xc8, 0xc3}); // mov rax,r9; ret
  auto Image = makeImage(Code, Win64);
  Image.Symbols.push_back(Symbol::makeFunc(Entry + 64));
  Symbol Service = Symbol::makeFunc(Entry + 128);
  Service.Name = "ZwSetSecurityObject";
  Image.Symbols.push_back(Service);
  checkComputedSource(Image, R"(
/* The two emitters differ in this untyped wrapper's return signedness. */
__typeof__(sub_140001040(0, 0, 0))
sub_140001040(uint64_t handle, uint32_t information, uint64_t descriptor) {
  return handle + 2 * information + 3 * descriptor;
}
int main(void) { return sub_140001000(11) != 154; }
)");
}

TEST(WindowsKernelABI, WdkServiceKeepsItsNarrowFirstParameter) {
  std::vector<uint8_t> Code = {
      0x48, 0x83, 0xec, 0x28,    // sub rsp,40
      0xb9, 11,   1,    0,    0, // mov ecx,267 (BOOLEAN 11)
      0xba, 22,   0,    0,    0, // mov edx,22
      0xe8, 45,   0,    0,    0, // call +64
      0x48, 0x83, 0xc4, 0x28, 0xc3};
  Code.resize(64, 0xcc);
  Code.insert(Code.end(), {0x4c, 0x89, 0xc8, 0xc3}); // mov rax,r9; ret
  auto Image = makeImage(Code, Win64);
  Symbol Service = Symbol::makeFunc(Entry + 64);
  Service.Name = "ZwQueryDefaultLocale";
  Image.Symbols.push_back(Service);
  checkComputedSource(Image, R"(
int64_t ZwQueryDefaultLocale(uint32_t user, int64_t locale) {
  return user + 3 * locale;
}
uint64_t sub_140001040(uint8_t user, uint64_t locale) {
  return user + 3 * locale;
}
int main(void) { return sub_140001000() != 77; }
)",
                      false,
                      "#include <stdint.h>\n"
                      "int64_t ZwQueryDefaultLocale(uint32_t, int64_t);\n");
}

TEST(WindowsKernelABI, WdkServiceKeepsNarrowAndStackArgumentsTogether) {
  std::vector<uint8_t> Code = {
      0x48, 0x83, 0xec, 0x28,                   // sub rsp,40
      0xb9, 11,   0,    0,    0,                // mov ecx,11
      0xba, 22,   0,    0,    0,                // mov edx,22
      0x41, 0xb8, 33,   1,    0,   0,           // mov r8d,289 (BOOLEAN 33)
      0x41, 0xb9, 44,   0,    0,   0,           // mov r9d,44
      0x48, 0xc7, 0x44, 0x24, 32,  55, 0, 0, 0, // mov qword [rsp+32],55
      0xe8, 24,   0,    0,    0,                // call +64
      0x48, 0x83, 0xc4, 0x28, 0xc3};
  Code.resize(64, 0xcc);
  Code.insert(Code.end(), {0x31, 0xc0, 0xc3});
  auto Image = makeImage(Code, Win64);
  Symbol Service = Symbol::makeFunc(Entry + 64);
  Service.Name = "ZwOpenThreadTokenEx";
  Image.Symbols.push_back(Service);
  checkComputedSource(
      Image, R"(
int64_t ZwOpenThreadTokenEx(int64_t a, uint32_t b, uint32_t c,
                          uint32_t d, int64_t e) {
  return a + 2*b + 3*c + 4*d + 5*e;
}
uint64_t sub_140001040(uint64_t a, uint32_t b, uint8_t c,
                     uint32_t d, uint64_t e) {
  return a + 2*b + 3*c + 4*d + 5*e;
}
int main(void) { return sub_140001000() != 605; }
)",
      false,
      "#include <stdint.h>\n"
      "int64_t ZwOpenThreadTokenEx(int64_t, uint32_t, uint32_t, "
      "uint32_t, int64_t);\n");
}

TEST(WindowsKernelABI, WdkZeroArgumentServiceDoesNotAcquireAmbientOperands) {
  std::vector<uint8_t> Code = {0x48, 0x83, 0xec, 0x28,    // sub rsp,40
                               0xb9, 13,   0,    0,    0, // mov ecx,13
                               0x48, 0xc7, 0x44, 0x24, 32,
                               55,   0,    0,    0, // mov qword [rsp+32],55
                               0xe8, 41,   0,    0,    0, // call +64
                               0x48, 0x83, 0xc4, 0x28, 0xc3};
  Code.resize(64, 0xcc);
  Code.insert(Code.end(), {0x48, 0x89, 0xc8, 0xc3}); // mov rax,rcx; ret
  auto Image = makeImage(Code, Win64);
  Symbol Service = Symbol::makeFunc(Entry + 64);
  Service.Name = "ZwYieldExecution";
  Image.Symbols.push_back(Service);
  checkComputedSource(Image, R"(
int64_t ZwYieldExecution(void) { return 7; }
uint64_t sub_140001040(void) { return 7; }
int main(void) { return sub_140001000() != 7; }
)",
                      false,
                      "#include <stdint.h>\n"
                      "int64_t ZwYieldExecution(void);\n");
}

TEST(ComputedCodeBranch, OverlappingInstructionStreamsKeepTheirFallthrough) {
  // The branch enters after the REX prefix. Both ADDs finish at +11; the
  // full-width path must not execute the overlapping 32-bit ADD as well.
  const std::vector<uint8_t> Code = {0x48, 0x89, 0xc8, // mov rax,rcx
                                     0x85, 0xd2,       // test edx,edx
                                     0x74, 0x01,       // je +8
                                     0x48, 0x83, 0xc0,
                                     0x01,  // add rax,1 / add eax,1
                                     0xc3}; // ret
  auto Image = makeImage(Code, Win64);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  auto F = CFGBuilder().build(Image, Dec, Entry);
  const auto *Wide = F.blockFor(Entry + 7);
  ASSERT_NE(Wide, nullptr);
  ASSERT_EQ(Wide->Succs.size(), 1u);
  EXPECT_EQ(F.Blocks[Wide->Succs[0]].StartAddr, Entry + 11);
  const auto *Narrow = F.blockFor(Entry + 8);
  ASSERT_NE(Narrow, nullptr);
  EXPECT_EQ(Narrow->StartAddr, Entry + 8);
  ASSERT_EQ(Narrow->Succs.size(), 1u);
  EXPECT_EQ(F.Blocks[Narrow->Succs[0]].StartAddr, Entry + 11);
  checkComputedSource(Image, R"(
int main(void) {
  return sub_140001000(0x100000000ULL, 1) != 0x100000001ULL ||
         sub_140001000(0x100000000ULL, 0) != 1;
}
)");
}

TEST(ComputedCodeBranch, MaskedArithmeticNeedsNoMemoryTable) {
  std::vector<uint8_t> Code = {0x89, 0xc8,       // mov eax,ecx
                               0x83, 0xe0, 0x03, // and eax,3
                               0xc1, 0xe0, 0x03, // shl eax,3
                               0x4c, 0x8d, 0x1d, 0x31,
                               0,    0,    0,     // lea r11,[+0x40]
                               0x49, 0x29, 0xc3,  // sub r11,rax
                               0x41, 0xff, 0xe3}; // jmp r11
  Code.resize(0x48, 0xcc);
  for (unsigned I = 0; I != 4; ++I) {
    const auto Offset = 0x40 - I * 8;
    Code[Offset] = 0xb8; // mov eax,I; ret
    Code[Offset + 1] = I;
    Code[Offset + 2] = Code[Offset + 3] = Code[Offset + 4] = 0;
    Code[Offset + 5] = 0xc3;
  }
  auto Image = makeImage(Code, Win64);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  auto F = CFGBuilder().build(Image, Dec, Entry);
  ASSERT_EQ(F.JumpTables.size(), 1u);
  EXPECT_EQ(F.JumpTables.front().Targets,
            (std::vector<va_t>{Entry + 0x28, Entry + 0x30, Entry + 0x38,
                               Entry + 0x40}));
  const std::string Harness = R"(
int main(void) {
  for (unsigned long long i = 0; i != 64; ++i)
    if (sub_140001000(i) != (i & 3)) return 1;
  return 0;
}
)";
  checkComputedSource(Image, Harness);
  // This arithmetic is independent of PE metadata and the incoming ABI.
  Code[1] = 0xf8; // mov eax,edi: first integer argument under System V
  for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::MachO}) {
    Platform P = SysV64;
    P.Format = Format;
    SCOPED_TRACE(static_cast<int>(Format));
    checkComputedSource(makeImage(Code, P), Harness);
  }
}

TEST(ComputedCodeBranch, DataAddressArithmeticUsesTheSameNumericCoordinate) {
  std::vector<uint8_t> Code = {
      0x89, 0xc8, 0x83, 0xe0, 3,    0xc1, 0xe0, 3,
      0x4c, 0x8d, 0x1d, 0x31, 0x10, 0,    0, // lea r11,[data+0x40]
      0x49, 0x29, 0xc3,                      // sub r11,rax
      0x49, 0x81, 0xeb, 0,    0x10, 0,    0, // sub r11,0x1000
      0x41, 0xff, 0xe3};
  Code.resize(0x48, 0xcc);
  for (unsigned I = 0; I != 4; ++I) {
    const auto Offset = 0x40 - I * 8;
    Code[Offset] = 0xb8;
    Code[Offset + 1] = I;
    Code[Offset + 2] = Code[Offset + 3] = Code[Offset + 4] = 0;
    Code[Offset + 5] = 0xc3;
  }
  auto Image = makeImage(Code, Win64);
  Segment Data;
  Data.Name = ".data";
  Data.VA = Entry + 0x1000;
  Data.Size = 0x80;
  Data.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Data.Data.resize(Data.Size);
  Image.Segments.push_back(std::move(Data));
  checkComputedSource(Image, R"(
int main(void) {
  for (unsigned i = 0; i != 64; ++i)
    if (sub_140001000(i) != (i & 3)) return 1;
  return 0;
}
)");
}

TEST(ComputedCodeBranch, I386ArithmeticKeepsItsTargetWidth) {
  std::vector<uint8_t> Code = {0x8b, 0x44, 0x24, 4, // mov eax,[esp+4]
                               0x83, 0xe0, 3,    0xc1, 0xe0,
                               3, // and eax,3; shl eax,3
                               0xba, 0,    0,    0,    0, // mov edx,base
                               0x29, 0xc2, 0xff, 0xe2}; // sub edx,eax; jmp edx
  for (unsigned I = 0; I != 4; ++I)
    Code[11 + I] = uint8_t((I386.Entry + 0x40) >> (I * 8));
  Code.resize(0x48, 0xcc);
  for (unsigned I = 0; I != 4; ++I) {
    const auto Offset = 0x40 - I * 8;
    Code[Offset] = 0xb8;
    Code[Offset + 1] = I;
    Code[Offset + 2] = Code[Offset + 3] = Code[Offset + 4] = 0;
    Code[Offset + 5] = 0xc3;
  }
  auto Image = makeImage(Code, I386);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  const auto F = CFGBuilder().build(Image, Dec, I386.Entry);
  ASSERT_EQ(F.JumpTables.size(), 1u);
  EXPECT_EQ(F.JumpTables[0].SelectorUseRefs[0].ExpectedSize, 4u);
  checkComputedSource(Image, R"(
int main(void) {
  for (unsigned i = 0; i != 64; ++i)
    if (sub_8049000(i) != (i & 3)) return 1;
  return 0;
}
)");
}

TEST(ComputedCodeBranch, EveryMaskedValueIncludesTheOverlappingLoop) {
  std::vector<uint8_t> Code = {
      0x89, 0xc8, 0x83, 0xe0, 15, 0xc1, 0xe0, 3,
      0x4c, 0x8d, 0x1d, 0x79, 0,  0,    0, // lea r11,[+0x88]
      0x49, 0x29, 0xc3,                    // sub r11,rax; +16: sub ebx,eax
      0x41, 0xff, 0xe3};                   // +18: jmp r11
  Code.resize(0x90, 0xcc);
  for (unsigned I = 0; I != 15; ++I) {
    const auto Offset = 0x88 - I * 8;
    Code[Offset] = 0xb8;
    Code[Offset + 1] = I;
    Code[Offset + 2] = Code[Offset + 3] = Code[Offset + 4] = 0;
    Code[Offset + 5] = 0xc3;
  }
  auto Image = makeImage(Code, Win64);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  auto F = CFGBuilder().build(Image, Dec, Entry);
  ASSERT_EQ(F.JumpTables.size(), 1u);
  EXPECT_EQ(F.JumpTables[0].Targets.size(), 16u);
  EXPECT_EQ(F.JumpTables[0].Targets.front(), Entry + 16);
  const auto *Loop = F.blockFor(Entry + 16);
  ASSERT_NE(Loop, nullptr);
  EXPECT_EQ(Loop->StartAddr, Entry + 16);
  ASSERT_EQ(Loop->Succs.size(), 1u);
  EXPECT_EQ(F.Blocks[Loop->Succs[0]].StartAddr, Entry + 18);
  checkComputedSource(Image, R"(
int main(void) {
  for (unsigned long long i = 0; i != 15; ++i)
    if (sub_140001000(i) != i) return 1;
  sub_140001000(15);
  return 1;
}
)",
                      true);
}

TEST(ComputedCodeBranch, X64Immediate32CodeBaseKeepsItsOriginalCoordinate) {
  const Platform LowELF = {Arch::X64, BinaryFormat::ELF,
                           ExceptionEncoding::DwarfFDE, 0x400000};
  std::vector<uint8_t> Code = {0x89, 0xf8, 0x83, 0xe0, 3, 0xc1, 0xe0, 3,
                               0x41, 0xbb, 0,    0,    0, 0, // mov r11d,base
                               0x49, 0x29, 0xc3,             // sub r11,rax
                               0x41, 0xff, 0xe3};            // jmp r11
  for (unsigned I = 0; I != 4; ++I)
    Code[10 + I] = uint8_t((LowELF.Entry + 0x40) >> (I * 8));
  Code.resize(0x48, 0xcc);
  for (unsigned I = 0; I != 4; ++I) {
    const auto Offset = 0x40 - I * 8;
    Code[Offset] = 0xb8;
    Code[Offset + 1] = I;
    Code[Offset + 2] = Code[Offset + 3] = Code[Offset + 4] = 0;
    Code[Offset + 5] = 0xc3;
  }
  auto Image = makeImage(Code, LowELF);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  const auto F = CFGBuilder().build(Image, Dec, LowELF.Entry);
  ASSERT_EQ(F.JumpTables.size(), 1u);
  checkComputedSource(Image, R"(
int main(void) {
  for (unsigned i = 0; i != 64; ++i)
    if (sub_400000(i) != (i & 3)) return 1;
  return 0;
}
)");
}

TEST(ComputedCodeBranch, NewlyDiscoveredUnknownBackedgeWithdrawsAllTargets) {
  std::vector<uint8_t> Code = {
      0x89, 0xc8, 0x83, 0xe0, 1,    0xc1, 0xe0, 3,
      0x4c, 0x8d, 0x1d, 0x41, 0,    0,    0,    // lea r11,[+0x50]
      0x49, 0x29, 0xc3, 0x49, 0x83, 0xeb, 0x10, // sub r11,rax; sub r11,16
      0x41, 0xff, 0xe3};
  // The address-taken anchor is not a case: the unknown arm is first
  // discovered through computed-target exploration, not a LEA root.
  Code.resize(0x58, 0xcc);
  Code[0x38] = 0xc3;
  // The newly discovered arm supplies an arbitrary RDX at the original JMP.
  // A target set inferred before discovering that edge has no final authority.
  const std::vector<uint8_t> Arm = {0x49, 0x89, 0xd3, // mov r11,rdx
                                    0xe9, 0xce, 0xff, 0xff, 0xff};
  std::copy(Arm.begin(), Arm.end(), Code.begin() + 0x40);
  auto Image = makeImage(Code, Win64);
  Decoder Dec;
  ASSERT_TRUE(Dec.init(Image));
  const auto F = CFGBuilder().build(Image, Dec, Entry);
  EXPECT_TRUE(F.JumpTables.empty());
  EXPECT_TRUE(F.UnsafeIndirectBranchAddresses.count(Entry + 22));
}

TEST(ComputedCodeBranch, EscapingCodeIdentityCannotBecomeNumericLayout) {
  std::vector<uint8_t> Code = {0x89, 0xc8, 0x83, 0xe0, 1,    0xc1, 0xe0,
                               3,    0x4c, 0x8d, 0x1d, 0x31, 0,    0,
                               0,    0x49, 0x29, 0xc3, 0x41, 0xff, 0xe3};
  Code.resize(0x48, 0xcc);
  for (unsigned Offset : {0x38u, 0x40u}) {
    // Both arms return the computed code pointer. A numeric switch alone
    // cannot establish the relocated identity of this independently observed
    // pointer, even though its set of machine destinations is complete.
    const std::vector<uint8_t> Arm = {0x4c, 0x89, 0xd8, 0xc3};
    std::copy(Arm.begin(), Arm.end(), Code.begin() + Offset);
  }
  auto Image = makeImage(Code, Win64);
  for (bool LLVM : {false, true}) {
    llvm::LLVMContext Context;
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = LLVM;
    Options.SourceProjection = true;
    Options.OnlyFunctionEntries = {Entry};
    auto Result = Pipeline().run(Image, Context, Options);
    EXPECT_FALSE(Result.Success);
    EXPECT_NE(Result.Error.find("escaping or unproved numeric selector"),
              std::string::npos)
        << Result.Error;
  }
}

LowOp scalarTestOp(va_t Address, NdOp Opcode, NdVar Output,
                   std::initializer_list<NdVar> Inputs) {
  LowOp Op;
  Op.Addr = Address;
  Op.Opcode = Opcode;
  Op.Output = Output;
  for (auto Input : Inputs)
    Op.addInput(Input);
  return Op;
}

TEST(ComputedCodeBranch, AllPredecessorsAndLoopIterationsOwnTheDomain) {
  for (bool Reverse : {false, true})
    for (bool UnknownArm : {false, true})
      for (unsigned Root = 0; Root != 3; ++Root) {
        LowFunc F;
        F.Entry = 0x1000;
        F.Blocks.resize(4);
        for (int I = 0; I != 4; ++I) {
          F.Blocks[I].Id = I;
          F.Blocks[I].StartAddr = F.Entry + I * 16;
        }
        const auto Index = NdVar::reg(0, 4), Target = NdVar::reg(8, 4);
        F.Blocks[0].Succs =
            Reverse ? std::vector<int>{2, 1} : std::vector<int>{1, 2};
        for (int I : {1, 2}) {
          F.Blocks[I].Preds = {0};
          F.Blocks[I].Succs = {3};
        }
        F.Blocks[1].Ops = {scalarTestOp(0x1010, NdOp::INT_AND, Index,
                                        {NdVar::reg(16, 4), NdVar::cst(3, 4)})};
        if (!UnknownArm)
          F.Blocks[2].Ops = {
              scalarTestOp(0x1020, NdOp::COPY, Index, {NdVar::cst(7, 4)})};
        F.Blocks[3].Preds =
            Reverse ? std::vector<int>{3, 2, 1} : std::vector<int>{1, 2, 3};
        F.Blocks[3].Succs = {3};
        F.Blocks[3].Ops = {scalarTestOp(0x1030, NdOp::INT_ADD, Target,
                                        {Index, NdVar::cst(0x1040, 4)}),
                           scalarTestOp(0x1031, NdOp::INDIR_BR, {}, {Target})};
        if (Root == 1)
          F.ModuleAnalysisRoots.insert(F.Blocks[3].StartAddr);
        if (Root == 2)
          F.OrdinaryModuleAnalysisRoots.insert(F.Blocks[3].StartAddr);
        size_t Work = 10000;
        const auto Result = finiteScalarBranchTargets(F, Work);
        ASSERT_TRUE(Result);
        if (UnknownArm || Root)
          EXPECT_TRUE(Result->empty());
        else {
          ASSERT_TRUE(Result->count(0x1031));
          EXPECT_EQ(
              Result->at(0x1031),
              (std::vector<va_t>{0x1040, 0x1041, 0x1042, 0x1043, 0x1047}));
        }
        Work = 1;
        EXPECT_FALSE(finiteScalarBranchTargets(F, Work));
      }
}

TEST(ComputedCodeBranch, PartialWritesCallsAndLargeMasksCannotBorrowFacts) {
  for (unsigned Variant = 0; Variant != 5; ++Variant) {
    LowFunc F;
    F.Entry = 0x1000;
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = F.Entry;
    const auto Index = NdVar::reg(0, 4);
    Block.Ops = {scalarTestOp(
        0x1000, NdOp::INT_AND, Index,
        {NdVar::reg(16, 4), NdVar::cst(Variant == 3 ? 127 : 3, 4)})};
    if (Variant == 1)
      Block.Ops.push_back(scalarTestOp(0x1001, NdOp::COPY, NdVar::reg(1, 1),
                                       {NdVar::reg(24, 1)}));
    if (Variant == 2)
      Block.Ops.push_back(
          scalarTestOp(0x1001, NdOp::CALL, {}, {NdVar::cst(0x2000, 8)}));
    if (Variant == 4) {
      auto Fragment = NdVar::cst(0x1000, 4);
      Fragment.Provenance = ConstantAddressProvenance::AddressFragment;
      Block.Ops.push_back(
          scalarTestOp(0x1001, NdOp::INT_ADD, Index, {Index, Fragment}));
    }
    Block.Ops.push_back(scalarTestOp(0x1002, NdOp::INDIR_BR, {}, {Index}));
    F.Blocks = {Block};
    size_t Work = 10000;
    const auto Result = finiteScalarBranchTargets(F, Work);
    ASSERT_TRUE(Result);
    EXPECT_EQ(Result->count(0x1002), Variant == 0 ? 1u : 0u);
  }
}

TEST(OwnInteriorCall, CallsForTheirReturnAddressPushAndJump) {
  // Each call skips an int3 to the next instruction; the stack pointer is
  // then restored past both return addresses.
  const Lifted L = lift({0x55,                         // push rbp
                         0x48, 0x89, 0xe5,             // mov  rbp, rsp
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x14000100A
                         0xcc,                         // int3
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x140001010
                         0xcc,                         // int3
                         0x48, 0x83, 0xc4, 0x10,       // add  rsp, 10h
                         0xb8, 0x07, 0x00, 0x00, 0x00, // mov  eax, 7
                         0x5d,                         // pop  rbp
                         0xc3});                       // ret
  ASSERT_TRUE(L.Unsupported.empty());
  EXPECT_EQ(L.Source.find("sub_14000100A"), std::string::npos) << L.Source;
  EXPECT_EQ(L.Source.find("sub_140001010"), std::string::npos) << L.Source;
  EXPECT_EQ(L.Source.find("unknown value"), std::string::npos) << L.Source;
  compileAndRun(L.Source + ReturnsSeven);
}

TEST(OwnInteriorCall, LoopedPushesRestoredThroughTheFramePointer) {
  // The pushes go deeper on every iteration; the frame pointer restores the
  // stack pointer above all of them.
  const Lifted L = lift({0x55,                         // push rbp
                         0x48, 0x89, 0xe5,             // mov  rbp, rsp
                         0xb9, 0x02, 0x00, 0x00, 0x00, // mov  ecx, 2
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x14000100F
                         0xcc,                         // int3
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x140001015
                         0xcc,                         // int3
                         0xff, 0xc9,                   // dec  ecx
                         0x75, 0xf0,                   // jnz  0x140001009
                         0x48, 0x89, 0xec,             // mov  rsp, rbp
                         0xb8, 0x07, 0x00, 0x00, 0x00, // mov  eax, 7
                         0x5d,                         // pop  rbp
                         0xc3});                       // ret
  ASSERT_TRUE(L.Unsupported.empty());
  EXPECT_EQ(L.Source.find("sub_14000100F"), std::string::npos) << L.Source;
  compileAndRun(L.Source + ReturnsSeven);
}

TEST(OwnInteriorCall, SubroutineThatReturnsStaysACall) {
  std::vector<uint8_t> Code = {0xe8, 0x0b, 0x00, 0x00, 0x00, // call 0x140001010
                               0x83, 0xc0, 0x01,             // add  eax, 1
                               0xc3};                        // ret
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0xb8, 0x05, 0x00, 0x00, 0x00, // mov eax, 5
                           0xc3});                       // ret
  const Lifted L = lift(std::move(Code));
  ASSERT_TRUE(L.Unsupported.empty());
  EXPECT_NE(L.Source.find("sub_140001010()"), std::string::npos) << L.Source;
  EXPECT_EQ(L.Source.find("return 5"), std::string::npos) << L.Source;
}

TEST(OwnInteriorCall, ClosedRecursiveNearCallsPreserveEachInvocation) {
  for (const Platform &P : {Win64, SysV64}) {
    // Two mutually recursive internal routines. The second decrements the
    // argument and tail-enters the first until zero; each ordinary RET then
    // consumes precisely its own CALL's PC.
    std::vector<uint8_t> Code = {0xB9, 5,    0, 0,   0, // mov ecx,5
                                 0xE8, 6,    0, 0,   0, // call +0x10
                                 0x83, 0xC0, 7, 0xC3};  // add eax,7; ret
    Code.resize(0x10, 0xCC);
    Code.insert(Code.end(), {0xE8, 0x0B, 0, 0, 0, 0xC3}); // call +0x20; ret
    Code.resize(0x20, 0xCC);
    Code.insert(Code.end(), {0x31, 0xC0,    // xor eax,eax
                             0x83, 0xE9, 1, // sub ecx,1
                             0x75, 0xE9,    // jne +0x10
                             0xC3});        // ret
    BinaryImage Image = makeImage(Code, P);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Image));
    const auto Low = CFGBuilder().build(Image, Dec, P.Entry);
    EXPECT_TRUE(Low.hasCompleteLiftCoverage());
    EXPECT_TRUE(Low.UnprovenReturnAddresses.empty());
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(static_cast<int>(P.Format));
        SCOPED_TRACE(LLVM);
        SCOPED_TRACE(NoOpt);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.LiftMode = LLVM;
        Options.SourceProjection = true;
        Options.NoOpt = NoOpt;
        Options.OnlyFunctionEntries = {P.Entry, P.Entry + 0x10, P.Entry + 0x20};
        auto Result = Pipeline().run(Image, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        std::string Source;
        llvm::raw_string_ostream Output(Source);
        CEmitterOptions COptions;
        COptions.TheArch = P.TheArch;
        COptions.Format = P.Format;
        COptions.Image = &Image;
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          ASSERT_TRUE(
              LLVMCEmitter().emit(*Result.LlvmModule, Output, COptions));
        } else
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Output, COptions));
        EXPECT_EQ(Source.find("__builtin_trap"), std::string::npos) << Source;
        compileAndRun(Source + ReturnsSeven);
      }
  }
}

TEST(OwnInteriorCall, ClosedRecursionCannotRepairUnknownOrUnbalancedEffects) {
  for (const std::vector<uint8_t> &Effect : {
           std::vector<uint8_t>{0x48, 0x89, 0x04, 0x24}, // replace this PC
           std::vector<uint8_t>{0x48, 0x89, 0x01},       // arbitrary write
           std::vector<uint8_t>{0xFF, 0xD0},             // unknown call
           std::vector<uint8_t>{0x48, 0x83, 0xEC, 8},    // unbalanced frame
       }) {
    std::vector<uint8_t> Code = {0xB9, 5, 0, 0, 0, 0xE8, 6, 0, 0, 0, 0xC3};
    Code.resize(0x10, 0xCC);
    Code.insert(Code.end(), {0xE8, 0x0B, 0, 0, 0, 0xC3});
    Code.resize(0x20, 0xCC);
    Code.insert(Code.end(), Effect.begin(), Effect.end());
    Code.insert(Code.end(), {0x83, 0xE9, 1, 0x75});
    Code.push_back(uint8_t(0x10 - (Code.size() + 1)));
    Code.push_back(0xC3);
    const auto Result = lift(Code);
    EXPECT_FALSE(Result.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, ReturnBelowTheEntryStackPointerIsRefused) {
  // Only one of the two return addresses is discarded, so `pop rbp` takes
  // the other and `ret` pops the saved frame pointer.
  const Lifted L = lift({0x55,                         // push rbp
                         0x48, 0x89, 0xe5,             // mov  rbp, rsp
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x14000100A
                         0xcc,                         // int3
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x140001010
                         0xcc,                         // int3
                         0x48, 0x83, 0xc4, 0x08,       // add  rsp, 8
                         0xb8, 0x07, 0x00, 0x00, 0x00, // mov  eax, 7
                         0x5d,                         // pop  rbp
                         0xc3});                       // ret
  EXPECT_TRUE(L.Source.empty()) << L.Source;
  EXPECT_EQ(L.UnprovenReturns, std::vector<va_t>{0x14000101A});
  EXPECT_NE(
      std::find(L.Unsupported.begin(), L.Unsupported.end(), va_t{0x14000101A}),
      L.Unsupported.end());
}

TEST(OwnInteriorCall, ReturnAfterAStackSwitchIsRefused) {
  // The stack pointer comes from memory before the return, so nothing shows
  // the pushed return address is not what it pops.
  const Lifted L = lift({0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x140001006
                         0xcc,                         // int3
                         0x48, 0x8b, 0x21,             // mov  rsp, [rcx]
                         0xc3});                       // ret
  EXPECT_TRUE(L.Source.empty()) << L.Source;
  EXPECT_EQ(L.UnprovenReturns, std::vector<va_t>{0x140001009});
}

TEST(OwnInteriorCall,
     RejectedReturnsRetainOnlyTheirNativeInstructionSelection) {
  auto Image =
      makeImage({0xe8, 1, 0, 0, 0, 0xcc, 0x48, 0x8b, 0x21, 0xc3}, SysV64);
  Image.Segments.front().FileSz = Image.Segments.front().Data.size();
  PipelineOptions Options;
  Options.OnlyFunctionEntries = {Entry};
  Options.EmitDumpOutput = false;
  Options.DeferMedIR = true;
  llvm::LLVMContext Context;
  auto Selected = Pipeline().run(Image, Context, Options);
  ASSERT_TRUE(Selected.Success) << Selected.Error;
  EXPECT_TRUE(Selected.LowFuncs.empty());
  EXPECT_TRUE(Selected.MedFuncs.empty());
  ASSERT_EQ(Selected.FunctionAudits.size(), 1u);
  const auto &Audit = Selected.FunctionAudits.front();
  EXPECT_FALSE(Audit.HasLowIR);
  EXPECT_EQ(Audit.UnprovenReturns, std::vector<va_t>{Entry + 9});
  // The interior CALL is a physical push-and-jump. Its old fallthrough is
  // outside the recovered region until the environment resumes there.
  EXPECT_EQ(Audit.NativeInstructionAddresses,
            (std::vector<va_t>{Entry, Entry + 6, Entry + 9}));
  const std::string Request =
      R"({"profile":"kernel-events-v6","mxcsr_mask":65471,"paging_address_bits":48,"entries":[)" +
      std::to_string(Entry) + "]}";
  for (bool LLVM : {false, true}) {
    auto Source = exportX64Continuation(Image, Request, LLVM);
    ASSERT_TRUE(bool(Source)) << llvm::toString(Source.takeError());
    EXPECT_NE(Source->Source.find("neverd_x64_continue_v6"), std::string::npos);
    EXPECT_NE(Source->Manifest.find("rejected-incomplete"), std::string::npos);
  }
  Options.DeferMedIR = false;
  auto Ordinary = Pipeline().run(Image, Context, Options);
  EXPECT_TRUE(Ordinary.LowFuncs.empty());
  EXPECT_TRUE(Ordinary.MedFuncs.empty());
  EXPECT_EQ(Ordinary.FunctionAudits.front().UnprovenReturns,
            std::vector<va_t>{Entry + 9});
}

TEST(OwnInteriorCall, SystemVCallsForTheirReturnAddressPushAndJump) {
  // The same body as above under a DWARF FDE.
  const Lifted L = lift({0x55,                         // push rbp
                         0x48, 0x89, 0xe5,             // mov  rbp, rsp
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x14000100A
                         0xcc,                         // int3
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x140001010
                         0xcc,                         // int3
                         0x48, 0x83, 0xc4, 0x10,       // add  rsp, 10h
                         0xb8, 0x07, 0x00, 0x00, 0x00, // mov  eax, 7
                         0x5d,                         // pop  rbp
                         0xc3},                        // ret
                        SysV64);
  ASSERT_TRUE(L.Unsupported.empty());
  EXPECT_EQ(L.Source.find("sub_14000100A"), std::string::npos) << L.Source;
  EXPECT_EQ(L.Source.find("sub_140001010"), std::string::npos) << L.Source;
  compileAndRun(L.Source + ReturnsSeven);
}

TEST(OwnInteriorCall, SystemVSubroutineThatReturnsStaysACall) {
  std::vector<uint8_t> Code = {0xe8, 0x0b, 0x00, 0x00, 0x00, // call 0x140001010
                               0x83, 0xc0, 0x01,             // add  eax, 1
                               0xc3};                        // ret
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), {0xb8, 0x05, 0x00, 0x00, 0x00, // mov eax, 5
                           0xc3});                       // ret
  const Lifted L = lift(std::move(Code), SysV64);
  ASSERT_TRUE(L.Unsupported.empty());
  EXPECT_NE(L.Source.find("sub_140001010()"), std::string::npos) << L.Source;
}

TEST(OwnInteriorCall, SystemVReturnFromASwitchedStackIsRefused) {
  // The target loads the stack pointer from a register and returns on that
  // stack: nothing shows its return pops the address the call pushed, so the
  // call is not the call it looks like.
  const Lifted L = lift({0xe8, 0x02, 0x00, 0x00, 0x00, // call 0x140001007
                         0xeb, 0x04,                   // jmp  0x14000100B
                         0x48, 0x89, 0xd4,             // mov  rsp, rdx
                         0xc3,                         // ret
                         0xc3},                        // ret
                        SysV64);
  EXPECT_TRUE(L.Source.empty()) << L.Source;
  EXPECT_EQ(L.UnprovenReturns, std::vector<va_t>{0x14000100A});
}

TEST(OwnInteriorCall, I386CallsForTheirReturnAddressPushAndJump) {
  // Four-byte return addresses: `add esp, 8` discards both.
  const Lifted L = lift({0x55,                         // push ebp
                         0x89, 0xe5,                   // mov  ebp, esp
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x8049009
                         0xcc,                         // int3
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x804900F
                         0xcc,                         // int3
                         0x83, 0xc4, 0x08,             // add  esp, 8
                         0xb8, 0x07, 0x00, 0x00, 0x00, // mov  eax, 7
                         0x5d,                         // pop  ebp
                         0xc3},                        // ret
                        I386);
  ASSERT_TRUE(L.Unsupported.empty());
  EXPECT_TRUE(L.UnprovenReturns.empty());
  EXPECT_EQ(L.Source.find("sub_8049009"), std::string::npos) << L.Source;
  EXPECT_EQ(L.Source.find("sub_804900F"), std::string::npos) << L.Source;
  EXPECT_NE(L.Source.find("7"), std::string::npos) << L.Source;
}

TEST(OwnInteriorCall, I386ReturnBelowTheEntryStackPointerIsRefused) {
  // Discarding four bytes leaves the other return address for `pop ebp`.
  const Lifted L = lift({0x55,                         // push ebp
                         0x89, 0xe5,                   // mov  ebp, esp
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x8049009
                         0xcc,                         // int3
                         0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x804900F
                         0xcc,                         // int3
                         0x83, 0xc4, 0x04,             // add  esp, 4
                         0xb8, 0x07, 0x00, 0x00, 0x00, // mov  eax, 7
                         0x5d,                         // pop  ebp
                         0xc3},                        // ret
                        I386);
  EXPECT_TRUE(L.Source.empty()) << L.Source;
  EXPECT_EQ(L.UnprovenReturns, std::vector<va_t>{0x8049018});
}

TEST(OwnInteriorCall, RecoveredRegistrationRangeBoundsNothing) {
  // An `_except_handler3` range is NeverD's reconstruction, not the image's
  // declaration, so the call stays a call and nothing is refused.
  const Lifted L = lift({0xe8, 0x01, 0x00, 0x00, 0x00, // call 0x401006
                         0xcc,                         // int3
                         0x8b, 0x21,                   // mov  esp, [ecx]
                         0xc3},                        // ret
                        I386Registration);
  EXPECT_TRUE(L.UnprovenReturns.empty());
  EXPECT_NE(L.Source.find("sub_401006"), std::string::npos) << L.Source;
}

TEST(OwnInteriorCall, OnlyDeclaredSingleFunctionRecordsBoundTheBody) {
  for (ExceptionEncoding E :
       {ExceptionEncoding::X64UnwindV1, ExceptionEncoding::X64UnwindV2,
        ExceptionEncoding::X64UnwindV3, ExceptionEncoding::ARM64Unpacked,
        ExceptionEncoding::DwarfFDE, ExceptionEncoding::GoFuncTable})
    EXPECT_TRUE(exceptionEncodingBoundsOneFunction(E))
        << getExceptionEncodingName(E);
  for (ExceptionEncoding E :
       {ExceptionEncoding::Unknown, ExceptionEncoding::CompactUnwind,
        ExceptionEncoding::X86ScopeTableEH3,
        ExceptionEncoding::X86ScopeTableEH4, ExceptionEncoding::X86CxxFuncInfo,
        ExceptionEncoding::DelphiX86Chain, ExceptionEncoding::ARMEHABIInline,
        ExceptionEncoding::ARMEHABIGeneric})
    EXPECT_FALSE(exceptionEncodingBoundsOneFunction(E))
        << getExceptionEncodingName(E);
}

constexpr Platform Darwin64 = {Arch::X64, BinaryFormat::MachO,
                               ExceptionEncoding::DwarfFDE, Entry};

void expectSourceRefused(const BinaryImage &Img) {
  for (bool LLVM : {false, true})
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(::testing::Message() << LLVM << '/' << NoOpt);
      llvm::LLVMContext Context;
      PipelineOptions Options;
      Options.EmitDumpOutput = false;
      Options.OnlyFunctionEntries = {Img.Entry};
      Options.SourceProjection = true;
      Options.LiftMode = LLVM;
      Options.NoOpt = NoOpt;
      auto Result = Pipeline().run(Img, Context, Options);
      ASSERT_EQ(Result.FunctionAudits.size(), 1u);
      const auto &Audit = Result.FunctionAudits.front();
      EXPECT_EQ(Audit.Disposition,
                PipelineFunctionDisposition::RejectedIncomplete);
      EXPECT_FALSE(Audit.UnprovenReturns.empty());
      EXPECT_FALSE(Audit.HasLLVMDefinition);
      EXPECT_TRUE(Result.HighFuncs.empty());
    }
}

TEST(OwnInteriorCall, IndirectTailMustRestoreTheParentsStack) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386})
    for (uint8_t Restore : {0, 32, 40}) {
      const bool Restored = Restore == 32;
      SCOPED_TRACE(::testing::Message()
                   << int(P.TheArch) << '/' << int(P.Format) << '/'
                   << int(Restore));
      std::vector<uint8_t> Code;
      if (P.TheArch == Arch::X64)
        Code.push_back(0x48);
      // Allocate a frame, call the next label, then discard that internal PC.
      Code.insert(Code.end(), {0x83, 0xec, 32, 0xe8, 1, 0, 0, 0, 0xcc, 0x58});
      if (Restore) {
        if (P.TheArch == Arch::X64)
          Code.push_back(0x48);
        Code.insert(Code.end(), {0x83, 0xc4, Restore});
      }
      // A callback can receive the parent's PC only after its frame is gone.
      // The CALL + RETURN used to represent this JMP must not lose that fact.
      const va_t Tail = P.Entry + Code.size();
      Code.insert(Code.end(), {0xff, 0xe1}); // jmp ecx/rcx
      auto Img = makeImage(std::move(Code), P);
      Decoder Dec;
      ASSERT_TRUE(Dec.init(Img));
      CFGBuilder Builder;
      const auto Low = Builder.build(Img, Dec, P.Entry, "tail_frame");
      EXPECT_EQ(Low.hasCompleteLiftCoverage(), Restored);
      EXPECT_EQ(Low.UnprovenReturnAddresses,
                Restored ? std::vector<va_t>{} : std::vector<va_t>{Tail});
      if (!Restored)
        expectSourceRefused(Img);
    }
}

TEST(OwnInteriorCall, DiscardedCallMustRetainTheParentsReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386})
    for (bool Tail : {false, true})
      for (bool OverwriteParent : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << int(P.TheArch) << '/' << int(P.Format) << '/' << Tail
                     << '/' << OverwriteParent);
        const uint8_t Width = P.TheArch == Arch::X64 ? 8 : 4;
        // Overwriting the discarded internal PC is harmless; overwriting
        // the parent's PC cannot become an ordinary return or callback.
        std::vector<uint8_t> Code{
            0xe8, 1,    0,    0,    0,
            0xcc, 0xc7, 0x44, 0x24, uint8_t(OverwriteParent ? Width : 0),
            0,    0,    0,    0};
        if (P.TheArch == Arch::X64)
          Code.push_back(0x48);
        Code.insert(Code.end(), {0x83, 0xc4, Width});
        if (Tail)
          Code.insert(Code.end(), {0xff, 0xe1}); // jmp ecx/rcx
        else
          Code.insert(Code.end(), {0xb8, 7, 0, 0, 0, 0xc3}); // mov eax,7; ret
        auto Img = makeImage(std::move(Code), P);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Img));
        CFGBuilder Builder;
        const auto Low = Builder.build(Img, Dec, P.Entry, "parent_pc");
        EXPECT_EQ(Low.hasCompleteLiftCoverage(), !OverwriteParent);
        EXPECT_EQ(Low.UnprovenReturnAddresses.empty(), !OverwriteParent);
        if (OverwriteParent)
          expectSourceRefused(Img);
      }
}

TEST(OwnInteriorCall, FrameReadsDoNotHideTheirExplicitStores) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386})
    for (bool Flags : {false, true})
      for (bool ParentStore : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << int(P.TheArch) << '/' << int(P.Format) << '/' << Flags
                     << '/' << ParentStore);
        std::vector<uint8_t> Code;
        if (P.TheArch == Arch::X64)
          Code.push_back(0x48);
        Code.insert(Code.end(), {0x83, 0xec, 32, 0xe8, 1, 0, 0, 0, 0xcc, 0x58});
        const uint8_t Offset = ParentStore ? 32 : 4;
        if (Flags) {
          const uint8_t AboveParent = 32 + (P.TheArch == Arch::X64 ? 8 : 4);
          if (ParentStore) {
            if (P.TheArch == Arch::X64)
              Code.push_back(0x48);
            Code.insert(Code.end(), {0x83, 0xc4, AboveParent});
          }
          Code.insert(Code.end(),
                      {0xf9, 0x9c, 0x58}); // stc; pushf; pop eax/rax
          if (ParentStore) {
            if (P.TheArch == Arch::X64)
              Code.push_back(0x48);
            Code.insert(Code.end(), {0x83, 0xec, AboveParent});
          }
        } else
          Code.insert(Code.end(),
                      {0x0f, 0xae, 0x5c, 0x24, Offset}); // stmxcsr [sp+offset]
        if (P.TheArch == Arch::X64)
          Code.push_back(0x48);
        Code.insert(Code.end(), {0x83, 0xc4, 32, 0xb8, 7, 0, 0, 0, 0xc3});
        const auto Img = makeImage(std::move(Code), P);
        Decoder Dec;
        ASSERT_TRUE(Dec.init(Img));
        CFGBuilder Builder;
        const auto Low = Builder.build(Img, Dec, P.Entry, "frame_read");
        EXPECT_EQ(Low.hasCompleteLiftCoverage(), !ParentStore);
        EXPECT_EQ(Low.UnprovenReturnAddresses.empty(), !ParentStore);
        if (ParentStore)
          expectSourceRefused(Img);
      }
}

TEST(OwnInteriorCall, FrameReadProofRequiresTheCompleteIntrinsicShape) {
  for (Arch Architecture : {Arch::X86, Arch::X64})
    for (auto Id : {Intrinsic::Pushf, Intrinsic::X86ReadMXCSR}) {
      LowOp Read;
      Read.Opcode = NdOp::INTRINSIC;
      Read.addInput(NdVar::cst(static_cast<unsigned>(Id), 2));
      Read.Output = NdVar::tmp(1, Id == Intrinsic::X86ReadMXCSR ? 4
                                  : Architecture == Arch::X64   ? 8
                                                                : 4);
      ASSERT_TRUE(isX86FrameReadIntrinsic(Read, Architecture));
      EXPECT_FALSE(isX86FrameReadIntrinsic(Read, Arch::AArch64));
      if (Id == Intrinsic::Pushf) {
        auto Narrow = Read;
        Narrow.Output.Size = 2;
        EXPECT_TRUE(isX86FrameReadIntrinsic(Narrow, Architecture));
        Narrow.Output.Size = Architecture == Arch::X64 ? 4 : 8;
        EXPECT_FALSE(isX86FrameReadIntrinsic(Narrow, Architecture));
      }
      for (unsigned Mutation = 0; Mutation != 8; ++Mutation) {
        auto Changed = Read;
        if (Mutation == 0)
          Changed.Output = NdVar::reg(x86reg::RSP, Read.Output.Size);
        if (Mutation == 1)
          Changed.Output.Size = 1;
        if (Mutation == 2)
          Changed.MemoryAddressSpace = NdMemoryAddressSpace::X86FS;
        if (Mutation == 3)
          Changed.Inputs[0].Size = 4;
        if (Mutation == 4)
          Changed.NumInputs = 0;
        if (Mutation == 5)
          Changed.addInput(NdVar::cst(0, 4));
        if (Mutation == 6)
          Changed.Inputs[0] =
              NdVar::cst(static_cast<unsigned>(Intrinsic::Popf), 2);
        if (Mutation == 7)
          Changed.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
        EXPECT_FALSE(isX86FrameReadIntrinsic(Changed, Architecture))
            << int(Id) << '/' << Mutation;
      }
    }
}

TEST(OwnInteriorCall, FrameReadValuesSurviveBothSourceRoutes) {
#if !defined(__x86_64__) && !defined(_M_X64)
  GTEST_SKIP()
      << "the source execution oracle requires native x64 flags and MXCSR";
#else
  for (bool Flags : {false, true})
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << Flags << '/' << LLVM << '/' << NoOpt);
        std::vector<uint8_t> Code{0x48, 0x83, 0xec, 32,   0xe8, 1,
                                  0,    0,    0,    0xcc, 0x58};
        if (Flags)
          Code.insert(Code.end(), {0x83, 0xff, 42, 0x9c, 0x58, 0x83, 0xe0,
                                   1}); // cmp edi,42; pushf; pop rax; and eax,1
        else
          Code.insert(Code.end(), {0x0f, 0xae, 0x5c, 0x24, 4, 0x8b, 0x44, 0x24,
                                   4}); // stmxcsr; load result
        Code.insert(Code.end(), {0x48, 0x83, 0xc4, 32, 0xc3});
        auto Img = makeImage(std::move(Code), SysV64);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Img.Entry};
        Options.SourceProjection = true;
        Options.LiftMode = LLVM;
        Options.NoOpt = NoOpt;
        auto Result = Pipeline().run(Img, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_FALSE(Result.MedFuncs.empty());
        for (const auto &Audit : Result.FunctionAudits)
          EXPECT_TRUE(Audit.UnprovenReturns.empty());
        std::string Source;
        llvm::raw_string_ostream Stream(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Img.Arch;
        COptions.Format = Img.Format;
        COptions.Image = &Img;
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          ASSERT_TRUE(
              LLVMCEmitter().emit(*Result.LlvmModule, Stream, COptions));
        } else
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Stream, COptions));
        EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
        EXPECT_EQ(Source.find("unknown register"), std::string::npos) << Source;
        compileAndRun(Source + (Flags ? R"C(
int main(void) {
  for (unsigned value = 0; value != 256; ++value)
    if (sub_140001000(value) != (value < 42))
      return 1;
  return 0;
}
)C"
                                      : R"C(
#include <immintrin.h>
int main(void) {
  unsigned original = _mm_getcsr();
  int failed = 0;
  for (unsigned mode = 0; mode != 4; ++mode) {
    unsigned expected = (original & ~0x6000u) | (mode << 13);
    _mm_setcsr(expected);
    failed |= sub_140001000() != expected;
  }
  _mm_setcsr(original);
  return failed;
}
)C"));
      }
#endif
}

TEST(OwnInteriorCall, FrameReadStoreRequiresAnExactUnsegmentedAccess) {
  for (Arch Architecture : {Arch::X86, Arch::X64}) {
    LowOp Store;
    Store.Opcode = NdOp::INTRINSIC;
    Store.addInput(NdVar::cst(static_cast<unsigned>(Intrinsic::Stmxcsr), 2));
    Store.addInput(NdVar::tmp(1, 8));
    const auto Memory = x86FrameStateStore(Store, Architecture);
    ASSERT_TRUE(Memory.Complete);
    EXPECT_EQ(Memory.Address, &Store.Inputs[1]);
    EXPECT_EQ(Memory.AccessSize, 4);
    EXPECT_EQ(Memory.StoredValue, nullptr);
    EXPECT_FALSE(x86FrameStateStore(Store, Arch::AArch64).Complete);
    for (unsigned Mutation = 0; Mutation != 7; ++Mutation) {
      auto Changed = Store;
      if (Mutation == 0)
        Changed.Output = NdVar::tmp(2, 4);
      if (Mutation == 1)
        Changed.Inputs[1].Size = 4;
      if (Mutation == 2)
        Changed.MemoryAddressSpace = NdMemoryAddressSpace::X86GS;
      if (Mutation == 3)
        Changed.Inputs[0].Size = 4;
      if (Mutation == 4)
        Changed.addInput(NdVar::cst(0, 4));
      if (Mutation == 5)
        Changed.Inputs[0] =
            NdVar::cst(static_cast<unsigned>(Intrinsic::Ldmxcsr), 2);
      if (Mutation == 6)
        Changed.MemoryOrdering = NdMemoryOrdering::SequentiallyConsistent;
      EXPECT_FALSE(x86FrameStateStore(Changed, Architecture).Complete)
          << Mutation;
    }
  }
}

Lifted liftOwnReturnBody(const Platform &P, llvm::ArrayRef<uint8_t> Body) {
  std::vector<uint8_t> Code = {0xe8, 0x0b, 0, 0, 0, 0xc3};
  Code.resize(0x10, 0xcc);
  Code.insert(Code.end(), Body.begin(), Body.end());
  return lift(std::move(Code), P);
}

TEST(OwnInteriorCall, OverwrittenReturnSlotCannotProveAnOrdinaryReturn) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    // mov dword ptr [e/rsp], 0; ret. On x64 this destroys half of the PC.
    const auto L = liftOwnReturnBody(P, {0xc7, 0x04, 0x24, 0, 0, 0, 0, 0xc3});
    EXPECT_TRUE(L.Source.empty()) << L.Source;
    EXPECT_FALSE(L.UnprovenReturns.empty());
  }
}

void checkRecoveredTailSource(bool DiscardFrame) {
  for (const auto &P : {Win64, SysV64, Darwin64})
    for (bool LLVM : {false, true})
      for (bool NoOpt : {false, true}) {
        SCOPED_TRACE(::testing::Message()
                     << int(P.Format) << '/' << LLVM << '/' << NoOpt);
        // Both a replaced-PC RET and a JMP after discarding a frame must
        // pass the original parent's stack and the same callback arguments.
        const uint8_t Input = P.Format == BinaryFormat::COFF ? 0xc8 : 0xf8;
        std::vector<uint8_t> Code{0x48, 0x89, Input};
        if (DiscardFrame)
          Code.insert(Code.end(), {0x48, 0x83, 0xec, 32}); // sub rsp,32
        Code.insert(Code.end(), {0xe8, 1, 0, 0, 0, 0xcc});
        const std::vector<uint8_t> Setup =
            P.Format == BinaryFormat::COFF
                ? std::vector<uint8_t>{0xb9, 17,   0,    0,    0,    0x48,
                                       0x83, 0xc2, 1,    0x49, 0x83, 0xc0,
                                       2,    0x49, 0x83, 0xc1, 3}
                : std::vector<uint8_t>{0xbf, 17,   0,    0,    0,    0x48,
                                       0x83, 0xc6, 1,    0x48, 0x83, 0xc2,
                                       2,    0x48, 0x83, 0xc1, 3};
        Code.insert(Code.end(), Setup.begin(), Setup.end());
        if (DiscardFrame)
          Code.insert(Code.end(), {0x48, 0x83, 0xc4, 40, 0xff,
                                   0xe0}); // add rsp,40; jmp rax
        else
          Code.insert(Code.end(),
                      {0x48, 0x89, 0x04, 0x24, 0xc3}); // mov [rsp],rax; ret
        auto Img = makeImage(std::move(Code), P);
        llvm::LLVMContext Context;
        PipelineOptions Options;
        Options.EmitDumpOutput = false;
        Options.OnlyFunctionEntries = {Img.Entry};
        Options.SourceProjection = true;
        Options.LiftMode = LLVM;
        Options.NoOpt = NoOpt;
        auto Result = Pipeline().run(Img, Context, Options);
        ASSERT_TRUE(Result.Success) << Result.Error;
        ASSERT_FALSE(Result.MedFuncs.empty());
        for (const auto &Audit : Result.FunctionAudits)
          EXPECT_TRUE(Audit.UnprovenReturns.empty());
        std::string Source;
        llvm::raw_string_ostream Stream(Source);
        CEmitterOptions COptions;
        COptions.TheArch = Img.Arch;
        COptions.Format = Img.Format;
        COptions.Image = &Img;
        if (LLVM) {
          ASSERT_TRUE(Result.LlvmModule);
          ASSERT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
          ASSERT_TRUE(
              LLVMCEmitter().emit(*Result.LlvmModule, Stream, COptions));
        } else
          ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, Stream, COptions));
        EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
        const char *CallbackValue =
            DiscardFrame ? "#define CALLBACK_VALUE ((void *)&callback)\n"
                         : "#define CALLBACK_VALUE ((uintptr_t)&callback)\n";
        compileAndRun(Source + CallbackValue + R"C(
static unsigned calls;
static uint64_t callback(uint64_t first, uint64_t a, uint64_t b, uint64_t c) {
  ++calls;
  return first == 17 ? ((a + 17) ^ b) - c : 0;
}
int main(void) {
  for (unsigned i = 0; i != 256; ++i) {
    uint64_t expected = ((i + 18) ^ (3 * i + 2)) - (uint64_t)(5 * i + 3);
    if ((uint64_t)sub_140001000(CALLBACK_VALUE, i, 3*i, 5*i) != expected)
      return 1;
  }
  return calls != 256;
}
)C");
      }
}

TEST(OwnInteriorCall, ReplacedPCIsAnIndirectTailTransferOnBothSourceRoutes) {
  checkRecoveredTailSource(false);
}

TEST(OwnInteriorCall, RestoredParentStackReachesTheCallbackOnBothSourceRoutes) {
  checkRecoveredTailSource(true);
}

TEST(OwnInteriorCall, ReplacedPCDoesNotRepairParentCorruptionOrAnExtraFrame) {
  for (const auto &P : {Win64, SysV64, Darwin64}) {
    // A complete replacement of the internal PC still cannot authorize a
    // tail call when the parent PC was overwritten, or when its stack frame
    // is not restored by this pop.
    const auto Corrupt = liftOwnReturnBody(
        P, {0x48, 0x89, 0x4c, 0x24, 8, 0x48, 0x89, 0x04, 0x24, 0xc3});
    EXPECT_TRUE(Corrupt.Source.empty()) << Corrupt.Source;
    EXPECT_FALSE(Corrupt.UnprovenReturns.empty());
    const auto Allocated = liftOwnReturnBody(
        P, {0x48, 0x83, 0xec, 8, 0x48, 0x89, 0x04, 0x24, 0xc3});
    EXPECT_TRUE(Allocated.Source.empty()) << Allocated.Source;
    EXPECT_FALSE(Allocated.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, UnknownStoreMayOverwriteTheActiveReturnSlot) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    // mov dword ptr [ecx/rcx], 0; ret. The pointer may name the active PC.
    const auto L = liftOwnReturnBody(P, {0xc7, 0x01, 0, 0, 0, 0, 0xc3});
    EXPECT_TRUE(L.Source.empty()) << L.Source;
    EXPECT_FALSE(L.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, StackAliasesAndAtomicWritesCannotDestroyTheReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    // mov eax/rax, esp/rsp; mov dword [eax/rax], 0; ret.
    std::vector<uint8_t> Alias;
    if (P.TheArch == Arch::X64)
      Alias.push_back(0x48);
    Alias.insert(Alias.end(), {0x89, 0xe0, 0xc7, 0x00, 0, 0, 0, 0, 0xc3});
    const auto A = liftOwnReturnBody(P, Alias);
    EXPECT_TRUE(A.Source.empty()) << A.Source;
    EXPECT_FALSE(A.UnprovenReturns.empty());
    // xchg [e/rsp], eax; ret.
    const auto B = liftOwnReturnBody(P, {0x87, 0x04, 0x24, 0xc3});
    EXPECT_TRUE(B.Source.empty()) << B.Source;
    EXPECT_FALSE(B.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, PartialOverlapIsCheckedAfterStackAllocation) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    for (uint8_t Offset : {uint8_t{0}, uint8_t{6}}) {
      SCOPED_TRACE(int(Offset));
      // Allocate eight bytes; the store at +6 straddles the PC, while the
      // one at +0 is wholly in the callee's own allocation.
      std::vector<uint8_t> Body;
      if (P.TheArch == Arch::X64)
        Body.push_back(0x48);
      Body.insert(Body.end(),
                  {0x83, 0xec, 8, 0xc7, 0x44, 0x24, Offset, 0, 0, 0, 0});
      if (P.TheArch == Arch::X64)
        Body.push_back(0x48);
      Body.insert(Body.end(), {0x83, 0xc4, 8, 0xc3});
      const auto L = liftOwnReturnBody(P, Body);
      EXPECT_EQ(L.UnprovenReturns.empty(), Offset == 0) << L.Source;
      EXPECT_EQ(L.Source.empty(), Offset != 0) << L.Source;
    }
  }
}

TEST(OwnInteriorCall, OpaqueCalleeDoesNotProveItsCallersReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    // call ecx/rcx; ret. A balanced SP says nothing about the callee's writes.
    const auto L = liftOwnReturnBody(P, {0xff, 0xd1, 0xc3});
    EXPECT_TRUE(L.Source.empty()) << L.Source;
    EXPECT_FALSE(L.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, ClobbersOnTerminatingPathsDoNotInvalidateOtherReturns) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    // test eax,eax; jz ret; mov dword [ecx/rcx],0; ud2; ret.
    const auto L = liftOwnReturnBody(
        P, {0x85, 0xc0, 0x74, 8, 0xc7, 0x01, 0, 0, 0, 0, 0x0f, 0x0b, 0xc3});
    EXPECT_TRUE(L.UnprovenReturns.empty());
    EXPECT_FALSE(L.Source.empty());
  }
}

TEST(OwnInteriorCall, DiscardedReturnSlotsNeedNotRetainTheirValue) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    std::vector<uint8_t> Body = {0xc7, 0x04, 0x24, 0, 0, 0, 0};
    if (P.TheArch == Arch::X64)
      Body.push_back(0x48);
    Body.insert(Body.end(),
                {0x83, 0xc4, uint8_t(P.TheArch == Arch::X64 ? 8 : 4), 0xb8, 7,
                 0, 0, 0, 0xc3});
    const auto L = liftOwnReturnBody(P, Body);
    ASSERT_TRUE(L.Unsupported.empty());
    ASSERT_TRUE(L.UnprovenReturns.empty());
    ASSERT_FALSE(L.Source.empty());
    if (P.TheArch == Arch::X64)
      compileAndRun(L.Source + ReturnsSeven);
  }
}

TEST(OwnInteriorCall, I386NegativeAddressDisplacementPreservesPrivateWrites) {
  // lea esp,[esp-8]; mov dword ptr [esp],0; add esp,8; ret. LowIR carries
  // the signed displacement in a 32-bit constant, then widens the address.
  const auto L =
      liftOwnReturnBody(I386, {0x8d, 0x64, 0x24, 0xf8, 0xc7, 0x04, 0x24, 0, 0,
                               0, 0, 0x83, 0xc4, 8, 0xc3});
  EXPECT_TRUE(L.Unsupported.empty());
  EXPECT_TRUE(L.UnprovenReturns.empty());
  EXPECT_FALSE(L.Source.empty());
}

std::vector<uint8_t> mixedReturnCode(const Platform &P) {
  std::vector<uint8_t> Code = {0xb8, 11,   0, 0,   0, // mov eax,11
                               0xe8, 0x16, 0, 0,   0, // call entry+0x20
                               0x83, 0xc0, 1, 0xc3};  // add eax,1; ret
  Code.resize(0x20, 0xcc);
  if (P.TheArch == Arch::X86)
    Code.insert(Code.end(), {0x83, 0x7c, 0x24, 8, 0}); // cmp [esp+8],0
  else
    Code.insert(Code.end(), {0x85, uint8_t(P.Format == BinaryFormat::COFF
                                               ? 0xc9     // test ecx,ecx
                                               : 0xff)}); // test edi,edi
  std::vector<uint8_t> Nonlocal;
  if (P.TheArch == Arch::X64)
    Nonlocal.push_back(0x48);
  Nonlocal.insert(Nonlocal.end(),
                  {0x83, 0xc4, uint8_t(P.TheArch == Arch::X64 ? 8 : 4), 0xb8, 7,
                   0, 0, 0, 0xc3});
  Code.insert(Code.end(), {0x74, uint8_t(Nonlocal.size())});
  Code.insert(Code.end(), Nonlocal.begin(), Nonlocal.end());
  Code.push_back(0xc3); // ordinary return consumes this call's PC
  return Code;
}

TEST(OwnInteriorCall, MixedExitsResumeOnlyTheMatchingInvocation) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    const auto Code = mixedReturnCode(P);
    const auto L = lift(Code, P);
    ASSERT_TRUE(L.Unsupported.empty());
    ASSERT_TRUE(L.UnprovenReturns.empty());
    ASSERT_FALSE(L.Source.empty());
    EXPECT_EQ(L.Source.find("unknown value"), std::string::npos) << L.Source;
    if (P.TheArch == Arch::X64)
      compileAndRun(L.Source + R"C(
int main(void) {
  return sub_140001000(0) != 12 || sub_140001000(1) != 7 ||
         sub_140001000(-1) != 7;
}
)C");
    for (bool NoOpt : {false, true}) {
      SCOPED_TRACE(NoOpt);
      auto Img = makeImage(Code, P);
      llvm::LLVMContext Ctx;
      PipelineOptions Opts;
      Opts.EmitDumpOutput = false;
      Opts.LiftMode = true;
      Opts.NoOpt = NoOpt;
      Opts.OnlyFunctionEntries.insert(P.Entry);
      auto Result = Pipeline().run(Img, Ctx, Opts);
      ASSERT_TRUE(Result.LlvmModule);
      EXPECT_FALSE(llvm::verifyModule(*Result.LlvmModule, &llvm::errs()));
      ASSERT_EQ(Result.FunctionAudits.size(), 1u);
      EXPECT_TRUE(Result.FunctionAudits.front().HasLLVMDefinition);
    }
  }
}

TEST(OwnInteriorCall, MixedReturnStillRequiresAnUnmodifiedNearReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << int(P.TheArch) << '/'
                                        << int(P.Format) << '/' << Mutation);
      auto Code = mixedReturnCode(P);
      Code.pop_back();
      if (Mutation == 0)
        Code.insert(Code.end(), {0xc7, 0x04, 0x24, 0, 0, 0, 0, 0xc3});
      else if (Mutation == 1)
        Code.insert(Code.end(), {0xc7, 0x01, 0, 0, 0, 0, 0xc3});
      else
        Code.insert(Code.end(), {0xc2, 4, 0});
      const auto L = lift(std::move(Code), P);
      EXPECT_TRUE(L.Source.empty()) << L.Source;
      EXPECT_FALSE(L.UnprovenReturns.empty());
    }
  }
}

TEST(OwnInteriorCall, NonlocalExitMustRetainTheParentsReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    for (unsigned Mutation = 0; Mutation != 3; ++Mutation) {
      SCOPED_TRACE(::testing::Message() << int(P.TheArch) << '/'
                                        << int(P.Format) << '/' << Mutation);
      auto Code = mixedReturnCode(P);
      const size_t Body = P.TheArch == Arch::X64 ? 0x24 : 0x27;
      const unsigned Width = P.TheArch == Arch::X64 ? 8 : 4;
      if (Mutation == 0)
        Code[Body + (P.TheArch == Arch::X64 ? 3 : 2)] = 2 * Width;
      else {
        // The active internal PC and the parent's physical PC have different
        // ownership. A nonlocal return must not consume a changed parent PC.
        const std::vector<uint8_t> Write =
            Mutation == 2 ? std::vector<uint8_t>{0xc7, 0x01, 0, 0, 0, 0}
                          : std::vector<uint8_t>{
                                0xc7, 0x44, 0x24, uint8_t(Width), 0, 0, 0, 0};
        Code.insert(Code.begin() + Body, Write.begin(), Write.end());
        Code[Body - 1] += Write.size();
      }
      const auto L = lift(std::move(Code), P);
      EXPECT_TRUE(L.Source.empty()) << L.Source;
      EXPECT_FALSE(L.UnprovenReturns.empty());
    }
  }
}

TEST(OwnInteriorCall, NonlocalPrivateWritesDoNotAliasTheParentsReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    const bool X64 = P.TheArch == Arch::X64;
    const unsigned Width = X64 ? 8 : 4;
    std::vector<uint8_t> Code;
    if (X64)
      Code.push_back(0x48);
    Code.insert(Code.end(), {0x83, 0xec, 16, 0xb8, 11, 0, 0, 0});
    const unsigned Continuation = Code.size() + 5;
    Code.insert(Code.end(),
                {0xe8, uint8_t(0x20 - Continuation), 0, 0, 0, 0x83, 0xc0, 1});
    if (X64)
      Code.push_back(0x48);
    Code.insert(Code.end(), {0x83, 0xc4, 16, 0xc3});
    Code.resize(0x20, 0xcc);
    const auto Single = mixedReturnCode(P);
    std::vector<uint8_t> Body(Single.begin() + 0x20, Single.end());
    const unsigned Nonlocal = X64 ? 4 : 7;
    if (!X64)
      Body[3] += 16; // the incoming parameter is above the parent allocation
    Body[Nonlocal + (X64 ? 3 : 2)] += 16;
    const std::vector<uint8_t> Write{0xc7, 0x44, 0x24, uint8_t(Width),
                                     0,    0,    0,    0};
    Body[Nonlocal - 1] += Write.size();
    Body.insert(Body.begin() + Nonlocal, Write.begin(), Write.end());
    Code.insert(Code.end(), Body.begin(), Body.end());
    const auto L = lift(std::move(Code), P);
    ASSERT_TRUE(L.Unsupported.empty());
    ASSERT_TRUE(L.UnprovenReturns.empty());
    ASSERT_FALSE(L.Source.empty());
    if (X64)
      compileAndRun(L.Source + R"C(
int main(void) { return sub_140001000(0) != 12 || sub_140001000(1) != 7; }
)C");
  }
}

TEST(OwnInteriorCall, MultipleCallSitesCannotShareOneMixedReturnCertificate) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    // Branch to either of two calls of the same target. Both continuations
    // are distinct even though their physical stack offsets agree.
    auto Code = mixedReturnCode(P);
    const std::vector<uint8_t> Prefix = {0x85, 0xc0, 0x74, 6,    0xe8, 0x17,
                                         0,    0,    0,    0xc3, 0xe8, 0x11,
                                         0,    0,    0,    0xc3};
    std::copy(Prefix.begin(), Prefix.end(), Code.begin());
    const auto L = lift(std::move(Code), P);
    EXPECT_TRUE(L.Source.empty()) << L.Source;
    EXPECT_FALSE(L.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, IndependentAndExceptionalRootsDoNotBorrowAnInvocation) {
  for (bool Exceptional : {false, true}) {
    SCOPED_TRACE(Exceptional);
    auto Code = mixedReturnCode(Win64);
    const auto Return = Win64.Entry + Code.size() - 1;
    auto Img = makeImage(std::move(Code), Win64);
    if (Exceptional) {
      auto &Own = Img.ExceptionMetadata.Functions.front();
      Own.SEH.emplace();
      Own.SEH->Scopes.push_back({{Win64.Entry + 5, Win64.Entry + 10},
                                 SEHScopeKind::CatchAll,
                                 1,
                                 Return,
                                 Return});
    }
    Img.ExceptionMetadata.rebuildIndex();
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    CFGBuilder Builder;
    const std::set<va_t> Roots{Return};
    if (!Exceptional)
      Builder.setCrossFunctionContinuationRoots(&Roots);
    const auto Low = Builder.build(Img, Dec, Win64.Entry, "mixed");
    if (!Exceptional)
      EXPECT_TRUE(Low.OrdinaryModuleAnalysisRoots.count(Return));
    EXPECT_FALSE(Low.UnprovenReturnAddresses.empty());
    EXPECT_FALSE(Low.hasCompleteLiftCoverage());
  }
}

TEST(OwnInteriorCall, NewlyDecodedContinuationMustRevalidateReturnOwnership) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    auto Code = mixedReturnCode(P);
    // The normal continuation reaches the same RET with the parent's SP.
    // That edge is decoded only after proposing the internal return. Its
    // independent invocation must invalidate the provisional certificate.
    Code[10] = 0xe9;
    Code[11] = uint8_t(Code.size() - 1 - 15);
    Code[12] = Code[13] = Code[14] = 0;
    const auto L = lift(std::move(Code), P);
    EXPECT_TRUE(L.Source.empty()) << L.Source;
    EXPECT_FALSE(L.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, RepeatedSingleSiteInvocationsRetainTheirStackBalance) {
  std::vector<uint8_t> Code = {
      0xb8, 11,   0,   0,    0,    // mov eax,11
      0xb9, 2,    0,   0,    0,    // mov ecx,2
      0xe8, 0x31, 0,   0,    0,    // call entry+0x40
      0x83, 0xc0, 1,   0xff, 0xc9, // add eax,1; dec ecx
      0x75, 0xf4, 0xc3};           // jnz call; ret
  Code.resize(0x40, 0xcc);
  const auto Single = mixedReturnCode(SysV64);
  Code.insert(Code.end(), Single.begin() + 0x20, Single.end());
  const auto L = lift(std::move(Code), SysV64);
  ASSERT_TRUE(L.Unsupported.empty());
  ASSERT_TRUE(L.UnprovenReturns.empty());
  ASSERT_FALSE(L.Source.empty());
  compileAndRun(L.Source + R"C(
int main(void) {
  return sub_140001000(0) != 13 || sub_140001000(1) != 7;
}
)C");
}

TEST(OwnInteriorCall, OpenIndirectExitCannotCertifyAMixedInvocation) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    auto Code = mixedReturnCode(P);
    const auto BodyBytes = Code.size() - 0x20;
    const std::vector<uint8_t> Prefix = {0x85, 0xd2, 0x75, uint8_t(BodyBytes)};
    Code.insert(Code.begin() + 0x20, Prefix.begin(), Prefix.end());
    Code.insert(Code.end(), {0xff, 0xe2}); // jmp edx/rdx
    const auto L = lift(std::move(Code), P);
    EXPECT_TRUE(L.Source.empty()) << L.Source;
    EXPECT_FALSE(L.UnprovenReturns.empty());
  }
}

TEST(OwnInteriorCall, LanguageRuntimeInvocationsNeedTheirOwnExitContract) {
  for (unsigned Mutation = 0; Mutation != 5; ++Mutation) {
    SCOPED_TRACE(Mutation);
    auto Img = makeImage(mixedReturnCode(SysV64), SysV64);
    auto &EH = Img.ExceptionMetadata.Functions.front();
    if (Mutation == 0)
      EH.Itanium.emplace();
    else if (Mutation == 1)
      EH.PersonalityVA = SysV64.Entry + 0x100;
    else if (Mutation == 2) {
      EH.Dwarf.emplace();
      EH.Dwarf->LSDAVA = SysV64.Entry + 0x100;
    } else
      EH.ParseStatus = Mutation == 3 ? ExceptionParseStatus::Partial
                                     : ExceptionParseStatus::Malformed;
    Img.ExceptionMetadata.rebuildIndex();
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    CFGBuilder Builder;
    const auto Low = Builder.build(Img, Dec, SysV64.Entry, "mixed");
    EXPECT_FALSE(Low.UnprovenReturnAddresses.empty());
    EXPECT_FALSE(Low.hasCompleteLiftCoverage());
  }
}

TEST(OwnInteriorCall, OrdinaryInternalReturnsRetainTheParentsReturnPC) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    std::vector<uint8_t> Code{0xe8, 0x1b, 0, 0, 0, 0xc3}; // call entry+32; ret
    Code.resize(0x20, 0xcc);
    // The callee's own PC remains intact, but this write changes the parent's
    // physical return address. A normal subroutine classification is unsafe.
    Code.insert(Code.end(),
                {0xc7, 0x44, 0x24, uint8_t(P.TheArch == Arch::X86 ? 4 : 8), 0,
                 0, 0, 0, 0xc3});
    auto Img = makeImage(std::move(Code), P);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    CFGBuilder Builder;
    const auto Low = Builder.build(Img, Dec, P.Entry, "parent_pc");
    EXPECT_FALSE(Low.UnprovenReturnAddresses.empty());
    EXPECT_FALSE(Low.hasCompleteLiftCoverage());
  }
}

TEST(OwnInteriorCall, OrdinaryInternalWritesCanUseTheParentsPrivateAllocation) {
  for (const auto &P : {Win64, SysV64, Darwin64, I386}) {
    SCOPED_TRACE(::testing::Message()
                 << int(P.TheArch) << '/' << int(P.Format));
    std::vector<uint8_t> Code;
    if (P.TheArch == Arch::X64)
      Code.push_back(0x48);
    Code.insert(Code.end(),
                {0x83, 0xec, 16, 0xe8,
                 uint8_t(P.TheArch == Arch::X86 ? 0x18 : 0x17), 0, 0, 0});
    if (P.TheArch == Arch::X64)
      Code.push_back(0x48);
    Code.insert(Code.end(), {0x83, 0xc4, 16, 0xc3});
    Code.resize(0x20, 0xcc);
    // The same displacement now names allocated parent storage. Refusing
    // every write above the callee's PC would lose this valid ordinary call.
    Code.insert(Code.end(),
                {0xc7, 0x44, 0x24, uint8_t(P.TheArch == Arch::X86 ? 4 : 8), 7,
                 0, 0, 0, 0xc3});
    auto Img = makeImage(std::move(Code), P);
    Decoder Dec;
    ASSERT_TRUE(Dec.init(Img));
    CFGBuilder Builder;
    const auto Low = Builder.build(Img, Dec, P.Entry, "parent_private");
    EXPECT_TRUE(Low.UnprovenReturnAddresses.empty());
    EXPECT_TRUE(Low.hasCompleteLiftCoverage());
    EXPECT_TRUE(llvm::any_of(Low.Blocks, [&](const LowBlock &Block) {
      return llvm::any_of(Block.Ops, [&](const LowOp &Op) {
        return Op.Opcode == NdOp::CALL && Op.NumInputs == 1 &&
               Op.Inputs[0].isConst() && Op.Inputs[0].Offset == P.Entry + 0x20;
      });
    }));
  }
}

} // namespace
