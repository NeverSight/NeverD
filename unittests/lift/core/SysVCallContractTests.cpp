//===- SysVCallContractTests.cpp - System V call arguments and returns ---===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// The shapes of QtXml's QDomNode::save and QDomNode::isDocument (x86-64
// System V): a method passes its incoming `this` on in RDI without writing it,
// a virtual call takes the object a dominating block loaded into RDI, and a
// void method returns without writing RAX on one path while it tail-calls on
// the others.  Also xxd's: wrappers pass their arguments straight to a libc
// import, and end at a helper that never returns.  Mach-O images follow
// the same convention.
//
//===----------------------------------------------------------------------===//

#include "../../../lib/pipeline/PipelineLowIRDetail.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/ir/med/LowToMed.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"

#include <algorithm>
#include <cstdlib>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace neverd;

constexpr va_t Text = 0x401000;
constexpr va_t IsDoc = Text;
constexpr va_t Save = Text + 0x40;
constexpr va_t Helper = Text + 0x80;
constexpr va_t Helper2 = Text + 0xA0;

void put(std::vector<uint8_t> &Code, va_t At, std::vector<uint8_t> Bytes) {
  std::copy(Bytes.begin(), Bytes.end(), Code.begin() + (At - Text));
}

uint8_t rel8(va_t From, va_t To) {
  return static_cast<uint8_t>(static_cast<int8_t>(To - From));
}

std::vector<uint8_t> rel32(va_t Next, va_t To) {
  const uint32_t Delta = static_cast<uint32_t>(To - Next);
  return {static_cast<uint8_t>(Delta), static_cast<uint8_t>(Delta >> 8),
          static_cast<uint8_t>(Delta >> 16), static_cast<uint8_t>(Delta >> 24)};
}

BinaryImage makeImage(BinaryFormat Format = BinaryFormat::ELF) {
  std::vector<uint8_t> Code(0xC0, 0xCC);
  // is_doc(node): rdi = node->impl; return impl && impl->vtbl[11](impl) == 9.
  put(Code, IsDoc, {0x48, 0x8B,
                    0x3F, // mov rdi, [rdi]
                    0x48, 0x85,
                    0xFF,                              // test rdi, rdi
                    0x74, rel8(IsDoc + 8, IsDoc + 29), // je none
                    0x48, 0x83,
                    0xEC, 0x08, // sub rsp, 8
                    0x48, 0x8B,
                    0x07, // mov rax, [rdi]
                    0xFF, 0x50,
                    0x58, // call [rax+0x58]
                    0x83, 0xF8,
                    0x09, // cmp eax, 9
                    0x0F, 0x94,
                    0xC0, // sete al
                    0x48, 0x83,
                    0xC4, 0x08, // add rsp, 8
                    0xC3,       // ret
                    0x31, 0xC0, // none: xor eax, eax
                    0xC3});     // ret
  // save(node): if (!node->impl) return; tail-call helper or helper2 with
  // node->impl after is_doc(node), which receives node untouched in RDI.
  std::vector<uint8_t> SaveCode = {0x48, 0x83,
                                   0x3F, 0x00, // cmp qword [rdi], 0
                                   0x74, rel8(Save + 6, Save + 37), // je done
                                   0x53,                            // push rbx
                                   0x48, 0x89,
                                   0xFB,  // mov rbx, rdi
                                   0xE8}; // call is_doc
  for (uint8_t B : rel32(Save + 15, IsDoc))
    SaveCode.push_back(B);
  for (uint8_t B : std::vector<uint8_t>{0x84, 0xC0, // test al, al
                                        0x74, rel8(Save + 19, Save + 28), 0x48,
                                        0x8B, 0x3B, // mov rdi, [rbx]
                                        0x5B,       // pop rbx
                                        0xE9})      // jmp helper
    SaveCode.push_back(B);
  for (uint8_t B : rel32(Save + 28, Helper))
    SaveCode.push_back(B);
  for (uint8_t B :
       std::vector<uint8_t>{0x48, 0x8B, 0x3B, // other: mov rdi,[rbx]
                            0x5B,             // pop rbx
                            0xE9})            // jmp helper2
    SaveCode.push_back(B);
  for (uint8_t B : rel32(Save + 37, Helper2))
    SaveCode.push_back(B);
  SaveCode.push_back(0xC3); // done: ret, RAX never written
  put(Code, Save, SaveCode);
  // helper(impl): impl->field8 = 1; helper2 stores 2.
  put(Code, Helper, {0xC7, 0x47, 0x08, 0x01, 0x00, 0x00, 0x00, 0xC3});
  put(Code, Helper2, {0xC7, 0x47, 0x08, 0x02, 0x00, 0x00, 0x00, 0xC3});

  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = Format;
  Img.Base = 0x400000;
  Img.Entry = Save;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Text;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  const std::pair<va_t, uint64_t> Funcs[] = {
      {IsDoc, 32}, {Save, 38}, {Helper, 8}, {Helper2, 8}};
  const char *Names[] = {"is_doc", "save", "helper", "helper2"};
  for (unsigned I = 0; I < 4; ++I) {
    Symbol Function = Symbol::makeFunc(Funcs[I].first, Funcs[I].second);
    // A Mach-O symbol spells a C name with a leading underscore.
    Function.Name =
        (Format == BinaryFormat::MachO ? "_" : "") + std::string(Names[I]);
    Img.Symbols.push_back(std::move(Function));
    Img.KnownCodeRanges.push_back(
        {Funcs[I].first, Funcs[I].first + Funcs[I].second});
  }
  return Img;
}

std::string lift(BinaryFormat Format = BinaryFormat::ELF) {
  BinaryImage Img = makeImage(Format);
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  for (va_t Entry : {IsDoc, Save, Helper, Helper2})
    Opts.OnlyFunctionEntries.insert(Entry);
  auto Result = Pipeline().run(Img, Ctx, Opts);
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

/// The text of function \p Name's definition.
std::string body(const std::string &Source, const std::string &Name) {
  const size_t At = Source.find(" " + Name + "(");
  if (At == std::string::npos)
    return {};
  const size_t Open = Source.find('{', At);
  const size_t Close = Source.find("\n}", Open);
  if (Open == std::string::npos || Close == std::string::npos)
    return {};
  return Source.substr(Source.rfind('\n', At) + 1,
                       Close + 2 - (Source.rfind('\n', At) + 1));
}

void compileAndRun(const std::string &Source) {
#ifdef NEVERD_TEST_CLANG
  const std::string Compiler = NEVERD_TEST_CLANG;
#else
  const auto Program = llvm::sys::findProgramByName("clang");
  ASSERT_TRUE(bool(Program)) << "clang is required for emitted C execution";
  const std::string Compiler = *Program;
#endif
  llvm::SmallString<128> Directory;
  ASSERT_FALSE(
      llvm::sys::fs::createUniqueDirectory("neverd-sysv-call", Directory));
  const llvm::scope_exit Cleanup(
      [&] { llvm::sys::fs::remove_directories(Directory); });
  llvm::SmallString<128> SourcePath(Directory), BinaryPath(Directory),
      ErrorPath(Directory);
  llvm::sys::path::append(SourcePath, "program.c");
  llvm::sys::path::append(BinaryPath, "program.exe");
  llvm::sys::path::append(ErrorPath, "program.err");
  std::error_code EC;
  {
    llvm::raw_fd_ostream Out(SourcePath, EC);
    ASSERT_FALSE(EC);
    Out << Source;
  }
  const std::optional<llvm::StringRef> Redirects[] = {
      std::nullopt, std::nullopt, ErrorPath.str()};
#ifdef _WIN32
  // Retain both sanitizers. The static ASan runtime cannot intercept some
  // current UCRT math prologues; Clang's own dynamic runtime supports them.
  // Keep that DLL beside this test's executable, without changing PATH or
  // copying runtime files into another test's temporary directory.
  llvm::SmallString<128> RuntimePath(Directory);
  llvm::sys::path::append(RuntimePath, "runtime.txt");
  const std::optional<llvm::StringRef> RuntimeRedirects[] = {
      std::nullopt, RuntimePath.str(), ErrorPath.str()};
  ASSERT_EQ(
      llvm::sys::ExecuteAndWait(
          Compiler,
          {Compiler,
           "-print-file-name=lib/windows/clang_rt.asan_dynamic-x86_64.dll"},
          std::nullopt, RuntimeRedirects, 30),
      0);
  auto Runtime = llvm::MemoryBuffer::getFile(RuntimePath);
  ASSERT_TRUE(bool(Runtime));
  llvm::SmallString<128> RuntimeCopy(Directory);
  llvm::sys::path::append(RuntimeCopy, "clang_rt.asan_dynamic-x86_64.dll");
  ASSERT_FALSE(
      llvm::sys::fs::copy_file((*Runtime)->getBuffer().trim(), RuntimeCopy));
#endif
  for (const char *Optimization : {"-O0", "-O2"}) {
    llvm::SmallVector<llvm::StringRef, 12> Arguments{
        Compiler,
        "-std=c11",
        Optimization,
        "-fsanitize=undefined,address",
        "-Werror=uninitialized",
        "-Werror=return-type",
        SourcePath,
        "-o",
        BinaryPath};
#if defined(__APPLE__) && defined(__aarch64__)
    // These x86-64 fixtures retain MXCSR and x86 floating-point helpers.
    // Compile for their ISA and execute with Rosetta on Apple Silicon.
    Arguments.push_back("-arch");
    Arguments.push_back("x86_64");
#endif
#ifdef _WIN32
    Arguments.push_back("-shared-libasan");
#endif
    std::string Error;
    int Result = llvm::sys::ExecuteAndWait(Compiler, Arguments, std::nullopt,
                                           Redirects, 60, 0, &Error);
    auto Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
    Result = llvm::sys::ExecuteAndWait(BinaryPath, {BinaryPath}, std::nullopt,
                                       Redirects, 30, 0, &Error);
    Errors = llvm::MemoryBuffer::getFile(ErrorPath);
    ASSERT_EQ(Result, 0) << Error
                         << (Errors ? (*Errors)->getBuffer().str() : "") << '\n'
                         << Source;
  }
}

TEST(SysVCallContract, PassThroughThisAndUndefinedReturnPathsStayFaithful) {
  const std::string Source = lift();
  const std::string SaveBody = body(Source, "save");
  const std::string IsDocBody = body(Source, "is_doc");
  ASSERT_FALSE(SaveBody.empty()) << Source;
  ASSERT_FALSE(IsDocBody.empty()) << Source;
  // A path that leaves RAX as the caller left it makes save void, and its
  // tail calls still run.
  EXPECT_EQ(SaveBody.rfind("void save(", 0), 0u) << SaveBody;
  EXPECT_EQ(SaveBody.find("unknown return value"), std::string::npos)
      << SaveBody;
  EXPECT_NE(SaveBody.find("helper("), std::string::npos) << SaveBody;
  EXPECT_NE(SaveBody.find("helper2("), std::string::npos) << SaveBody;
  // is_doc reads the RDI save never wrote: the call passes save's own node.
  EXPECT_EQ(SaveBody.find("is_doc()"), std::string::npos) << SaveBody;
  EXPECT_NE(SaveBody.find("is_doc("), std::string::npos) << SaveBody;
  // The virtual call takes the object the dominating block loaded into RDI.
  EXPECT_EQ(IsDocBody.find("+ 0x58)))()"), std::string::npos) << IsDocBody;
  compileAndRun(Source + R"(
static uint64_t document_type(void *self) { return self ? 9 : 0; }
static uint64_t element_type(void *self) { return self ? 1 : 0; }
struct impl {
  void **vtbl;
  int32_t field8;
};
int main(void) {
  void *document_vtbl[12] = {0}, *element_vtbl[12] = {0};
  document_vtbl[11] = (void *)document_type;
  element_vtbl[11] = (void *)element_type;
  struct impl document = {document_vtbl, 0}, element = {element_vtbl, 0};
  struct impl *documentNode = &document, *elementNode = &element, *null = 0;
  save(&documentNode);
  save(&elementNode);
  save(&null);
  return document.field8 == 1 && element.field8 == 2 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, MachOCallsPassTheRegistersTheirCalleesRead) {
  // Apple's x86-64 convention is the System V one: is_doc reads the RDI save
  // never wrote, so the call passes save's own node there as on ELF.
  const std::string Source = lift(BinaryFormat::MachO);
  const std::string SaveBody = body(Source, "save");
  ASSERT_FALSE(SaveBody.empty()) << Source;
  EXPECT_EQ(SaveBody.find("is_doc()"), std::string::npos) << SaveBody;
  EXPECT_NE(SaveBody.find("is_doc("), std::string::npos) << SaveBody;
}

TEST(SysVCallContract, ALowByteWriteIsTheReturnValueOnEveryPath) {
  // is_small(c): al = 0, set to 1 only for 1 <= c <= 4.  A comparison chain
  // reaches one shared ret, where AL merges, and one fall-through ret that
  // returns the cleared AL directly.  RAX above AL is the caller's.
  constexpr va_t IsZero = 0x401000;
  std::vector<uint8_t> Code = {0x30, 0xC0,             // xor al, al
                               0x40, 0x84, 0xFF,       // test dil, dil
                               0x74, 0x25,             // je done
                               0x40, 0x80, 0xFF, 0x04, // cmp dil, 4
                               0x76, 0x1D,             // jbe set
                               0x40, 0x80, 0xFF, 0x10, // cmp dil, 0x10
                               0x74, 0x19,             // je done
                               0x40, 0x80, 0xFF, 0x18, // cmp dil, 0x18
                               0x74, 0x13,             // je done
                               0x40, 0x80, 0xFF, 0x4F, // cmp dil, 0x4f
                               0x76, 0x0D,             // jbe done
                               0x40, 0x80, 0xFF, 0x51, // cmp dil, 0x51
                               0x76, 0x07,             // jbe done
                               0x40, 0x80, 0xC7, 0x08, // add dil, 8
                               0xC3,                   // ret
                               0xB0, 0x01,             // set: mov al, 1
                               0xC3};                  // done: ret
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Base = 0x400000;
  Img.Entry = IsZero;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = IsZero;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = Code;
  Img.Segments.push_back(std::move(Seg));
  Symbol Function = Symbol::makeFunc(IsZero, Code.size());
  Function.Name = "is_zero";
  Img.Symbols.push_back(std::move(Function));
  Img.KnownCodeRanges.push_back({IsZero, IsZero + Code.size()});
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries.insert(IsZero);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  EXPECT_EQ(Source.find("unknown return value"), std::string::npos) << Source;
  compileAndRun(Source + R"(
int main(void) {
  for (int c = 0; c < 256; ++c)
    if ((uint8_t)is_zero(c) != (c >= 1 && c <= 4))
      return 1;
  return 0;
}
)");
}

/// An x86-64 ELF image with \p Code at Text, the given function symbols, and
/// the imports \p Imports bound to consecutive slots at 0x403000, each with
/// a PLT-style stub `jmp [slot]` at the address paired with it.
BinaryImage
makeImportImage(std::vector<uint8_t> Code,
                const std::vector<std::pair<va_t, const char *>> &Functions,
                const std::vector<std::pair<va_t, const char *>> &Imports) {
  constexpr va_t Got = 0x403000;
  for (size_t I = 0; I < Imports.size(); ++I) {
    const va_t Stub = Imports[I].first;
    std::vector<uint8_t> Jump = {0xFF, 0x25}; // jmp [rip + slot]
    for (uint8_t B : rel32(Stub + 6, Got + 8 * I))
      Jump.push_back(B);
    put(Code, Stub, Jump);
  }
  BinaryImage Img;
  Img.Arch = Arch::X64;
  Img.Bits = Bitness::Bits64;
  Img.Format = BinaryFormat::ELF;
  Img.Base = 0x400000;
  Img.Entry = Text;
  Segment Seg;
  Seg.Name = ".text";
  Seg.VA = Text;
  Seg.Size = Code.size();
  Seg.Flags = SegmentFlags::Readable | SegmentFlags::Executable;
  Seg.Data = std::move(Code);
  Img.Segments.push_back(std::move(Seg));
  if (!Imports.empty()) {
    Segment GotSeg;
    GotSeg.Name = ".got";
    GotSeg.VA = Got;
    GotSeg.Size = GotSeg.FileSz = 8 * Imports.size();
    GotSeg.Data.resize(GotSeg.Size);
    GotSeg.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
    Img.Segments.push_back(std::move(GotSeg));
  }
  for (size_t I = 0; I < Imports.size(); ++I) {
    Import Imp;
    Imp.Module = "libc.so.6";
    Imp.Name = Imports[I].second;
    Imp.IATAddr = Got + 8 * I;
    Img.Imports.push_back(std::move(Imp));
    EXPECT_TRUE(Img.recordImportStub(Imports[I].first, I));
  }
  for (const auto &[Entry, Name] : Functions) {
    Symbol Function = Symbol::makeFunc(Entry);
    Function.Name = Name;
    Img.Symbols.push_back(std::move(Function));
  }
  return Img;
}

/// HighC for the functions at \p Entries of \p Img.
std::string liftEntries(const BinaryImage &Img, std::set<va_t> Entries) {
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = std::move(Entries);
  auto Result = Pipeline().run(Img, Ctx, Opts);
  EXPECT_TRUE(Result.Success) << Result.Error;
  if (const auto *Root = std::getenv("NEVERD_ISA_REGRESSION_ARTIFACT_DIR")) {
    llvm::SmallString<128> Directory(Root);
    llvm::sys::path::append(
        Directory,
        testing::UnitTest::GetInstance()->current_test_info()->name());
    EXPECT_FALSE(llvm::sys::fs::create_directories(Directory));
    llvm::SmallString<128> Path(Directory);
    llvm::sys::path::append(Path, "call-return-med.txt");
    std::error_code Error;
    llvm::raw_fd_ostream Dump(Path, Error);
    EXPECT_FALSE(Error);
    if (!Error)
      Pipeline::dumpMedIR(Result.MedFuncs, Dump);
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  EXPECT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  return Source;
}

TEST(SysVCallContract, PassThroughArgumentsReachAPrototypedImport) {
  // wrap(s, f) returns fputs(s, f) + 1 and fwd(s, f) tail-jumps to fputs,
  // both without writing RDI or RSI.  The prototype says fputs reads both,
  // so they are the functions' own parameters, and the stub passes them on.
  constexpr va_t Wrap = Text, Fwd = Text + 0x20, Stub = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> WrapCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                   0xE8};                  // call fputs
  for (uint8_t B : rel32(Wrap + 9, Stub))
    WrapCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0x83, 0xC0, 0x01,       // add eax, 1
                    0xC3})                  // ret
    WrapCode.push_back(B);
  put(Code, Wrap, WrapCode);
  std::vector<uint8_t> FwdCode = {0xE9}; // jmp fputs
  for (uint8_t B : rel32(Fwd + 5, Stub))
    FwdCode.push_back(B);
  put(Code, Fwd, FwdCode);
  const BinaryImage Img = makeImportImage(
      Code, {{Wrap, "wrap"}, {Fwd, "fwd"}, {Stub, "fputs_stub"}},
      {{Stub, "fputs"}});
  const std::string Source = liftEntries(Img, {Wrap, Fwd, Stub});
  for (const char *Name : {"wrap", "fwd", "fputs_stub"}) {
    SCOPED_TRACE(Name);
    const std::string Body = body(Source, Name);
    ASSERT_FALSE(Body.empty()) << Source;
    EXPECT_NE(
        Body.find("fputs((void *)(uintptr_t)arg0, (void *)(uintptr_t)arg1)"),
        std::string::npos)
        << Body;
    EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
  }
}

TEST(SysVCallContract, AFloatArgumentReachesTheCalleeThatReadsIt) {
  // twice(x) = x + x; quadruple(x) doubles twice(x), passing x on in xmm0
  // without touching it. The alignment `push rax` reads all of rax: it is no
  // variadic call's count of vector arguments.
  constexpr va_t Twice = Text, Quadruple = Text + 0x10;
  std::vector<uint8_t> Code(0x20, 0xCC);
  put(Code, Twice,
      {0xF2, 0x0F, 0x58, 0xC0,                 // addsd xmm0, xmm0
       0xC3});                                 // ret
  std::vector<uint8_t> QuadrupleCode = {0x50,  // push rax
                                        0xE8}; // call twice
  for (uint8_t B : rel32(Quadruple + 6, Twice))
    QuadrupleCode.push_back(B);
  for (uint8_t B : {0xF2, 0x0F, 0x58, 0xC0, // addsd xmm0, xmm0
                    0x59,                   // pop rcx
                    0xC3})                  // ret
    QuadrupleCode.push_back(B);
  put(Code, Quadruple, QuadrupleCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Twice, "twice"}, {Quadruple, "quadruple"}}, {});
  const std::string Source = liftEntries(Img, {Twice, Quadruple});
  EXPECT_NE(Source.find("double twice(double arg0)"), std::string::npos)
      << Source;
  const std::string Body = body(Source, "quadruple");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("double quadruple(double arg0)"), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("twice(arg0)"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  compileAndRun(Source + R"(
int main(void) {
  return quadruple(1.5) == 6.0 && quadruple(-0.25) == -1.0 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, PushedStackArgumentsReachTheCalleeThatReadsThem) {
  // ten(a, ..., j) reads its six register arguments and its four stack
  // arguments, the last in its own order: ((g * 10 + h) * 10 + i) * 10 + j.
  // caller(x) pushes j, i, h and then x as g, each at the stack pointer it
  // is made with, and passes x on in rdi.
  constexpr va_t Ten = Text, Caller = Text + 0x40;
  std::vector<uint8_t> Code(0x80, 0xCC);
  put(Code, Ten, {0x48, 0x8B, 0x44, 0x24, 0x08, // mov rax, [rsp+8]
                  0x48, 0x6B, 0xC0, 0x0A,       // imul rax, rax, 10
                  0x48, 0x03, 0x44, 0x24, 0x10, // add rax, [rsp+16]
                  0x48, 0x6B, 0xC0, 0x0A,       // imul rax, rax, 10
                  0x48, 0x03, 0x44, 0x24, 0x18, // add rax, [rsp+24]
                  0x48, 0x6B, 0xC0, 0x0A,       // imul rax, rax, 10
                  0x48, 0x03, 0x44, 0x24, 0x20, // add rax, [rsp+32]
                  0x48, 0x01, 0xF8,             // add rax, rdi
                  0x48, 0x01, 0xF0,             // add rax, rsi
                  0x48, 0x01, 0xD0,             // add rax, rdx
                  0x48, 0x01, 0xC8,             // add rax, rcx
                  0x4C, 0x01, 0xC0,             // add rax, r8
                  0x4C, 0x01, 0xC8,             // add rax, r9
                  0xC3});                       // ret
  std::vector<uint8_t> CallerCode = {
      0x6A, 0x04,                         // push 4
      0x6A, 0x03,                         // push 3
      0x6A, 0x02,                         // push 2
      0x57,                               // push rdi
      0xBE, 0x0A, 0x00, 0x00, 0x00,       // mov esi, 10
      0xBA, 0x14, 0x00, 0x00, 0x00,       // mov edx, 20
      0xB9, 0x1E, 0x00, 0x00, 0x00,       // mov ecx, 30
      0x41, 0xB8, 0x28, 0x00, 0x00, 0x00, // mov r8d, 40
      0x41, 0xB9, 0x32, 0x00, 0x00, 0x00, // mov r9d, 50
      0xE8};                              // call ten
  for (uint8_t B : rel32(Caller + CallerCode.size() + 4, Ten))
    CallerCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x20, // add rsp, 32
                    0xC3})                  // ret
    CallerCode.push_back(B);
  put(Code, Caller, CallerCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Ten, "ten"}, {Caller, "caller"}}, {});
  const std::string Source = liftEntries(Img, {Ten, Caller});
  const std::string Body = body(Source, "caller");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("ten(arg0, 10, 0x14, 0x1E, 0x28, 0x32, arg0, 2, 3, 4)"),
            std::string::npos)
      << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  compileAndRun(Source + R"(
int main(void) {
  return caller(5) == 1001 * 5 + 384 && caller(-7) == 1001 * -7 + 384 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, PushedArgumentsAfterAStackJoinKeepEverySlot) {
  // The branch leaves RSP at different entry offsets. All five pushes after
  // that join still have exact distances from the eventual call SP.
  constexpr va_t Eleven = Text, Caller = Text + 0x60;
  std::vector<uint8_t> Code(0xC0, 0xCC);
  put(Code, Eleven,
      {0x48, 0x8B, 0x44, 0x24, 0x08, // mov rax, [rsp+8]
       0x48, 0x6B, 0xC0, 0x0A,       // imul rax, rax, 10
       0x48, 0x03, 0x44, 0x24, 0x10, // add rax, [rsp+16]
       0x48, 0x6B, 0xC0, 0x0A, 0x48, 0x03, 0x44, 0x24, 0x18, 0x48, 0x6B,
       0xC0, 0x0A, 0x48, 0x03, 0x44, 0x24, 0x20, 0x48, 0x6B, 0xC0, 0x0A,
       0x48, 0x03, 0x44, 0x24, 0x28, 0x48, 0x01, 0xF8, // add rax, rdi
       0x48, 0x01, 0xF0,                               // add rax, rsi
       0x48, 0x01, 0xD0,                               // add rax, rdx
       0x48, 0x01, 0xC8,                               // add rax, rcx
       0x4C, 0x01, 0xC0,                               // add rax, r8
       0x4C, 0x01, 0xC8,                               // add rax, r9
       0xC3});
  std::vector<uint8_t> CallerCode = {
      0x55, 0x48, 0x89, 0xE5,             // push rbp; mov rbp, rsp
      0x85, 0xFF, 0x74, 0x06,             // test edi, edi; je other
      0x48, 0x83, 0xEC, 0x08,             // sub rsp, 8
      0xEB, 0x04,                         // jmp joined
      0x48, 0x83, 0xEC, 0x18,             // other: sub rsp, 24
      0x6A, 0x05, 0x6A, 0x04,             // joined: push 5; push 4
      0x6A, 0x03, 0x6A, 0x02,             // push 3; push 2
      0x6A, 0x01,                         // push 1
      0xBE, 0x02, 0x00, 0x00, 0x00,       // mov esi, 2
      0xBA, 0x03, 0x00, 0x00, 0x00,       // mov edx, 3
      0xB9, 0x04, 0x00, 0x00, 0x00,       // mov ecx, 4
      0x41, 0xB8, 0x05, 0x00, 0x00, 0x00, // mov r8d, 5
      0x41, 0xB9, 0x06, 0x00, 0x00, 0x00, // mov r9d, 6
      0xBF, 0x01, 0x00, 0x00, 0x00,       // mov edi, 1
      0xE8};
  for (uint8_t B : rel32(Caller + CallerCode.size() + 4, Eleven))
    CallerCode.push_back(B);
  for (uint8_t B : {0x48, 0x89, 0xEC, 0x5D, 0xC3}) // mov rsp, rbp; pop; ret
    CallerCode.push_back(B);
  put(Code, Caller, CallerCode);
  for (BinaryFormat Format : {BinaryFormat::ELF, BinaryFormat::MachO}) {
    SCOPED_TRACE(static_cast<unsigned>(Format));
    BinaryImage Img =
        makeImportImage(Code, {{Eleven, "eleven"}, {Caller, "caller"}}, {});
    Img.Format = Format;
    if (Format == BinaryFormat::MachO)
      for (auto &Symbol : Img.Symbols)
        Symbol.Name = "_" + Symbol.Name;
    const std::string Source = liftEntries(Img, {Eleven, Caller});
    const std::string Body = body(Source, "caller");
    EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
    compileAndRun(Source + R"(
int main(void) {
  return caller(0) == 12366 && caller(1) == 12366 && caller(-7) == 12366
             ? 0 : 1;
}
)");
  }
}

TEST(SysVCallContract, AFloatArgumentReachesAPrototypedImport) {
  // twice_sin(x) = 2 * sin(x + x): math.h declares sin's double parameter,
  // so the sum in xmm0 is its argument and x the function's own parameter.
  constexpr va_t TwiceSin = Text, Stub = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> TwiceSinCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                       0xF2, 0x0F, 0x58,
                                       0xC0,  // addsd xmm0, xmm0
                                       0xE8}; // call sin
  for (uint8_t B : rel32(TwiceSin + 13, Stub))
    TwiceSinCode.push_back(B);
  for (uint8_t B : {0xF2, 0x0F, 0x58, 0xC0, // addsd xmm0, xmm0
                    0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0xC3})                  // ret
    TwiceSinCode.push_back(B);
  put(Code, TwiceSin, TwiceSinCode);
  const BinaryImage Img = makeImportImage(
      Code, {{TwiceSin, "twice_sin"}, {Stub, "sin_stub"}}, {{Stub, "sin"}});
  const std::string Source = liftEntries(Img, {TwiceSin, Stub});
  const std::string Body = body(Source, "twice_sin");
  ASSERT_FALSE(Body.empty());
  EXPECT_NE(Body.find("double twice_sin(double arg0)"), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("sin("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  // A declared double import must receive the sum and its result must feed
  // the second operation. Execute a known independent implementation so
  // explicit FP state temporaries cannot hide a stale ABI value.
  compileAndRun("#include <math.h>\n#define sin neverd_test_sine\n"
                "double sin(double);\n" +
                Source + R"(
double sin(double value) {
  _mm_setcsr(0x3f81);
  return value * value;
}
int main(void) {
  uint32_t saved = _mm_getcsr();
  _mm_setcsr(0x1f80);
  double first = twice_sin(1.5);
  uint32_t first_state = _mm_getcsr();
  _mm_setcsr(0x1f80);
  double second = twice_sin(-0.25);
  uint32_t second_state = _mm_getcsr();
  _mm_setcsr(saved);
  return first == 18.0 && second == 0.5 &&
         first_state == 0x3f81 && second_state == 0x3f81 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, AScalarMathImportWithAnIntegerArgumentKeepsItsReturn) {
  constexpr va_t Wrap = Text, Stub = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> WrapCode = {0x48, 0x83, 0xEC, 0x08,       // sub rsp, 8
                                   0xBF, 0x02, 0x00, 0x00, 0x00, // mov edi, 2
                                   0xE8};                        // call ldexp
  for (uint8_t B : rel32(Wrap + WrapCode.size() + 4, Stub))
    WrapCode.push_back(B);
  for (uint8_t B : {0xF2, 0x0F, 0x58, 0xC0, // addsd xmm0, xmm0
                    0x48, 0x83, 0xC4, 0x08, 0xC3})
    WrapCode.push_back(B);
  put(Code, Wrap, WrapCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Wrap, "twice_ldexp"}}, {{Stub, "ldexp"}});
  const std::string Source = liftEntries(Img, {Wrap});
  compileAndRun("#include <math.h>\n#define ldexp neverd_test_ldexp\n"
                "double ldexp(double, int);\n" +
                Source + R"(
double ldexp(double value, int exponent) {
  return exponent == 2 ? value * 4.0 : -100.0;
}
int main(void) {
  return twice_ldexp(1.5) == 12.0 && twice_ldexp(-0.25) == -2.0 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, ALocalMathNameDoesNotOverrideMixedReturnEvidence) {
  for (const auto &[Name, Imported] : {std::pair{"sin", false},
                                       {"custom", true},
                                       {"cpow", true},
                                       {"sinl", true}}) {
    SCOPED_TRACE(Name);
    constexpr va_t Stub = Text + 0x20;
    BinaryImage Img = makeImportImage(
        std::vector<uint8_t>(0x30, 0xCC), {{Stub, Name}},
        Imported ? std::vector<std::pair<va_t, const char *>>{{Stub, Name}}
                 : std::vector<std::pair<va_t, const char *>>{});
    LowFunc Low;
    Low.Entry = Text;
    Low.Name = "mixed_caller";
    LowBlock Block;
    Block.Id = 0;
    Block.StartAddr = Text;
    Block.EndAddr = Text + 16;
    LowOp Call;
    Call.Opcode = NdOp::CALL;
    Call.Output = NdVar::reg(x86reg::RAX, 8);
    Call.addInput(NdVar::cst(Stub, 8));
    Call.Addr = Text;
    Block.Ops.push_back(Call);
    for (const auto &[Address, Register] :
         {std::pair<uint64_t, uint64_t>{0x404000, x86reg::RAX},
          {0x404008, x86reg::XMM0}}) {
      LowOp Store;
      Store.Opcode = NdOp::STORE;
      Store.addInput(NdVar::cst(Address, 8));
      Store.addInput(NdVar::reg(Register, 8));
      Store.Addr = Text + 4;
      Block.Ops.push_back(Store);
    }
    LowOp Return;
    Return.Opcode = NdOp::RETURN;
    Return.addInput(NdVar::reg(x86reg::RAX, 8));
    Return.Addr = Text + 12;
    Block.Ops.push_back(Return);
    Low.Blocks.push_back(Block);
    LowToMedConverter Converter;
    Converter.setBinaryImage(&Img);
    const auto Med = Converter.convert(Low, Arch::X64);
    unsigned Calls = 0;
    for (const auto &B : Med.Blocks)
      for (const auto &Op : B.Ops)
        if (Op.Opcode == NdOp::CALL) {
          ++Calls;
          EXPECT_EQ(Op.Output.Kind, MedVar::Temp);
          EXPECT_EQ(Op.Output.Size, 16);
        }
    EXPECT_EQ(Calls, 1U);
  }
}

TEST(SysVCallContract, ACallerReturnsTheFloatItsImportReturns) {
  // sin_twice(x) = sin(x + x): the result sin leaves in xmm0 is the one
  // sin_twice returns, though no instruction of its own writes it.
  constexpr va_t SinTwice = Text, Stub = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> SinTwiceCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                       0xF2, 0x0F, 0x58,
                                       0xC0,  // addsd xmm0, xmm0
                                       0xE8}; // call sin
  for (uint8_t B : rel32(SinTwice + 13, Stub))
    SinTwiceCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0xC3})                  // ret
    SinTwiceCode.push_back(B);
  put(Code, SinTwice, SinTwiceCode);
  const BinaryImage Img = makeImportImage(
      Code, {{SinTwice, "sin_twice"}, {Stub, "sin_stub"}}, {{Stub, "sin"}});
  const std::string Source = liftEntries(Img, {SinTwice, Stub});
  const std::string Body = body(Source, "sin_twice");
  ASSERT_FALSE(Body.empty());
  EXPECT_NE(Body.find("double sin_twice(double arg0)"), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("return sin("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  compileAndRun("#include <math.h>\n#define sin neverd_test_sine\n"
                "double sin(double);\n" +
                Source + R"(
double sin(double value) { return value + 0.5; }
int main(void) {
  return sin_twice(1.5) == 3.5 && sin_twice(-0.25) == 0.0 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, AForwarderReturnsTheFloatItsCalleeReturns) {
  // forward(x) = twice(x) passes x on and returns twice's result in xmm0,
  // writing neither itself.
  constexpr va_t Twice = Text, Forward = Text + 0x10;
  std::vector<uint8_t> Code(0x20, 0xCC);
  put(Code, Twice,
      {0xF2, 0x0F, 0x58, 0xC0,               // addsd xmm0, xmm0
       0xC3});                               // ret
  std::vector<uint8_t> ForwardCode = {0x50,  // push rax
                                      0xE8}; // call twice
  for (uint8_t B : rel32(Forward + 6, Twice))
    ForwardCode.push_back(B);
  for (uint8_t B : {0x59,  // pop rcx
                    0xC3}) // ret
    ForwardCode.push_back(B);
  put(Code, Forward, ForwardCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Twice, "twice"}, {Forward, "forward"}}, {});
  const std::string Source = liftEntries(Img, {Twice, Forward});
  const std::string Body = body(Source, "forward");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("double forward(double arg0)"), std::string::npos)
      << Body;
  EXPECT_NE(Body.find("twice(arg0)"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  compileAndRun(Source + R"(
int main(void) {
  return forward(1.5) == 3.0 && forward(-0.25) == -0.5 ? 0 : 1;
}
)");
}

/// Map \p Bytes read-only at \p At in \p Img, where a constant format lives.
void addReadOnlyData(BinaryImage &Img, va_t At, const std::string &Bytes) {
  Segment Data;
  Data.Name = ".rodata";
  Data.VA = At;
  Data.Size = Data.FileSz = Bytes.size() + 1;
  Data.Data.assign(Bytes.begin(), Bytes.end());
  Data.Data.push_back(0);
  Data.Flags = SegmentFlags::Readable;
  Img.Segments.push_back(std::move(Data));
}

TEST(SysVCallContract, AFormatNamesTheArgumentsAFormattedCallPasses) {
  // show(a, s, x, b, f) = printf("%d %s %f %d %f\n", a, s, x, b, f): x stays
  // in xmm0 and f widens into xmm1, and AL counts the two doubles.  pass(...)
  // calls show with its own arguments: show reads xmm0 only by passing it on.
  constexpr va_t Show = Text, Pass = Text + 0x30, Stub = Text + 0x40;
  constexpr va_t Format = 0x402000;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> ShowCode = {0x89, 0xD1,             // mov ecx, edx
                                   0x48, 0x89, 0xF2,       // mov rdx, rsi
                                   0x89, 0xFE,             // mov esi, edi
                                   0xF3, 0x0F, 0x5A, 0xC9, // cvtss2sd xmm1
                                   0x48, 0x8D, 0x3D};      // lea rdi, [fmt]
  for (uint8_t B : rel32(Show + 18, Format))
    ShowCode.push_back(B);
  for (uint8_t B : {0xB0, 0x02, // mov al, 2
                    0xE9})      // jmp printf
    ShowCode.push_back(B);
  for (uint8_t B : rel32(Show + 25, Stub))
    ShowCode.push_back(B);
  put(Code, Show, ShowCode);
  std::vector<uint8_t> PassCode = {0x50,  // push rax
                                   0xE8}; // call show
  for (uint8_t B : rel32(Pass + 6, Show))
    PassCode.push_back(B);
  for (uint8_t B : {0x59,  // pop rcx
                    0xC3}) // ret
    PassCode.push_back(B);
  put(Code, Pass, PassCode);
  BinaryImage Img = makeImportImage(
      Code, {{Show, "show"}, {Pass, "pass"}, {Stub, "printf_stub"}},
      {{Stub, "printf"}});
  addReadOnlyData(Img, Format, "%d %s %f %d %f\n");
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Show, Pass, Stub};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  // show reads a, s and b, x by passing it on, and the low lane of f.
  const auto Reads = Result.CallEntryReadGPRs.find(Show);
  ASSERT_NE(Reads, Result.CallEntryReadGPRs.end());
  EXPECT_EQ(Reads->second[x86reg::RDI / 8], 4);
  EXPECT_EQ(Reads->second[x86reg::RSI / 8], 8);
  EXPECT_EQ(Reads->second[x86reg::RDX / 8], 4);
  EXPECT_EQ(Reads->second[kX64VectorFamilyBase], 8);
  EXPECT_EQ(Reads->second[kX64VectorFamilyBase + 1], 4);
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  const std::string ShowBody = body(Source, "show");
  ASSERT_FALSE(ShowBody.empty()) << Source;
  // CVTSS2SD retains MXCSR status and denormal behavior. Its double result,
  // rather than a host-language cast, occupies the final variadic argument.
  llvm::SmallVector<llvm::StringRef, 3> Widened;
  ASSERT_TRUE(
      llvm::Regex(
          R"((t[0-9]+) = neverd_x86_cvtss2sd_source32\(__builtin_bit_cast\(uint32_t, \(float\)\(arg4\)\), (t[0-9]+)\);)")
          .match(ShowBody, &Widened))
      << ShowBody;
  EXPECT_NE(ShowBody.find(Widened[2].str() + " = neverd_x86_read_mxcsr();"),
            std::string::npos)
      << ShowBody;
  const size_t FormatCall =
      ShowBody.find("printf(\"%d %s %f %d %f\\n\", arg0, (void*)arg1, "
                    "arg3, arg2, ");
  ASSERT_NE(FormatCall, std::string::npos) << ShowBody;
  // Tie the actual variadic argument to the checked conversion result while
  // allowing equivalent source casts and storage representations.
  const size_t FormatEnd = ShowBody.find(';', FormatCall);
  EXPECT_NE(ShowBody.substr(FormatCall, FormatEnd - FormatCall)
                .find(Widened[1].str()),
            std::string::npos)
      << ShowBody;
  const std::string PassBody = body(Source, "pass");
  ASSERT_FALSE(PassBody.empty()) << Source;
  EXPECT_NE(PassBody.find("show(arg0, arg1, arg2, arg3, arg4)"),
            std::string::npos)
      << PassBody;
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
}

TEST(SysVCallContract, AFormattedCallFormatsTheValuesItPasses) {
  // fmt_show(buf, n, a, x) = snprintf(buf, n, "%d %.2f", a, x): the
  // recompiled call writes what the original does.
  constexpr va_t FmtShow = Text, Stub = Text + 0x20, Format = 0x402000;
  std::vector<uint8_t> Code(0x30, 0xCC);
  std::vector<uint8_t> FmtShowCode = {0x89, 0xD1,        // mov ecx, edx
                                      0x48, 0x8D, 0x15}; // lea rdx, [fmt]
  for (uint8_t B : rel32(FmtShow + 9, Format))
    FmtShowCode.push_back(B);
  for (uint8_t B : {0xB0, 0x01, // mov al, 1
                    0xE9})      // jmp snprintf
    FmtShowCode.push_back(B);
  for (uint8_t B : rel32(FmtShow + 16, Stub))
    FmtShowCode.push_back(B);
  put(Code, FmtShow, FmtShowCode);
  BinaryImage Img =
      makeImportImage(Code, {{FmtShow, "fmt_show"}, {Stub, "snprintf_stub"}},
                      {{Stub, "snprintf"}});
  addReadOnlyData(Img, Format, "%d %.2f");
  const std::string Source = liftEntries(Img, {FmtShow});
  const std::string Body = body(Source, "fmt_show");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("\"%d %.2f\", arg2, arg3)"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  compileAndRun(Source + R"(
#include <string.h>
int main(void) {
  char Text[32];
  int (*Call)(char *, unsigned long, int, double) =
      (int (*)(char *, unsigned long, int, double))(void *)fmt_show;
  Call(Text, sizeof Text, 42, 3.25);
  return strcmp(Text, "42 3.25") == 0 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, ASharedCallTakesTheFormatAnotherBodyGivesIt) {
  // b(x) = printf("%d", x) loads the format and calls; a jumps straight to
  // that call with its own registers, a(f, x) = printf(f, x).  A run that
  // lifts b before a makes the call they share a formatted one, which reads
  // the value after the format, for a too.  a's facts, kept from a run that
  // reached a alone, read an ordinary call and must not stand in for it.
  constexpr va_t B = Text, Call = B + 7, A = Text + 0x10, Q = Text + 0x20,
                 R = Text + 0x30, Stub = Text + 0x40, Format = 0x402000;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> BCode = {0x48, 0x8D, 0x3D}; // lea rdi, [fmt]
  for (uint8_t Byte : rel32(Call, Format))
    BCode.push_back(Byte);
  BCode.push_back(0xE8); // call printf
  for (uint8_t Byte : rel32(Call + 5, Stub))
    BCode.push_back(Byte);
  BCode.push_back(0xC3); // ret
  put(Code, B, BCode);
  std::vector<uint8_t> ACode = {0xE9}; // jmp to b's call
  for (uint8_t Byte : rel32(A + 5, Call))
    ACode.push_back(Byte);
  put(Code, A, ACode);
  std::vector<uint8_t> QCode = {0xE8}; // call b
  for (uint8_t Byte : rel32(Q + 5, B))
    QCode.push_back(Byte);
  QCode.push_back(0xE8); // call a
  for (uint8_t Byte : rel32(Q + 10, A))
    QCode.push_back(Byte);
  QCode.push_back(0xC3); // ret
  put(Code, Q, QCode);
  std::vector<uint8_t> RCode = {0xE8}; // call a
  for (uint8_t Byte : rel32(R + 5, A))
    RCode.push_back(Byte);
  RCode.push_back(0xC3); // ret
  put(Code, R, RCode);
  BinaryImage Img = makeImportImage(
      Code, {{B, "b"}, {A, "a"}, {Q, "q"}, {R, "r"}}, {{Stub, "printf"}});
  addReadOnlyData(Img, Format, "%d");
  const auto Run = [&](va_t Entry, PipelineAnalysisCache *Cache) {
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.OnlyFunctionEntries = {Entry};
    Opts.AnalysisCache = Cache;
    return Pipeline().run(Img, Ctx, Opts);
  };
  PipelineAnalysisCache Cache;
  for (va_t Root : {R, Q}) {
    SCOPED_TRACE(Root);
    const PipelineResult Kept = Run(Root, &Cache), Cold = Run(Root, nullptr);
    EXPECT_EQ(Kept.CallEntryReadGPRs, Cold.CallEntryReadGPRs);
    EXPECT_EQ(Kept.CallMayWriteGPRs, Cold.CallMayWriteGPRs);
    EXPECT_EQ(Kept.CallEntryStackArgs, Cold.CallEntryStackArgs);
  }
}

TEST(SysVCallContract, AVariadicImportsFixedArgumentsReachItFromAJoin) {
  // wrap(flag) picks the format in either arm of an `if` and calls
  // __fprintf_chk(0, 2, fmt) after the join: the format reaches the call as
  // the value of EDX on both paths, not as a value the call site never set.
  constexpr va_t Wrap = Text, Stub = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> WrapCode = {
      0x48, 0x83, 0xEC, 0x08,       // sub rsp, 8
      0x85, 0xFF,                   // test edi, edi
      0x74, 0x07,                   // je L1
      0xBA, 0x11, 0x11, 0x00, 0x00, // mov edx, 0x1111
      0xEB, 0x05,                   // jmp L2
      0xBA, 0x22, 0x22, 0x00, 0x00, // L1: mov edx, 0x2222
      0xBE, 0x02, 0x00, 0x00, 0x00, // L2: mov esi, 2
      0x31, 0xFF,                   // xor edi, edi
      0x31, 0xC0,                   // xor eax, eax
      0xE8};                        // call __fprintf_chk
  for (uint8_t B : rel32(Wrap + WrapCode.size() + 4, Stub))
    WrapCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0xC3})                  // ret
    WrapCode.push_back(B);
  put(Code, Wrap, WrapCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Wrap, "wrap"}}, {{Stub, "__fprintf_chk"}});
  const std::string Source = liftEntries(Img, {Wrap});
  const std::string Body = body(Source, "wrap");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("__fprintf_chk("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(SysVCallContract, ACallTwoPathsShareTakesTheArgumentsBothFormatsName) {
  // report(flag, n, s, c) prints n with "%lu%c" in one arm and s with
  // "%s%c" in the other, and both arms jump to one __fprintf_chk call, as
  // sed's cross-jumped calls do.  Each arm sets the format and its two
  // arguments; the call passes them all whichever arm ran.  Formats that name
  // their arguments in different registers say nothing about the call.
  constexpr va_t Report = Text, Stub = Text + 0x40, First = 0x402000,
                 Second = 0x402010;
  const auto Lift = [&](const char *FirstFormat, const char *SecondFormat) {
    std::vector<uint8_t> Code(0x50, 0xCC);
    std::vector<uint8_t> ReportCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                       0x85, 0xFF,             // test edi, edi
                                       0x74, 0x10,             // je L1
                                       0x44, 0x0F, 0xBE, 0xC1, // movsx r8d, cl
                                       0x48, 0x89, 0xF1,       // mov rcx, rsi
                                       0x48, 0x8D, 0x15}; // lea rdx, [first]
    for (uint8_t B : rel32(Report + 22, First))
      ReportCode.push_back(B);
    for (uint8_t B : {0xEB, 0x0E,             // jmp L2
                      0x44, 0x0F, 0xBE, 0xC1, // L1: movsx r8d, cl
                      0x48, 0x89, 0xD1,       // mov rcx, rdx
                      0x48, 0x8D, 0x15})      // lea rdx, [second]
      ReportCode.push_back(B);
    for (uint8_t B : rel32(Report + 38, Second))
      ReportCode.push_back(B);
    for (uint8_t B : {0xBE, 0x02, 0x00, 0x00, 0x00, // L2: mov esi, 2
                      0x31, 0xFF,                   // xor edi, edi
                      0x31, 0xC0,                   // xor eax, eax
                      0xE8})                        // call __fprintf_chk
      ReportCode.push_back(B);
    for (uint8_t B : rel32(Report + 52, Stub))
      ReportCode.push_back(B);
    for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                      0xC3})                  // ret
      ReportCode.push_back(B);
    put(Code, Report, ReportCode);
    BinaryImage Img =
        makeImportImage(Code, {{Report, "report"}}, {{Stub, "__fprintf_chk"}});
    addReadOnlyData(Img, First, FirstFormat);
    addReadOnlyData(Img, Second, SecondFormat);
    const std::string Source = liftEntries(Img, {Report});
    return body(Source, "report");
  };
  const llvm::Regex FiveArguments(
      "__fprintf_chk\\([^,]+, 2, [^,]+, [^,]+, [^,]+\\);");
  const std::string Shared = Lift("%lu%c", "%s%c");
  ASSERT_FALSE(Shared.empty());
  EXPECT_TRUE(FiveArguments.match(Shared)) << Shared;
  const std::string Different = Lift("%lu%c", "%s");
  ASSERT_FALSE(Different.empty());
  EXPECT_NE(Different.find("__fprintf_chk("), std::string::npos) << Different;
  EXPECT_FALSE(FiveArguments.match(Different)) << Different;
}

TEST(SysVCallContract, JoinedFormatsRequireCompleteCompatibleReachingValues) {
  constexpr va_t Stub = Text + 0x80, First = 0x402000, Second = First + 0x20;
  for (const auto Format : {BinaryFormat::ELF, BinaryFormat::MachO})
    for (bool Reverse : {false, true})
      for (unsigned Case = 0; Case < 15; ++Case) {
        SCOPED_TRACE(static_cast<int>(Format));
        SCOPED_TRACE(Reverse);
        SCOPED_TRACE(Case);
        BinaryImage Img =
            makeImportImage(std::vector<uint8_t>(0x90, 0xCC),
                            {{Text, "report"}}, {{Stub, "printf"}});
        Img.Format = Format;
        if (Format == BinaryFormat::MachO)
          Img.Imports.front().Name = "_printf";
        addReadOnlyData(Img, First,
                        Case == 1    ? "%p%c"
                        : Case >= 11 ? "%f%c"
                                     : "%llu%c");
        const char *Other = Case == 2   ? "%s"
                            : Case == 3 ? "%f%c"
                            : Case == 4 ? "%n%c"
                                        : "%s%c";
        addReadOnlyData(Img, Second, Case >= 11 ? "%f%c" : Other);
        auto Op = [](NdOp Code, NdVar Output,
                     std::initializer_list<NdVar> Inputs) {
          LowOp O;
          O.Opcode = Code;
          O.Output = Output;
          for (const auto &Input : Inputs)
            O.addInput(Input);
          return O;
        };
        LowFunc F;
        F.Entry = Text;
        F.Blocks.resize(4);
        for (int I = 0; I < 4; ++I) {
          F.Blocks[I].Id = I;
          F.Blocks[I].StartAddr = Text + 0x10 * I;
        }
        F.Blocks[0].Succs = {1, 2};
        for (int I : {1, 2}) {
          auto &B = F.Blocks[I];
          B.Preds = {0};
          B.Succs = {3};
          B.Ops = {Op(NdOp::COPY, NdVar::reg(x86reg::RDI, 8),
                      {NdVar::cst(I == 1 ? First : Second, 8)})};
        }
        auto &Join = F.Blocks[3];
        Join.Preds = Reverse ? std::vector<int>{2, 1} : std::vector<int>{1, 2};
        Join.Ops = {
            Op(NdOp::COPY, NdVar::reg(x86reg::RAX, 4), {NdVar::cst(0, 4)}),
            Op(NdOp::CALL, {}, {NdVar::cst(Stub, 8)})};
        Join.Ops.back().Addr = Join.StartAddr;
        if (Case == 5)
          F.Blocks[2].Ops.clear();
        if (Case == 6)
          F.Blocks[2].Ops.push_back(
              Op(NdOp::COPY, NdVar::reg(x86reg::RDI, 1), {NdVar::cst(0, 1)}));
        if (Case == 7)
          F.Blocks[2].Ops.push_back(
              Op(NdOp::CALL, {}, {NdVar::cst(Stub + 0x10, 8)}));
        if (Case == 8)
          Join.Preds.push_back(99);
        if (Case == 9) {
          ExceptionalEdge Edge;
          Edge.BlockId = 0;
          Join.ExceptionalPreds.push_back(Edge);
        }
        if (Case == 10)
          F.OrdinaryModuleAnalysisRoots.insert(Join.StartAddr);
        if (Case >= 11) {
          // The vector count reaches the call from its predecessors too.
          // A zero AL cannot supply the double the format names.
          for (int I : {1, 2}) {
            const unsigned Count = Case == 11 || (Case == 13 && I == 2) ? 0
                                   : Case == 14 && I == 2               ? 257
                                                                        : 1;
            F.Blocks[I].Ops.push_back(Op(NdOp::COPY, NdVar::reg(x86reg::RAX, 4),
                                         {NdVar::cst(Count, 4)}));
          }
          Join.Ops.erase(Join.Ops.begin());
        }
        if (Reverse)
          std::swap(F.Blocks[1], F.Blocks[2]);
        std::map<va_t, FormattedCall> Calls;
        pipeline_detail::collectFormattedCalls(Img, F, Calls);
        const bool Floating = Case == 12 || Case == 14;
        if (Case >= 2 && !Floating) {
          EXPECT_TRUE(Calls.empty());
          continue;
        }
        ASSERT_EQ(Calls.size(), 1u);
        const auto &Arguments = Calls.begin()->second.Arguments;
        ASSERT_EQ(Arguments.size(), 3u);
        EXPECT_EQ(Arguments[1].Register, Floating ? x86reg::XMM0 : x86reg::RSI);
        EXPECT_EQ(Arguments[1].Bytes, 8);
        EXPECT_EQ(Arguments[1].Floating, Floating);
        EXPECT_EQ(Arguments[1].Pointer, Case == 1);
        EXPECT_EQ(Arguments[2].Register, Floating ? x86reg::RSI : x86reg::RDX);
        EXPECT_EQ(Arguments[2].Bytes, 4);
      }
}

TEST(SysVCallContract, JoinedFormatsKeepEntryValuesAndProofLimitsUnknown) {
  constexpr va_t Stub = Text + 0x400, Format = 0x402000;
  for (unsigned Case = 0; Case < 4; ++Case) {
    SCOPED_TRACE(Case);
    auto Img = makeImportImage(std::vector<uint8_t>(0x410, 0xCC),
                               {{Text, "report"}}, {{Stub, "printf"}});
    addReadOnlyData(Img, Format, "%d%c");
    auto Copy = [](va_t Address) {
      LowOp O;
      O.Opcode = NdOp::COPY;
      O.Output = NdVar::reg(x86reg::RDI, 8);
      O.addInput(NdVar::cst(Address, 8));
      return O;
    };
    auto Call = [&](va_t Address) {
      LowOp O;
      O.Opcode = NdOp::CALL;
      O.Addr = Address;
      O.addInput(NdVar::cst(Stub, 8));
      return O;
    };
    LowFunc F;
    F.Entry = Text;
    auto Initialize = [&](unsigned Count) {
      F.Blocks.resize(Count);
      for (unsigned I = 0; I < Count; ++I) {
        F.Blocks[I].Id = I;
        F.Blocks[I].StartAddr = Text + 0x10 * I;
      }
    };
    if (Case == 0) {
      // A real initialization reaches the loop on its first iteration.
      Initialize(4);
      F.Blocks[0].Ops = {Copy(Format)};
      F.Blocks[0].Succs = {1};
      F.Blocks[1].Preds = {0, 2};
      F.Blocks[1].Succs = {2, 3};
      F.Blocks[2].Preds = {1};
      F.Blocks[2].Succs = {1};
      F.Blocks[3].Preds = {1};
      F.Blocks[3].Ops = {Call(F.Blocks[3].StartAddr)};
    } else if (Case == 1) {
      // A backedge cannot initialize a format on entry to the function.
      Initialize(2);
      F.Blocks[0].Preds = {1};
      F.Blocks[0].Succs = {1};
      F.Blocks[0].Ops = {Call(Text)};
      F.Blocks[1].Preds = {0};
      F.Blocks[1].Succs = {0};
      F.Blocks[1].Ops = {Copy(Format)};
    } else if (Case == 2) {
      // More alternatives than the bounded set retains prove no format.
      Initialize(7);
      for (int I = 1; I <= 5; ++I) {
        const va_t Address = Format + 0x20 * I;
        addReadOnlyData(Img, Address, "%d%c");
        F.Blocks[0].Succs.push_back(I);
        F.Blocks[I].Preds = {0};
        F.Blocks[I].Succs = {6};
        F.Blocks[I].Ops = {Copy(Address)};
        F.Blocks[6].Preds.push_back(I);
      }
      F.Blocks[6].Ops = {Call(F.Blocks[6].StartAddr)};
    } else {
      // An early call becomes known, but the reverse-ordered long sibling
      // path exhausts the fixed-point budget. Publish no partial result.
      Initialize(18);
      F.Blocks[0].Ops = {Copy(Format)};
      F.Blocks[0].Succs = {1, 3};
      F.Blocks[1].Preds = {0};
      F.Blocks[1].Succs = {2};
      F.Blocks[2].Preds = {1};
      F.Blocks[2].Ops = {Call(F.Blocks[2].StartAddr)};
      for (int I = 3; I < 18; ++I) {
        F.Blocks[I].Preds = {I == 3 ? 0 : I - 1};
        if (I + 1 < 18)
          F.Blocks[I].Succs = {I + 1};
      }
      std::reverse(F.Blocks.begin() + 1, F.Blocks.end());
    }
    std::map<va_t, FormattedCall> Calls;
    pipeline_detail::collectFormattedCalls(Img, F, Calls);
    EXPECT_EQ(Calls.size(), Case == 0 ? 1u : 0u);
  }
}

TEST(SysVCallContract, AJoinedFormatPrintsEveryArgumentOnBothPaths) {
  // report(buf, size, flag, n, s, c) shares one snprintf after choosing
  // "%llu%c" or "%s%c". Compile and execute both arms with a number whose
  // upper word is nonzero and a real string pointer.
  constexpr va_t Stub = Text + 0x40, First = 0x402000, Second = First + 0x20;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> Report = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                 0x85, 0xD2,             // test edx, edx
                                 0x74, 0x0D,             // je string
                                 0x45, 0x0F, 0xBE, 0xC1, // movsx r8d, r9b
                                 0x48, 0x8D, 0x15};      // lea rdx, [first]
  for (uint8_t B : rel32(Text + 19, First))
    Report.push_back(B);
  for (uint8_t B : {0xEB, 0x0E,             // jmp call
                    0x4C, 0x89, 0xC1,       // string: mov rcx, r8
                    0x45, 0x0F, 0xBE, 0xC1, // movsx r8d, r9b
                    0x48, 0x8D, 0x15})      // lea rdx, [second]
    Report.push_back(B);
  for (uint8_t B : rel32(Text + 35, Second))
    Report.push_back(B);
  for (uint8_t B : {0x31, 0xC0, 0xE8}) // xor eax, eax; call snprintf
    Report.push_back(B);
  for (uint8_t B : rel32(Text + 42, Stub))
    Report.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, 0xC3})
    Report.push_back(B);
  put(Code, Text, Report);
  auto Img =
      makeImportImage(Code, {{Text, "joined_report"}}, {{Stub, "snprintf"}});
  addReadOnlyData(Img, First, "%llu%c");
  addReadOnlyData(Img, Second, "%s%c");
  const auto Source = liftEntries(Img, {Text});
  compileAndRun(Source + R"(
#include <string.h>
int main(void) {
  char Buffer[64];
  int (*Call)(char *, uint64_t, int, uint64_t, const char *, int) =
      (int (*)(char *, uint64_t, int, uint64_t, const char *, int))(void *)joined_report;
  if (Call(Buffer, sizeof Buffer, 1, UINT64_C(4294967303), "joined", '!') != 11 ||
      strcmp(Buffer, "4294967303!")) return 1;
  if (Call(Buffer, sizeof Buffer, 0, UINT64_C(4294967303), "joined", '?') != 7 ||
      strcmp(Buffer, "joined?")) return 2;
  return 0;
}
)");
}

TEST(SysVCallContract, AVariadicImportTakesTheParameterPassedThroughToIt) {
  // reader(path, flags) opens path with flags & 3 and writer(path, flags)
  // creates it with a mode, as less and gzip do.  Each reads ESI itself, so
  // RDI is the parameter before it, which the body never reads: the entry
  // holds no copy of it, yet it reaches open as it entered.  open is
  // variadic, so no prototype says what its stub reads.  less's unit called
  // open() with no argument.
  constexpr va_t Reader = Text, Writer = Text + 0x20, Stub = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  auto Define = [&](va_t At, std::vector<uint8_t> Setup) {
    std::vector<uint8_t> Bytes = {0x48, 0x83, 0xEC, 0x08}; // sub rsp, 8
    Bytes.insert(Bytes.end(), Setup.begin(), Setup.end());
    Bytes.push_back(0xE8); // call open
    for (uint8_t B : rel32(At + Bytes.size() + 4, Stub))
      Bytes.push_back(B);
    for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                      0xC3})                  // ret
      Bytes.push_back(B);
    put(Code, At, Bytes);
  };
  Define(Reader, {0x83, 0xE6, 0x03,             // and esi, 3
                  0x31, 0xC0});                 // xor eax, eax
  Define(Writer, {0x83, 0xCE, 0x41,             // or esi, O_CREAT | O_WRONLY
                  0xBA, 0xA4, 0x01, 0x00, 0x00, // mov edx, 0644
                  0x31, 0xC0});                 // xor eax, eax
  const BinaryImage Img = makeImportImage(
      Code, {{Reader, "reader"}, {Writer, "writer"}}, {{Stub, "open"}});
  const std::string Source = liftEntries(Img, {Reader, Writer});
  const std::string Read = body(Source, "reader");
  const std::string Write = body(Source, "writer");
  ASSERT_FALSE(Read.empty()) << Source;
  ASSERT_FALSE(Write.empty()) << Source;
  EXPECT_EQ(Read.find("open()"), std::string::npos) << Read;
  EXPECT_TRUE(llvm::Regex("open\\([^,]*arg0[^,]*, [^,]*arg1").match(Read))
      << Read;
  EXPECT_TRUE(llvm::Regex("open\\([^,]*arg0[^,]*, [^,]*arg1[^,]*, 0x1A4\\)")
                  .match(Write))
      << Write;
  EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
}

TEST(SysVCallContract, AVariadicImportReadsTheParametersPassedOnToIt) {
  // grep opens a file `xor ecx, ecx; xor eax, eax; call openat`, passing
  // its dirfd, path and flags on untouched and reading none of them, and
  // logs `mov esi, 7; xor eax, eax; call printf` with its format.  A
  // variadic import reads its fixed parameters, which are then the
  // functions' own, and the arguments a call sets after them follow them:
  // its unit called openat() in a function of no parameter.
  constexpr va_t Opener = Text, Logger = Text + 0x20, OpenAt = Text + 0x40,
                 Printf = Text + 0x50;
  std::vector<uint8_t> Code(0x60, 0xCC);
  auto Define = [&](va_t At, std::vector<uint8_t> Setup, va_t Callee) {
    std::vector<uint8_t> Bytes = {0x48, 0x83, 0xEC, 0x08}; // sub rsp, 8
    Bytes.insert(Bytes.end(), Setup.begin(), Setup.end());
    Bytes.push_back(0xE8); // call
    for (uint8_t B : rel32(At + Bytes.size() + 4, Callee))
      Bytes.push_back(B);
    for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                      0xC3})                  // ret
      Bytes.push_back(B);
    put(Code, At, Bytes);
  };
  Define(Opener,
         {0x31, 0xC9,  // xor ecx, ecx
          0x31, 0xC0}, // xor eax, eax
         OpenAt);
  Define(Logger,
         {0xBE, 0x07, 0x00, 0x00, 0x00, // mov esi, 7
          0x31, 0xC0},                  // xor eax, eax
         Printf);
  const BinaryImage Img =
      makeImportImage(Code, {{Opener, "opener"}, {Logger, "logger"}},
                      {{OpenAt, "openat"}, {Printf, "printf"}});
  const std::string Source = liftEntries(Img, {Opener, Logger});
  const std::string Open = body(Source, "opener");
  const std::string Log = body(Source, "logger");
  ASSERT_FALSE(Open.empty()) << Source;
  ASSERT_FALSE(Log.empty()) << Source;
  EXPECT_TRUE(
      llvm::Regex("opener\\([^)]*arg0, [^)]*arg1, [^)]*arg2\\)").match(Open))
      << Open;
  EXPECT_TRUE(llvm::Regex("openat\\([^,]*arg0[^,]*, [^,]*arg1[^,]*, "
                          "[^,]*arg2[^,]*, 0\\)")
                  .match(Open))
      << Open;
  EXPECT_TRUE(llvm::Regex("printf\\([^,]*arg0[^,]*, 7\\)").match(Log)) << Log;
  EXPECT_EQ(Source.find("unknown value"), std::string::npos) << Source;
}

TEST(SysVCallContract, AJumpThroughALoadedSlotPassesTheArgumentsSetBefore) {
  // GCC's register_tm_clones: RDI and RSI are set and tested, RAX is
  // loaded from the GOT slot of _ITM_registerTMCloneTable and tested, and
  // `jmp rax` alone in its block passes the arguments set two blocks up.
  constexpr va_t Wrap = Text, Slot = 0x403000;
  std::vector<uint8_t> Code(0x60, 0xCC);
  std::vector<uint8_t> WrapCode = {0x48, 0x8D, 0x3D}; // lea rdi, [rip+data]
  for (uint8_t B : rel32(Wrap + 7, Text + 0x50))
    WrapCode.push_back(B);
  for (uint8_t B : {0xBE, 0x08, 0x00, 0x00, 0x00, // mov esi, 8
                    0x85, 0xF6,                   // test esi, esi
                    0x74, 0x10,                   // je done
                    0x48, 0x8B, 0x05})            // mov rax, [rip+slot]
    WrapCode.push_back(B);
  for (uint8_t B : rel32(Wrap + 23, Slot))
    WrapCode.push_back(B);
  for (uint8_t B : {0x48, 0x85, 0xC0, // test rax, rax
                    0x74, 0x02,       // je done
                    0xFF, 0xE0,       // jmp rax
                    0xC3})            // done: ret
    WrapCode.push_back(B);
  put(Code, Wrap, WrapCode);
  BinaryImage Img = makeImportImage(Code, {{Wrap, "wrap"}}, {});
  Segment Got;
  Got.Name = ".got";
  Got.VA = Slot;
  Got.Size = Got.FileSz = 8;
  Got.Data.resize(8);
  Got.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Got));
  ASSERT_TRUE(Img.recordImportStorageSlot(Slot, "_ITM_registerTMCloneTable", 0,
                                          ImportStorageEvidence::LoaderBind));
  const std::string Source = liftEntries(Img, {Wrap});
  const std::string Body = body(Source, "wrap");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("_ITM_registerTMCloneTable("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
  // The jump through the import's GOT slot reaches a void routine, so the
  // wrapper returns nothing either, like GCC's register_tm_clones.
  EXPECT_NE(Source.find("void wrap(void)"), std::string::npos) << Source;
}

TEST(SysVCallContract, ACallThroughALoaderBoundSlotPassesThePrototype) {
  // Built with -fno-plt, wrap(s, f) calls fputs through the GOT slot that a
  // GLOB_DAT relocation binds, which no import directory entry lists.
  constexpr va_t Wrap = Text, Slot = 0x403000;
  std::vector<uint8_t> Code(0x20, 0xCC);
  std::vector<uint8_t> WrapCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                   0xFF, 0x15};            // call [rip+slot]
  for (uint8_t B : rel32(Wrap + 10, Slot))
    WrapCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0xC3})                  // ret
    WrapCode.push_back(B);
  put(Code, Wrap, WrapCode);
  BinaryImage Img = makeImportImage(Code, {{Wrap, "wrap"}}, {});
  Segment Got;
  Got.Name = ".got";
  Got.VA = Slot;
  Got.Size = Got.FileSz = 8;
  Got.Data.resize(8);
  Got.Flags = SegmentFlags::Readable | SegmentFlags::Writable;
  Img.Segments.push_back(std::move(Got));
  ASSERT_TRUE(Img.recordImportStorageSlot(Slot, "fputs", 0,
                                          ImportStorageEvidence::LoaderBind));
  const std::string Source = liftEntries(Img, {Wrap});
  const std::string Body = body(Source, "wrap");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(
      Body.find("fputs((void *)(uintptr_t)arg0, (void *)(uintptr_t)arg1)"),
      std::string::npos)
      << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(SysVCallContract, APLTEntryOfItsOwnRoutinePassesTheRoutinesArguments) {
  // A shared object defines g(x) { return 2 * x + 1; } and reaches it through
  // its PLT entry, `f: jmp g@plt`, as it does a preemptible g.  The loader
  // relocates g's slot to g itself, so f and the entry pass x on, to g.
  constexpr va_t G = Text, F = Text + 0x10, Stub = Text + 0x20;
  std::vector<uint8_t> Code(0x30, 0xCC);
  put(Code, G, {0x8D, 0x44, 0x3F, 0x01, 0xC3}); // lea eax, [rdi+rdi+1]; ret
  std::vector<uint8_t> Jump = {0xE9};           // jmp stub
  for (uint8_t B : rel32(F + 5, Stub))
    Jump.push_back(B);
  put(Code, F, Jump);
  BinaryImage Img = makeImportImage(Code, {{G, "g"}, {F, "f"}}, {{Stub, "g"}});
  constexpr va_t Slot = 0x403000;
  for (unsigned B = 0; B < 8; ++B)
    Img.Segments.back().Data[B] = static_cast<uint8_t>(G >> (8 * B));
  Img.CodePtrRelocSlots.insert(Slot);
  for (const std::set<va_t> &Entries :
       {std::set<va_t>{G, F, Stub}, std::set<va_t>{F}}) {
    SCOPED_TRACE(Entries.size());
    const std::string Source = liftEntries(Img, Entries);
    const std::string Caller = body(Source, "f");
    ASSERT_FALSE(Caller.empty()) << Source;
    EXPECT_NE(Caller.find("f(int64_t arg0)"), std::string::npos) << Caller;
    EXPECT_TRUE(llvm::Regex("[^[:alnum:]_]g\\(arg0\\)").match(Caller))
        << Caller;
    EXPECT_FALSE(llvm::Regex("[^[:alnum:]_]g_[[:xdigit:]]+\\(").match(Source))
        << Source;
  }
  const std::string Source = liftEntries(Img, {G, Stub});
  const std::string Entry = body(Source, "sub_401020");
  ASSERT_FALSE(Entry.empty()) << Source;
  EXPECT_NE(Entry.find("sub_401020(int64_t arg0)"), std::string::npos) << Entry;
  EXPECT_TRUE(llvm::Regex("[^[:alnum:]_]g\\(arg0\\)").match(Entry)) << Entry;
}

TEST(SysVCallContract, AnArgumentOnlyTheCallSiteScanReadsKeepsItsValue) {
  // parse(o): s = o->text + 1 in RDI, tested by a load through it; the call
  // to an import with no prototype in the next block passes it on.  Only
  // the call site's register scan reads that RDI, after the uses were
  // counted, so its value must still print.
  constexpr va_t Parse = Text, Stub = Text + 0x30;
  std::vector<uint8_t> Code(0x40, 0xCC);
  std::vector<uint8_t> ParseCode = {0x48, 0x8B, 0x47, 0x10, // mov rax, [rdi+16]
                                    0x48, 0x8D, 0x78, 0x01, // lea rdi, [rax+1]
                                    0x80, 0x3F, 0x2D, // cmp byte [rdi], '-'
                                    0x74, 0x08,       // je minus
                                    0x31, 0xF6,       // xor esi, esi
                                    0xE8};            // call parse_num
  for (uint8_t B : rel32(Parse + 20, Stub))
    ParseCode.push_back(B);
  for (uint8_t B : {0xC3,                         // ret
                    0xB8, 0x01, 0x00, 0x00, 0x00, // minus: mov eax, 1
                    0xC3})                        // ret
    ParseCode.push_back(B);
  put(Code, Parse, ParseCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Parse, "parse"}}, {{Stub, "parse_num"}});
  const std::string Source = liftEntries(Img, {Parse});
  const std::string Body = body(Source, "parse");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("parse_num("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown register"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(SysVCallContract, ARegisterTheCalleeLeavesAloneIsNoReturnedField) {
  // add(a, b): rdx = b; eax = inc(a); return eax + edx.  inc never writes
  // RDX, so GCC keeps b there across the call; EDX after it is b, not a
  // second eightbyte that inc returns.
  constexpr va_t Add = Text, Inc = Text + 0x10;
  std::vector<uint8_t> Code(0x20, 0xCC);
  std::vector<uint8_t> AddCode = {0x48, 0x89, 0xF2, // mov rdx, rsi
                                  0xE8};            // call inc
  for (uint8_t B : rel32(Add + 8, Inc))
    AddCode.push_back(B);
  for (uint8_t B : {0x01, 0xD0, // add eax, edx
                    0xC3})      // ret
    AddCode.push_back(B);
  put(Code, Add, AddCode);
  put(Code, Inc,
      {0x8D, 0x47, 0x01, // lea eax, [rdi+1]
       0xC3});           // ret
  const BinaryImage Img =
      makeImportImage(Code, {{Add, "add"}, {Inc, "inc"}}, {});
  const std::string Source = liftEntries(Img, {Add, Inc});
  const std::string Body = body(Source, "add");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_EQ(Body.find(">> 0x20"), std::string::npos) << Body;
  compileAndRun(Source + R"(
int main(void) {
  return (uint32_t)add(5, 7) == 13 && (uint32_t)add(-3, 40) == 38 ? 0 : 1;
}
)");
}

TEST(SysVCallContract, ACallThatEndsItsFunctionDoesNotReturn) {
  // pfatal(name) calls error(2, 0, NULL, name): error returns only for a
  // zero status, so GCC places nothing after the call, and pfatal's sized
  // symbol ends with it.  What follows the 2-byte no-op is next's code.
  constexpr va_t PFatal = Text, Next = Text + 0x13, ErrorStub = Text + 0x30;
  std::vector<uint8_t> Code(0x40, 0xCC);
  std::vector<uint8_t> PFatalCode = {0x48, 0x89, 0xF9, // mov rcx, rdi
                                     0xBF, 0x02, 0x00, 0x00, 0x00, // mov edi, 2
                                     0x31, 0xF6, // xor esi, esi
                                     0x31, 0xD2, // xor edx, edx
                                     0xE8};      // call error
  for (uint8_t B : rel32(PFatal + 17, ErrorStub))
    PFatalCode.push_back(B);
  PFatalCode.push_back(0x66); // xchg ax, ax
  PFatalCode.push_back(0x90);
  put(Code, PFatal, PFatalCode);
  put(Code, Next,
      {0x48, 0x8D, 0x47, 0x01, // lea rax, [rdi+1]
       0xC3});                 // ret
  BinaryImage Img = makeImportImage(Code, {{PFatal, "pfatal"}, {Next, "next"}},
                                    {{ErrorStub, "error"}});
  Img.Symbols.front().Size = 17;
  ASSERT_EQ(PFatal + static_cast<va_t>(PFatalCode.size()), Next);
  const std::string Source = liftEntries(Img, {PFatal});
  const std::string Body = body(Source, "pfatal");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(Body.find("error("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("next("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

TEST(SysVCallContract, AVariadicPrologueSpillReadsNoParameter) {
  // A variadic panic(fmt, ...) spills RSI..R9 to its register save area
  // around `test al, al`, and the vector registers in the block that test
  // branches around: GCC without a frame pointer, which first pushes and pops
  // RAX, GCC with a frame pointer off RBP (sed, patch),
  // GCC and Clang without one off RSP, and Clang -O0 where the branch over
  // the vector spills rejoins, beside a home slot for RDI.  wrapper(s) calls
  // panic("%s", s) and caller(x) calls wrapper(x).  Decompiling caller alone
  // summarizes the other two as its callees, the way the GUI does: the spills
  // of registers wrapper never sets are no parameters of wrapper, so caller
  // passes it exactly one argument.
  const std::vector<std::vector<uint8_t>> Prologues = {
      {0xF3, 0x0F, 0x1E, 0xFA,                    // endbr64
       0x50,                                      // push rax
       0x58,                                      // pop rax
       0x48, 0x89, 0xFB,                          // mov rbx, rdi
       0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00,  // sub rsp, 0xd8
       0x48, 0x89, 0x74, 0x24, 0x28,              // mov [rsp+0x28], rsi
       0x48, 0x89, 0x54, 0x24, 0x30,              // mov [rsp+0x30], rdx
       0x48, 0x89, 0x4C, 0x24, 0x38,              // mov [rsp+0x38], rcx
       0x4C, 0x89, 0x44, 0x24, 0x40,              // mov [rsp+0x40], r8
       0x4C, 0x89, 0x4C, 0x24, 0x48,              // mov [rsp+0x48], r9
       0x84, 0xC0,                                // test al, al
       0x74, 0x0A,                                // je spilled
       0x0F, 0x29, 0x44, 0x24, 0x50,              // movaps [rsp+0x50], xmm0
       0x0F, 0x29, 0x4C, 0x24, 0x60},             // movaps [rsp+0x60], xmm1
      {0xF3, 0x0F, 0x1E, 0xFA,                    // endbr64
       0x55,                                      // push rbp
       0x48, 0x89, 0xE5,                          // mov rbp, rsp
       0x53,                                      // push rbx
       0x48, 0x89, 0xFB,                          // mov rbx, rdi
       0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00,  // sub rsp, 0xd8
       0x48, 0x89, 0xB5, 0x48, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xb8], rsi
       0x48, 0x89, 0x95, 0x50, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xb0], rdx
       0x48, 0x89, 0x8D, 0x58, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xa8], rcx
       0x4C, 0x89, 0x85, 0x60, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xa0], r8
       0x4C, 0x89, 0x8D, 0x68, 0xFF, 0xFF, 0xFF,  // mov [rbp-0x98], r9
       0x84, 0xC0,                                // test al, al
       0x74, 0x07,                                // je spilled
       0x0F, 0x29, 0x85, 0x70, 0xFF, 0xFF, 0xFF}, // movaps [rbp-0x90], xmm0
      {0xF3, 0x0F, 0x1E, 0xFA,                    // endbr64
       0x53,                                      // push rbx
       0x48, 0x89, 0xFB,                          // mov rbx, rdi
       0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00,  // sub rsp, 0xd0
       0x48, 0x89, 0x74, 0x24, 0x28,              // mov [rsp+0x28], rsi
       0x48, 0x89, 0x54, 0x24, 0x30,              // mov [rsp+0x30], rdx
       0x48, 0x89, 0x4C, 0x24, 0x38,              // mov [rsp+0x38], rcx
       0x4C, 0x89, 0x44, 0x24, 0x40,              // mov [rsp+0x40], r8
       0x4C, 0x89, 0x4C, 0x24, 0x48,              // mov [rsp+0x48], r9
       0x84, 0xC0,                                // test al, al
       0x74, 0x05,                                // je spilled
       0x0F, 0x29, 0x44, 0x24, 0x50},             // movaps [rsp+0x50], xmm0
      {0x55,                                      // push rbp
       0x48, 0x89, 0xE5,                          // mov rbp, rsp
       0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00,  // sub rsp, 0xd0
       0x84, 0xC0,                                // test al, al
       0x74, 0x07,                                // je spilled
       0x0F, 0x29, 0x85, 0x60, 0xFF, 0xFF, 0xFF,  // movaps [rbp-0xa0], xmm0
       0x4C, 0x89, 0x8D, 0x58, 0xFF, 0xFF, 0xFF,  // spilled: mov [rbp-0xa8], r9
       0x4C, 0x89, 0x85, 0x50, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xb0], r8
       0x48, 0x89, 0x8D, 0x48, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xb8], rcx
       0x48, 0x89, 0x95, 0x40, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xc0], rdx
       0x48, 0x89, 0xB5, 0x38, 0xFF, 0xFF, 0xFF,  // mov [rbp-0xc8], rsi
       0x48, 0x89, 0x7D, 0xF8,                    // mov [rbp-0x8], rdi
       0x48, 0x8B, 0x5D, 0xF8}};                  // mov rbx, [rbp-0x8]
  constexpr va_t Panic = Text, Wrapper = Text + 0x60, Caller = Text + 0x80,
                 ExitStub = Text + 0xA0;
  for (const std::vector<uint8_t> &Prologue : Prologues) {
    SCOPED_TRACE(Prologue.size());
    std::vector<uint8_t> Code(0xB0, 0xCC);
    std::vector<uint8_t> PanicCode = Prologue;
    for (uint8_t B : {0x48, 0x89, 0xDF, // mov rdi, rbx
                      0xE8})            // call exit
      PanicCode.push_back(B);
    for (uint8_t B : rel32(Panic + PanicCode.size() + 4, ExitStub))
      PanicCode.push_back(B);
    ASSERT_LE(PanicCode.size(), Wrapper - Panic);
    put(Code, Panic, PanicCode);
    std::vector<uint8_t> WrapperCode = {0x48, 0x89, 0xFE, // mov rsi, rdi
                                        0x31, 0xC0,       // xor eax, eax
                                        0xBF, 0x00, 0x20,
                                        0x40, 0x00, // mov edi, fmt
                                        0xE8};      // call panic
    for (uint8_t B : rel32(Wrapper + 15, Panic))
      WrapperCode.push_back(B);
    put(Code, Wrapper, WrapperCode);
    std::vector<uint8_t> CallerCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                       0xE8};                  // call wrapper
    for (uint8_t B : rel32(Caller + 9, Wrapper))
      CallerCode.push_back(B);
    for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                      0xC3})                  // ret
      CallerCode.push_back(B);
    put(Code, Caller, CallerCode);
    const BinaryImage Img = makeImportImage(
        Code, {{Panic, "panic"}, {Wrapper, "wrapper"}, {Caller, "caller"}},
        {{ExitStub, "exit"}});
    llvm::LLVMContext Ctx;
    PipelineOptions Opts;
    Opts.EmitDumpOutput = false;
    Opts.OnlyFunctionEntries = {Caller};
    const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
    ASSERT_TRUE(Result.Success) << Result.Error;
    for (const va_t Entry : {Panic, Wrapper}) {
      const auto Reads = Result.CallEntryReadGPRs.find(Entry);
      ASSERT_NE(Reads, Result.CallEntryReadGPRs.end()) << std::hex << Entry;
      for (const uint64_t Spilled :
           {x86reg::RSI, x86reg::RDX, x86reg::RCX, x86reg::R8, x86reg::R9})
        EXPECT_EQ(Reads->second[Spilled / 8], 0)
            << std::hex << Entry << " reads " << Spilled;
      EXPECT_EQ(Reads->second[x86reg::RDI / 8], 8) << std::hex << Entry;
      for (unsigned Vector = 0; Vector < 2; ++Vector)
        EXPECT_EQ(Reads->second[kX64VectorFamilyBase + Vector], 0)
            << std::hex << Entry << " reads xmm" << Vector;
    }
    std::string Source;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions Options;
    Options.TheArch = Img.Arch;
    Options.Format = Img.Format;
    Options.Image = &Img;
    ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
    const std::string Body = body(Source, "caller");
    ASSERT_FALSE(Body.empty()) << Source;
    EXPECT_NE(Body.find("wrapper(arg0)"), std::string::npos) << Body;
    EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  }
}

TEST(SysVCallContract, ACopyOfEaxAfterSeteIsNoVectorCount) {
  // is_slash(c) is `c == '/'`: GCC sets AL with sete and copies the whole of
  // EAX, so the summary reads RAX's upper bytes as well.  Only a read of
  // exactly AL is a variadic prologue's vector count, so the call still
  // passes is_slash's one parameter, not the RSI the caller left set.
  constexpr va_t IsSlash = Text, Caller = Text + 0x20;
  std::vector<uint8_t> Code(0x40, 0xCC);
  put(Code, IsSlash,
      {0x83, 0xFF, 0x2F, // cmp edi, '/'
       0x0F, 0x94, 0xC0, // sete al
       0x89, 0xC2,       // mov edx, eax
       0x89, 0xD0,       // mov eax, edx
       0xC3});           // ret
  std::vector<uint8_t> CallerCode = {
      0x48, 0x83, 0xEC, 0x08,       // sub rsp, 8
      0xBE, 0x03, 0x00, 0x00, 0x00, // mov esi, 3
      0xBF, 0x5C, 0x00, 0x00, 0x00, // mov edi, '\\'
      0xE8};                        // call is_slash
  for (uint8_t B : rel32(Caller + CallerCode.size() + 4, IsSlash))
    CallerCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0xC3})                  // ret
    CallerCode.push_back(B);
  put(Code, Caller, CallerCode);
  const BinaryImage Img =
      makeImportImage(Code, {{IsSlash, "is_slash"}, {Caller, "caller"}}, {});
  const std::string Source = liftEntries(Img, {Caller});
  const std::string Body = body(Source, "caller");
  ASSERT_FALSE(Body.empty()) << Source;
  const size_t Call = Body.find("is_slash(");
  ASSERT_NE(Call, std::string::npos) << Body;
  const std::string Line = Body.substr(Call, Body.find('\n', Call) - Call);
  EXPECT_EQ(Line.find(','), std::string::npos) << Line;
}

TEST(SysVCallContract, ADoubleAVariadicCallPassesOnIsAParameter) {
  // note(fmt, ...) tests AL and saves the vector registers for va_arg, and
  // each forwarder loads its format into RDI and AL with the doubles it
  // passes: two jump to note and one calls it, passing on its own XMM0
  // untouched (`long gd(double x){return logv("y", x);}`), and one sets
  // XMM1 before AL counts two.  AL counts what the call passes, so each
  // reads XMM0 at entry, a double, as its parameter, and caller passes it
  // the double it computes.  A forwarder that leaves AL alone passes no
  // count of its own, and its XMM0 stays no argument.
  constexpr va_t Note = Text, Jumps = Text + 0x40, Calls = Text + 0x60,
                 Sets = Text + 0x80, Unset = Text + 0xA0, Caller = Text + 0xC0;
  std::vector<uint8_t> Code(0xE0, 0xCC);
  const std::vector<uint8_t> NoteCode = {
      0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00, // sub rsp, 0xd8
      0x48, 0x89, 0x74, 0x24, 0x28,             // mov [rsp+0x28], rsi
      0x48, 0x89, 0x54, 0x24, 0x30,             // mov [rsp+0x30], rdx
      0x48, 0x89, 0x4C, 0x24, 0x38,             // mov [rsp+0x38], rcx
      0x4C, 0x89, 0x44, 0x24, 0x40,             // mov [rsp+0x40], r8
      0x4C, 0x89, 0x4C, 0x24, 0x48,             // mov [rsp+0x48], r9
      0x84, 0xC0,                               // test al, al
      0x74, 0x0A,                               // je spilled
      0x0F, 0x29, 0x44, 0x24, 0x50,             // movaps [rsp+0x50], xmm0
      0x0F, 0x29, 0x4C, 0x24, 0x60,             // movaps [rsp+0x60], xmm1
      0x31, 0xC0,                               // spilled: xor eax, eax
      0x48, 0x81, 0xC4, 0xD8, 0x00, 0x00, 0x00, // add rsp, 0xd8
      0xC3};                                    // ret
  put(Code, Note, NoteCode);
  // mov edi, fmt; the count and other setup; jmp or call note.
  auto Forwarder = [&](va_t At, std::vector<uint8_t> Setup, bool Call) {
    std::vector<uint8_t> Bytes;
    if (Call)
      Bytes = {0x48, 0x83, 0xEC, 0x08}; // sub rsp, 8
    for (uint8_t B : {0xBF, 0x00, 0x20, 0x40, 0x00})
      Bytes.push_back(B);
    llvm::append_range(Bytes, Setup);
    Bytes.push_back(Call ? 0xE8 : 0xE9);
    for (uint8_t B : rel32(At + Bytes.size() + 4, Note))
      Bytes.push_back(B);
    if (Call)
      for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                        0xC3})                  // ret
        Bytes.push_back(B);
    put(Code, At, Bytes);
  };
  const std::vector<uint8_t> CountsOne = {0xB0, 0x01}; // mov al, 1
  Forwarder(Jumps, CountsOne, false);
  Forwarder(Calls, CountsOne, true);
  Forwarder(Sets,
            {0x0F, 0x57, 0xC9, // xorps xmm1, xmm1
             0xB0, 0x02},      // mov al, 2
            false);
  Forwarder(Unset, {}, false);
  std::vector<uint8_t> CallerCode = {
      0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
      0xF2, 0x0F, 0x2A, 0xC7, // cvtsi2sd xmm0, edi
      0xE8};                  // call jumps
  for (uint8_t B : rel32(Caller + CallerCode.size() + 4, Jumps))
    CallerCode.push_back(B);
  for (uint8_t B : {0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0xC3})                  // ret
    CallerCode.push_back(B);
  put(Code, Caller, CallerCode);
  const BinaryImage Img = makeImportImage(Code,
                                          {{Note, "note"},
                                           {Jumps, "jumps"},
                                           {Calls, "calls"},
                                           {Sets, "sets"},
                                           {Unset, "unset"},
                                           {Caller, "caller"}},
                                          {});
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Jumps, Calls, Sets, Unset, Caller};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  const std::pair<va_t, bool> Forwarders[] = {
      {Jumps, true}, {Calls, true}, {Sets, true}, {Unset, false}};
  for (const auto &[Entry, Passed] : Forwarders) {
    SCOPED_TRACE(Entry);
    const auto Reads = Result.CallEntryReadGPRs.find(Entry);
    ASSERT_NE(Reads, Result.CallEntryReadGPRs.end());
    EXPECT_EQ(Reads->second[kX64VectorFamilyBase], Passed ? 8 : 0);
    EXPECT_EQ(Reads->second[kX64VectorFamilyBase + 1], 0);
    const auto Func = llvm::find_if(
        Result.MedFuncs, [&](const MedFunc &F) { return F.Entry == Entry; });
    ASSERT_NE(Func, Result.MedFuncs.end());
    auto Takes = [&](uint64_t Register) {
      return llvm::any_of(
          Func->Params, [&](const MedVar &P) { return P.RegOff == Register; });
    };
    EXPECT_EQ(Takes(x86reg::XMM0), Passed);
    EXPECT_FALSE(Takes(x86reg::XMM1));
  }
  std::string Source;
  llvm::raw_string_ostream OS(Source);
  CEmitterOptions Options;
  Options.TheArch = Img.Arch;
  Options.Format = Img.Format;
  Options.Image = &Img;
  ASSERT_TRUE(HighCEmitter().emit(Result.HighFuncs, OS, Options));
  EXPECT_NE(Source.find("jumps(double arg0)"), std::string::npos) << Source;
  const std::string Body = body(Source, "caller");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_EQ(Body.find("jumps()"), std::string::npos) << Body;
  EXPECT_NE(Body.find("jumps("), std::string::npos) << Body;
}

TEST(SysVCallContract, GccSplitVaStartMarksTheFunctionVariadic) {
  // fatal(fmt, ...) as GCC builds it: va_start stores gp_offset and fp_offset
  // as two 32-bit constants where Clang stores one 64-bit word.  Either marks
  // the variadic prologue, whose register save area holds what va_arg reads:
  // the integer registers, and in the vector ones the doubles a floating
  // vararg is promoted to, never a whole vector.
  constexpr va_t Fatal = Text, VprintfStub = Text + 0x100,
                 ExitStub = Text + 0x110;
  std::vector<uint8_t> Code(0x120, 0xCC);
  std::vector<uint8_t> FatalCode = {
      0x55,                                     // push rbp
      0x48, 0x89, 0xE5,                         // mov rbp, rsp
      0x48, 0x81, 0xEC, 0xD0, 0x00, 0x00, 0x00, // sub rsp, 0xd0
      0x48, 0x89, 0xB5, 0x58, 0xFF, 0xFF, 0xFF, // mov [rbp-0xa8], rsi
      0x48, 0x89, 0x95, 0x60, 0xFF, 0xFF, 0xFF, // mov [rbp-0xa0], rdx
      0x48, 0x89, 0x8D, 0x68, 0xFF, 0xFF, 0xFF, // mov [rbp-0x98], rcx
      0x4C, 0x89, 0x85, 0x70, 0xFF, 0xFF, 0xFF, // mov [rbp-0x90], r8
      0x4C, 0x89, 0x8D, 0x78, 0xFF, 0xFF, 0xFF, // mov [rbp-0x88], r9
      0x84, 0xC0,                               // test al, al
      0x74, 0x04,                               // je spilled
      0x0F, 0x29, 0x45, 0x80,                   // movaps [rbp-0x80], xmm0
      0x48, 0x8D, 0x45, 0x10,                   // spilled: lea rax, [rbp+0x10]
      0x48, 0x89, 0x85, 0x38, 0xFF, 0xFF, 0xFF, // mov [rbp-0xc8], rax
      0x48, 0x8D, 0x85, 0x50, 0xFF, 0xFF, 0xFF, // lea rax, [rbp-0xb0]
      0x48, 0x89, 0x85, 0x40, 0xFF, 0xFF, 0xFF, // mov [rbp-0xc0], rax
      0xC7, 0x85, 0x30, 0xFF, 0xFF, 0xFF,       // mov dword [rbp-0xd0],
      0x08, 0x00, 0x00, 0x00,                   //   8 (gp_offset)
      0xC7, 0x85, 0x34, 0xFF, 0xFF, 0xFF,       // mov dword [rbp-0xcc],
      0x30, 0x00, 0x00, 0x00,                   //   0x30 (fp_offset)
      0x48, 0x8D, 0xB5, 0x30, 0xFF, 0xFF, 0xFF, // lea rsi, [rbp-0xd0]
      0xE8};                                    // call vprintf
  for (uint8_t B : rel32(Fatal + FatalCode.size() + 4, VprintfStub))
    FatalCode.push_back(B);
  for (uint8_t B : {0xBF, 0x02, 0x00, 0x00, 0x00, // mov edi, 2
                    0xE8})                        // call exit
    FatalCode.push_back(B);
  for (uint8_t B : rel32(Fatal + FatalCode.size() + 4, ExitStub))
    FatalCode.push_back(B);
  ASSERT_LE(FatalCode.size(), VprintfStub - Fatal);
  put(Code, Fatal, FatalCode);
  const BinaryImage Img = makeImportImage(
      Code, {{Fatal, "fatal"}}, {{VprintfStub, "vprintf"}, {ExitStub, "exit"}});
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Fatal};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  const auto F = std::find_if(
      Result.MedFuncs.begin(), Result.MedFuncs.end(),
      [&](const MedFunc &Function) { return Function.Entry == Fatal; });
  ASSERT_NE(F, Result.MedFuncs.end());
  EXPECT_TRUE(F->IsVariadic);
  for (const MedVar &Param : F->Params) {
    if (Param.RegOff < x86reg::VectorBase) {
      EXPECT_LE(Param.Size, 8u);
      continue;
    }
    const auto Scalar = F->FPParamScalarBytes.find(Param.RegOff);
    ASSERT_NE(Scalar, F->FPParamScalarBytes.end())
        << "vector save-area slot as a vector parameter";
    EXPECT_EQ(Scalar->second, sizeof(double));
  }
}

TEST(SysVCallContract, AColdFragmentCallingAbortThroughItsStubNeverReturns) {
  // check(x) jumps to its cold fragment, which calls abort through the PLT
  // stub that function discovery also lists as a function.  The stub's
  // import never returns, so neither does the fragment, and check's jump to
  // it hands back no value.
  constexpr va_t Check = Text, Cold = Text + 0x40, AbortStub = Text + 0x100;
  std::vector<uint8_t> Code(0x110, 0xCC);
  std::vector<uint8_t> CheckCode = {0x85, 0xFF,  // test edi, edi
                                    0x0F, 0x88}; // js cold
  for (uint8_t B : rel32(Check + CheckCode.size() + 4, Cold))
    CheckCode.push_back(B);
  for (uint8_t B : {0x8D, 0x04, 0x7F, // lea eax, [rdi + rdi*2]
                    0xC3})            // ret
    CheckCode.push_back(B);
  put(Code, Check, CheckCode);
  std::vector<uint8_t> ColdCode = {0xE8}; // call abort
  for (uint8_t B : rel32(Cold + 5, AbortStub))
    ColdCode.push_back(B);
  put(Code, Cold, ColdCode);
  const BinaryImage Img = makeImportImage(
      Code, {{Check, "check"}, {Cold, "check_cold"}}, {{AbortStub, "abort"}});
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Check, Cold, AbortStub};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  auto Find = [&](va_t Entry) {
    return std::find_if(
        Result.MedFuncs.begin(), Result.MedFuncs.end(),
        [&](const MedFunc &Function) { return Function.Entry == Entry; });
  };
  const auto Fragment = Find(Cold);
  ASSERT_NE(Fragment, Result.MedFuncs.end());
  EXPECT_TRUE(Fragment->DoesNotReturn);
  const auto Caller = Find(Check);
  ASSERT_NE(Caller, Result.MedFuncs.end());
  bool SawJump = false;
  for (const MedBlock &Block : Caller->Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL && Op.NumInputs && Op.Inputs[0].isConst() &&
          Op.Inputs[0].ConstVal == Cold) {
        SawJump = true;
        EXPECT_TRUE(Op.DoesNotReturn);
      }
  EXPECT_TRUE(SawJump);
}

TEST(SysVCallContract, AByteSetAfterACallReadsNoSecondResult) {
  // g calls f, which clobbers EDX, then compares two flags it sets in DL and
  // AL.  `setne dl` keeps what f left in the rest of RDX, but nothing reads
  // those bytes, so f returns one register and no record of two.
  constexpr va_t G = Text, F = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> GCode = {0x53,             // push rbx
                                0x48, 0x89, 0xFB, // mov rbx, rdi
                                0xE8};            // call f
  for (uint8_t B : rel32(G + GCode.size() + 4, F))
    GCode.push_back(B);
  for (uint8_t B : {0x80, 0x3B, 0x00, // cmp byte [rbx], 0
                    0x0F, 0x95, 0xC2, // setne dl
                    0x83, 0xF8, 0x07, // cmp eax, 7
                    0x0F, 0x94, 0xC0, // sete al
                    0x38, 0xD0,       // cmp al, dl
                    0x0F, 0x94, 0xC0, // sete al
                    0x0F, 0xB6, 0xC0, // movzx eax, al
                    0x5B,             // pop rbx
                    0xC3})            // ret
    GCode.push_back(B);
  put(Code, G, GCode);
  put(Code, F,
      {0x89, 0xFA,       // mov edx, edi
       0x8D, 0x04, 0x52, // lea eax, [rdx + rdx*2]
       0xC3});           // ret
  const BinaryImage Img = makeImportImage(Code, {{G, "g"}, {F, "f"}}, {});
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {G, F};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  const auto Callee = std::find_if(
      Result.MedFuncs.begin(), Result.MedFuncs.end(),
      [&](const MedFunc &Function) { return Function.Entry == F; });
  ASSERT_NE(Callee, Result.MedFuncs.end());
  EXPECT_TRUE(Callee->MultiReturn.empty());
}

TEST(SysVCallContract, AStringCompareLeavesNoSecondResultInRDX) {
  // same(a, b) returns `a && strcmp(a, b) != 0` the way GCC returns a bool:
  // EDX is zero, or `setne dl` over what strcmp left in RDX, and EDX moves
  // whole into EAX.  strcmp declares an int, so RDX holds no second field of
  // its result and its stub returns one register; same returns the one byte
  // both paths define, which no byte strcmp left in RDX reaches.
  constexpr va_t Same = Text, StrcmpStub = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> SameCode = {0x31, 0xD2,       // xor edx, edx
                                   0x48, 0x85, 0xFF, // test rdi, rdi
                                   0x74, 0x0A,       // je done
                                   0xE8};            // call strcmp
  for (uint8_t B : rel32(Same + 12, StrcmpStub))
    SameCode.push_back(B);
  for (uint8_t B : {0x85, 0xC0,       // test eax, eax
                    0x0F, 0x95, 0xC2, // setne dl
                    0x89, 0xD0,       // done: mov eax, edx
                    0xC3})            // ret
    SameCode.push_back(B);
  put(Code, Same, SameCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Same, "same"}}, {{StrcmpStub, "strcmp"}});
  const std::string Source = liftEntries(Img, {Same, StrcmpStub});
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
  EXPECT_NE(Source.find("int8_t same("), std::string::npos) << Source;
}

TEST(SysVCallContract, AWholeRegisterArgumentKeepsItsHighBytes) {
  // ntfscp's parse_options passes a string to an import no declaration
  // describes with `lea rdi, [rip + s]` while EDI is live: the lifter syncs
  // EDI with the address's low bytes, and the call took that sync for its
  // argument, printing `(int32_t)("parse_options")`.  The call reads all of
  // RDI.
  constexpr va_t Name = Text, Str = Text + 0x20, LogStub = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> NameCode = {0x53,              // push rbx
                                   0x48, 0x8D, 0x3D}; // lea rdi, [rip + str]
  for (uint8_t B : rel32(Name + 8, Str))
    NameCode.push_back(B);
  for (uint8_t B : {0x89, 0xFB, // mov ebx, edi
                    0xE8})      // call log_name
    NameCode.push_back(B);
  for (uint8_t B : rel32(Name + 15, LogStub))
    NameCode.push_back(B);
  for (uint8_t B : {0x89, 0xD8, // mov eax, ebx
                    0x5B,       // pop rbx
                    0xC3})      // ret
    NameCode.push_back(B);
  put(Code, Name, NameCode);
  put(Code, Str, {'h', 'i', 0});
  const BinaryImage Img =
      makeImportImage(Code, {{Name, "name"}}, {{LogStub, "log_name"}});
  const std::string Source = liftEntries(Img, {Name});
  const std::string Body = body(Source, "name");
  const size_t Call = Body.find("log_name(");
  ASSERT_NE(Call, std::string::npos) << Source;
  const std::string Line = Body.substr(Call, Body.find('\n', Call) - Call);
  EXPECT_EQ(Line.find("int32_t"), std::string::npos) << Body;
}

TEST(SysVCallContract, APaddingPushAfterAPointerCallReadsNoSecondResult) {
  // ntfscp's utils_set_locale passes setlocale's result as the one stack
  // argument of a variadic call, which GCC aligns with a `push rdx` between
  // the call's register setup: a read of what setlocale left in RDX.
  // setlocale declares a pointer, so RDX holds no second field of its
  // result, and the call returns that one register.
  constexpr va_t Setup = Text, SetlocaleStub = Text + 0x40,
                 LogStub = Text + 0x48;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> SetupCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                    0x31, 0xF6,             // xor esi, esi
                                    0xBF, 0x06, 0x00, 0x00, 0x00, // mov edi, 6
                                    0xE8}; // call setlocale
  for (uint8_t B : rel32(Setup + 16, SetlocaleStub))
    SetupCode.push_back(B);
  for (uint8_t B : {0xB9, 0x80, 0x00, 0x00, 0x00,       // mov ecx, 0x80
                    0xBE, 0x34, 0x12, 0x00, 0x00,       // mov esi, 0x1234
                    0x41, 0xB9, 0x78, 0x56, 0x00, 0x00, // mov r9d, 0x5678
                    0x52,                               // push rdx
                    0x45, 0x31, 0xC0,                   // xor r8d, r8d
                    0xBA, 0x8C, 0x00, 0x00, 0x00,       // mov edx, 0x8c
                    0xBF, 0xBC, 0x9A, 0x00, 0x00,       // mov edi, 0x9abc
                    0x50,                               // push rax
                    0x31, 0xC0,                         // xor eax, eax
                    0xE8})                              // call log_it
    SetupCode.push_back(B);
  for (uint8_t B : rel32(Setup + 54, LogStub))
    SetupCode.push_back(B);
  for (uint8_t B : {0x59,                   // pop rcx
                    0x5E,                   // pop rsi
                    0x48, 0x83, 0xC4, 0x08, // add rsp, 8
                    0xC3})                  // ret
    SetupCode.push_back(B);
  put(Code, Setup, SetupCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Setup, "setup"}},
                      {{SetlocaleStub, "setlocale"}, {LogStub, "log_it"}});
  llvm::LLVMContext Ctx;
  PipelineOptions Opts;
  Opts.EmitDumpOutput = false;
  Opts.OnlyFunctionEntries = {Setup};
  const PipelineResult Result = Pipeline().run(Img, Ctx, Opts);
  ASSERT_TRUE(Result.Success) << Result.Error;
  const auto Caller = std::find_if(
      Result.MedFuncs.begin(), Result.MedFuncs.end(),
      [&](const MedFunc &Function) { return Function.Entry == Setup; });
  ASSERT_NE(Caller, Result.MedFuncs.end());
  const MedOp *Call = nullptr;
  for (const MedBlock &Block : Caller->Blocks)
    for (const MedOp &Op : Block.Ops)
      if (Op.Opcode == NdOp::CALL && Op.NumInputs >= 1 &&
          Op.Inputs[0].isConst() && Op.Inputs[0].ConstVal == SetlocaleStub)
        Call = &Op;
  ASSERT_NE(Call, nullptr);
  EXPECT_EQ(Call->Output.Kind, MedVar::Reg);
  EXPECT_EQ(Call->Output.Size, 8);
}

TEST(SysVCallContract, ATailCallLeavesNoRegisterOfTheCallersOwn) {
  // g adds RDX to what f returns in RAX, so f returns the pair.  f sets
  // EDX to 11 and tail-jumps to an import no declaration describes: RDX
  // then holds what the import leaves there, not 11.  Nothing forwards the
  // pair such an import returns yet, so f refuses to return one.
  constexpr va_t F = Text, G = Text + 0x20, Stub = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> FCode = {0xBA, 0x0B, 0x00, 0x00, 0x00, // mov edx, 11
                                0xE9};                        // jmp stub
  for (uint8_t B : rel32(F + 10, Stub))
    FCode.push_back(B);
  put(Code, F, FCode);
  std::vector<uint8_t> GCode = {0xE8}; // call f
  for (uint8_t B : rel32(G + 5, F))
    GCode.push_back(B);
  for (uint8_t B : {0x48, 0x01, 0xD0, // add rax, rdx
                    0xC3})            // ret
    GCode.push_back(B);
  put(Code, G, GCode);
  const BinaryImage Img = makeImportImage(Code, {{F, "f"}, {G, "g"}},
                                          {{Stub, "neverd_pair_source"}});
  const std::string Source = liftEntries(Img, {F, G, Stub});
  EXPECT_EQ(Source.find("(11)) << 64"), std::string::npos) << Source;
  EXPECT_NE(Source.find("int64_t f(void) {\n    __builtin_trap(); /* unknown "
                        "return value */"),
            std::string::npos)
      << Source;
}

TEST(SysVCallContract, AByteSetAfterAFloatCallLeavesNoUnknownBytes) {
  // later(a, b) returns `a && difftime(a, b) >= 0` the way GCC returns a
  // bool: EAX is zero, or `setbe al` over what difftime, which returns a
  // double, left in RAX.  later returns the one byte both paths define.
  constexpr va_t Later = Text, DifftimeStub = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> LaterCode = {0x31, 0xC0,       // xor eax, eax
                                    0x48, 0x85, 0xFF, // test rdi, rdi
                                    0x74, 0x10,       // je done
                                    0xE8};            // call difftime
  for (uint8_t B : rel32(Later + 12, DifftimeStub))
    LaterCode.push_back(B);
  for (uint8_t B : {0x66, 0x0F, 0xEF, 0xC9, // pxor xmm1, xmm1
                    0x66, 0x0F, 0x2F, 0xC8, // comisd xmm1, xmm0
                    0x0F, 0x96, 0xC0,       // setbe al
                    0xC3})                  // done: ret
    LaterCode.push_back(B);
  put(Code, Later, LaterCode);
  const BinaryImage Img =
      makeImportImage(Code, {{Later, "later"}}, {{DifftimeStub, "difftime"}});
  const std::string Source = liftEntries(Img, {Later, DifftimeStub});
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
  EXPECT_NE(Source.find("int8_t later("), std::string::npos) << Source;
}

TEST(SysVCallContract, ABoolCallersTestIsNoFloatALeftoverReturns) {
  // f(s) parses s with strtod and returns whether the value is positive,
  // like find's get_relative_timestamp: EAX is 1 or 0 at the shared
  // epilogue, where XMM0 still holds the value or its square.  g tests AL
  // after the call, so f returns that integer and no double.
  constexpr va_t F = Text, G = Text + 0x30, StrtodStub = Text + 0x40;
  std::vector<uint8_t> Code(0x50, 0xCC);
  std::vector<uint8_t> FCode = {0x53,  // push rbx
                                0xE8}; // call strtod
  for (uint8_t B : rel32(F + 6, StrtodStub))
    FCode.push_back(B);
  for (uint8_t B : {0x66, 0x0F, 0xEF, 0xC9,       // pxor xmm1, xmm1
                    0x66, 0x0F, 0x2F, 0xC1,       // comisd xmm0, xmm1
                    0x76, 0x0B,                   // jbe false
                    0xF2, 0x0F, 0x59, 0xC0,       // mulsd xmm0, xmm0
                    0xB8, 0x01, 0x00, 0x00, 0x00, // mov eax, 1
                    0xEB, 0x02,                   // jmp done
                    0x31, 0xC0,                   // false: xor eax, eax
                    0x5B,                         // done: pop rbx
                    0xC3})                        // ret
    FCode.push_back(B);
  put(Code, F, FCode);
  std::vector<uint8_t> GCode = {0xE8}; // call f
  for (uint8_t B : rel32(G + 5, F))
    GCode.push_back(B);
  for (uint8_t B : {0x84, 0xC0,       // test al, al
                    0x0F, 0x94, 0xC0, // sete al
                    0x0F, 0xB6, 0xC0, // movzx eax, al
                    0xC3})            // ret
    GCode.push_back(B);
  put(Code, G, GCode);
  const BinaryImage Img =
      makeImportImage(Code, {{F, "f"}, {G, "g"}}, {{StrtodStub, "strtod"}});
  const std::string Source = liftEntries(Img, {F, G, StrtodStub});
  EXPECT_EQ(Source.find("unknown"), std::string::npos) << Source;
  EXPECT_EQ(Source.find("double f("), std::string::npos) << Source;
}

TEST(SysVCallContract, AnAlignmentPushIsNoStackArgument) {
  // die() calls the variadic panic("out of memory") and never returns, so
  // it aligns the stack for the call with a push of RAX (Clang) or a push,
  // pop and `sub rsp, 8` (GCC) instead of a frame.  RAX's incoming value is
  // undefined: the slot it lands in at the call passes nothing.
  constexpr va_t Panic = Text, Die = Text + 0x60, ExitStub = Text + 0xA0;
  const std::vector<std::vector<uint8_t>> Alignments = {
      {0x50},                    // push rax
      {0x50, 0x58,               // push rax; pop rax
       0x48, 0x83, 0xEC, 0x08}}; // sub rsp, 8
  for (const std::vector<uint8_t> &Alignment : Alignments) {
    SCOPED_TRACE(Alignment.size());
    std::vector<uint8_t> Code(0xB0, 0xCC);
    std::vector<uint8_t> PanicCode = {
        0x48, 0x81, 0xEC, 0xD8, 0x00, 0x00, 0x00, // sub rsp, 0xd8
        0x48, 0x89, 0x74, 0x24, 0x28,             // mov [rsp+0x28], rsi
        0x48, 0x89, 0x54, 0x24, 0x30,             // mov [rsp+0x30], rdx
        0x48, 0x89, 0x4C, 0x24, 0x38,             // mov [rsp+0x38], rcx
        0x4C, 0x89, 0x44, 0x24, 0x40,             // mov [rsp+0x40], r8
        0x4C, 0x89, 0x4C, 0x24, 0x48,             // mov [rsp+0x48], r9
        0x84, 0xC0,                               // test al, al
        0x74, 0x05,                               // je spilled
        0x0F, 0x29, 0x44, 0x24, 0x50,             // movaps [rsp+0x50], xmm0
        0xE8};                                    // spilled: call exit
    for (uint8_t B : rel32(Panic + PanicCode.size() + 4, ExitStub))
      PanicCode.push_back(B);
    put(Code, Panic, PanicCode);
    std::vector<uint8_t> DieCode = {0xBF, 0x00, 0x20,
                                    0x40, 0x00,  // mov edi, fmt
                                    0x31, 0xC0}; // xor eax, eax
    DieCode.insert(DieCode.begin(), Alignment.begin(), Alignment.end());
    DieCode.push_back(0xE8); // call panic
    for (uint8_t B : rel32(Die + DieCode.size() + 4, Panic))
      DieCode.push_back(B);
    put(Code, Die, DieCode);
    const BinaryImage Img = makeImportImage(
        Code, {{Panic, "panic"}, {Die, "die"}}, {{ExitStub, "exit"}});
    const std::string Source = liftEntries(Img, {Die});
    const std::string Body = body(Source, "die");
    ASSERT_FALSE(Body.empty()) << Source;
    const size_t Call = Body.find("panic(");
    ASSERT_NE(Call, std::string::npos) << Body;
    const std::string Line = Body.substr(Call, Body.find('\n', Call) - Call);
    EXPECT_EQ(Line.find(','), std::string::npos) << Line;
    EXPECT_EQ(Body.find("unknown"), std::string::npos) << Body;
  }
}

TEST(SysVCallContract, ErrorCheckingWrapperEndsAtItsExitHelper) {
  // xxd's shape: put(s, f) returns fputs(s, f) unless it fails, then calls
  // die(3), which ends in exit.  GCC aligns the next function with no-ops,
  // and the bytes after them are another function's, not put's.
  constexpr va_t Die = Text, Put = Text + 0x20, Next = Text + 0x40,
                 FputsStub = Text + 0x60, ExitStub = Text + 0x70;
  std::vector<uint8_t> Code(0x80, 0xCC);
  std::vector<uint8_t> DieCode = {0x48, 0x83, 0xEC, 0x08, // sub rsp, 8
                                  0xE8};                  // call exit
  for (uint8_t B : rel32(Die + 9, ExitStub))
    DieCode.push_back(B);
  put(Code, Die, DieCode);
  std::vector<uint8_t> PutCode = {0x55,             // push rbp
                                  0x48, 0x89, 0xE5, // mov rbp, rsp
                                  0xE8};            // call fputs
  for (uint8_t B : rel32(Put + 9, FputsStub))
    PutCode.push_back(B);
  for (uint8_t B : {0x83, 0xF8, 0xFF,             // cmp eax, -1
                    0x74, 0x02,                   // je fail
                    0x5D,                         // pop rbp
                    0xC3,                         // ret
                    0xBF, 0x03, 0x00, 0x00, 0x00, // fail: mov edi, 3
                    0xE8})                        // call die
    PutCode.push_back(B);
  for (uint8_t B : rel32(Put + 26, Die))
    PutCode.push_back(B);
  // nop word ptr [rax+rax*1+0x0] up to Next.
  for (uint8_t B : {0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00})
    PutCode.push_back(B);
  put(Code, Put, PutCode);
  // next(a, b, c): a function that reads three argument registers.
  put(Code, Next,
      {0x48, 0x8D, 0x04, 0x37, // lea rax, [rdi+rsi]
       0x48, 0x01, 0xD0,       // add rax, rdx
       0xC3});                 // ret
  const BinaryImage Img =
      makeImportImage(Code, {{Die, "die"}, {Put, "put"}, {Next, "next"}},
                      {{FputsStub, "fputs"}, {ExitStub, "exit"}});
  ASSERT_EQ(Put + static_cast<va_t>(PutCode.size()), Next);
  const std::string Source = liftEntries(Img, {Put});
  const std::string Body = body(Source, "put");
  ASSERT_FALSE(Body.empty()) << Source;
  EXPECT_NE(
      Body.find("fputs((void *)(uintptr_t)arg0, (void *)(uintptr_t)arg1)"),
      std::string::npos)
      << Body;
  EXPECT_NE(Body.find("die(3)"), std::string::npos) << Body;
  EXPECT_EQ(Body.find("next("), std::string::npos) << Body;
  EXPECT_EQ(Body.find("unknown value"), std::string::npos) << Body;
}

} // namespace
