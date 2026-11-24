//===- GoStackResultsTests.cpp - Results Go returns on the stack ----------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Go's stack ABI returns a function's results in the words of its caller's
/// frame after its arguments.  These tests run the C of small stack-ABI
/// routines and check each caller reads what its callee returned.
///
//===----------------------------------------------------------------------===//

#include "../language/GoRuntimeEHTestsDetail.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::go_eh_test;

constexpr va_t kCode = kTextVA + 0x100;

/// A routine of the fixture: its name, where it starts in the code, and the
/// bytes of arguments and results its record gives.
struct Routine {
  const char *Name;
  uint32_t Offset;
  uint32_t FrameBytes;
};

/// x86-64 routines, each as Go's compiler lays its frame out: arguments from
/// `[rsp+8]` at the entry, results in the words after them, and a callee's
/// from `[rsp]` at the call.
///   main.add(a, b) int:    mov rax, [rsp+8]; add rax, [rsp+16]
///                          mov [rsp+24], rax; ret
///   main.caller(x) int:    sub rsp, 0x20; mov rax, [rsp+0x28]; mov [rsp], rax
///                          mov qword [rsp+8], 5; call main.add
///                          mov rax, [rsp+0x10]; inc rax; mov [rsp+0x30], rax
///                          add rsp, 0x20; ret
///   main.sumok(a, b) (int, bool):
///                          mov rax, [rsp+8]; add rax, [rsp+16]
///                          mov [rsp+24], rax; mov byte [rsp+32], 1; ret
///   main.use(a) int:       sub rsp, 0x30; mov rax, [rsp+0x38]; mov [rsp], rax
///                          mov qword [rsp+8], 3; call main.sumok
///                          mov rax, [rsp+0x10]; movzx ecx, byte [rsp+0x18]
///                          add rax, rcx; mov [rsp+0x40], rax
///                          add rsp, 0x30; ret
///   main.fill(p *int):     mov rax, [rsp+8]; mov qword [rax], 41; ret
///   main.named() (r int):  sub rsp, 0x10; mov qword [rsp+0x18], 0
///                          lea rax, [rsp+0x18]; mov [rsp], rax
///                          call main.fill; nop; add rsp, 0x10; ret
///   main.wrap(a, b) int:   inc qword [rsp+8]; jmp main.add
///   main.viaWrap(x) int:   main.caller's body, calling main.wrap
///   main.ignore(x):        main.caller's body up to its call, then
///                          add rsp, 0x20; ret
///   main.pair() (int, int):
///                          mov qword [rsp+8], 3; mov qword [rsp+16], 4; ret
///   main.sum() int:        sub rsp, 0x20; call main.pair; call main.add
///                          mov rax, [rsp+0x10]; mov [rsp+0x28], rax
///                          add rsp, 0x20; ret
///   main.one(a) int:       mov rax, [rsp+8]; mov [rsp+16], rax; ret
///   main.ind(fn, a):       sub rsp, 0x20; mov rax, [rsp+0x30]; mov [rsp], rax
///                          call main.one; mov rax, [rsp+0x30]; mov [rsp], rax
///                          mov rdx, [rsp+0x28]; mov rax, [rdx]; call rax
///                          add rsp, 0x20; ret
///   main.bump() (r int):   mov qword [rsp+8], 41; inc qword [rsp+8]; ret
const std::vector<uint8_t> &x64Code() {
  static const std::vector<uint8_t> Code = {
      0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x03, 0x44, 0x24, 0x10, 0x48, 0x89,
      0x44, 0x24, 0x18, 0xC3, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x44, 0x24,
      0x28, 0x48, 0x89, 0x04, 0x24, 0x48, 0xC7, 0x44, 0x24, 0x08, 0x05, 0x00,
      0x00, 0x00, 0xE8, 0xD5, 0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0x44, 0x24, 0x10,
      0x48, 0xFF, 0xC0, 0x48, 0x89, 0x44, 0x24, 0x30, 0x48, 0x83, 0xC4, 0x20,
      0xC3, 0x0F, 0x1F, 0x00, 0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x03, 0x44,
      0x24, 0x10, 0x48, 0x89, 0x44, 0x24, 0x18, 0xC6, 0x44, 0x24, 0x20, 0x01,
      0xC3, 0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x44, 0x24, 0x38, 0x48, 0x89, 0x04,
      0x24, 0x48, 0xC7, 0x44, 0x24, 0x08, 0x03, 0x00, 0x00, 0x00, 0xE8, 0xC5,
      0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0x44, 0x24, 0x10, 0x0F, 0xB6, 0x4C, 0x24,
      0x18, 0x48, 0x01, 0xC8, 0x48, 0x89, 0x44, 0x24, 0x40, 0x48, 0x83, 0xC4,
      0x30, 0xC3, 0x0F, 0x1F, 0x00, 0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0xC7, 0x00,
      0x29, 0x00, 0x00, 0x00, 0xC3, 0x0F, 0x1F, 0x00, 0x48, 0x83, 0xEC, 0x10,
      0x48, 0xC7, 0x44, 0x24, 0x18, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x44,
      0x24, 0x18, 0x48, 0x89, 0x04, 0x24, 0xE8, 0xD5, 0xFF, 0xFF, 0xFF, 0x0F,
      0x1F, 0x44, 0x00, 0x00, 0x48, 0x83, 0xC4, 0x10, 0xC3, 0x66, 0x66, 0x2E,
      0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0xFF, 0x44, 0x24,
      0x08, 0xE9, 0x16, 0xFF, 0xFF, 0xFF, 0x66, 0x0F, 0x1F, 0x44, 0x00, 0x00,
      0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x44, 0x24, 0x28, 0x48, 0x89, 0x04,
      0x24, 0x48, 0xC7, 0x44, 0x24, 0x08, 0x05, 0x00, 0x00, 0x00, 0xE8, 0xD5,
      0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0x44, 0x24, 0x10, 0x48, 0xFF, 0xC0, 0x48,
      0x89, 0x44, 0x24, 0x30, 0x48, 0x83, 0xC4, 0x20, 0xC3, 0x0F, 0x1F, 0x00,
      0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x44, 0x24, 0x28, 0x48, 0x89, 0x04,
      0x24, 0x48, 0xC7, 0x44, 0x24, 0x08, 0x05, 0x00, 0x00, 0x00, 0xE8, 0xC5,
      0xFE, 0xFF, 0xFF, 0x48, 0x83, 0xC4, 0x20, 0xC3, 0x48, 0xC7, 0x44, 0x24,
      0x08, 0x03, 0x00, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x10, 0x04, 0x00,
      0x00, 0x00, 0xC3, 0x66, 0x90, 0x66, 0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xEC, 0x20, 0xE8, 0xD7, 0xFF, 0xFF,
      0xFF, 0xE8, 0x92, 0xFE, 0xFF, 0xFF, 0x48, 0x8B, 0x44, 0x24, 0x10, 0x48,
      0x89, 0x44, 0x24, 0x28, 0x48, 0x83, 0xC4, 0x20, 0xC3, 0x0F, 0x1F, 0x00,
      0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0x89, 0x44, 0x24, 0x10, 0xC3, 0x0F,
      0x1F, 0x44, 0x00, 0x00, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x44, 0x24,
      0x30, 0x48, 0x89, 0x04, 0x24, 0xE8, 0xDE, 0xFF, 0xFF, 0xFF, 0x48, 0x8B,
      0x44, 0x24, 0x30, 0x48, 0x89, 0x04, 0x24, 0x48, 0x8B, 0x54, 0x24, 0x28,
      0x48, 0x8B, 0x02, 0xFF, 0xD0, 0x48, 0x83, 0xC4, 0x20, 0xC3, 0x66, 0x0F,
      0x1F, 0x44, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x08, 0x29, 0x00, 0x00,
      0x00, 0x48, 0xFF, 0x44, 0x24, 0x08, 0xC3,
  };
  return Code;
}

const Routine X64Routines[] = {
    {"main.add", 0x000, 24},   {"main.caller", 0x010, 16},
    {"main.sumok", 0x040, 25}, {"main.use", 0x060, 16},
    {"main.fill", 0x0A0, 8},   {"main.named", 0x0B0, 8},
    {"main.wrap", 0x0E0, 24},  {"main.viaWrap", 0x0F0, 16},
    {"main.ignore", 0x120, 8}, {"main.pair", 0x140, 16},
    {"main.sum", 0x160, 8},    {"main.one", 0x180, 16},
    {"main.ind", 0x190, 16},   {"main.bump", 0x1C0, 8},
};

/// arm64 routines, where a callee's words begin one above the stack pointer
/// at the call and a frame keeps the link register in its lowest word.
///   main.add(a, b) int:    ldr x0, [sp, #8]; ldr x1, [sp, #16]
///                          add x0, x0, x1; str x0, [sp, #24]; ret
///   main.caller(x) int:    str x30, [sp, #-0x30]!; ldr x0, [sp, #0x38]
///                          str x0, [sp, #8]; mov x0, #5; str x0, [sp, #16]
///                          bl main.add; ldr x0, [sp, #24]; add x0, x0, #1
///                          str x0, [sp, #0x40]; ldr x30, [sp], #0x30; ret
///   main.sumok(a, b) (int, bool):
///                          main.add's body; mov w1, #1; strb w1, [sp, #32]
///                          ret
///   main.use(a) int:       str x30, [sp, #-0x40]!; ldr x0, [sp, #0x48]
///                          str x0, [sp, #8]; mov x0, #3; str x0, [sp, #16]
///                          bl main.sumok; ldr x0, [sp, #24]
///                          ldrb w1, [sp, #32]; add x0, x0, x1
///                          str x0, [sp, #0x50]; ldr x30, [sp], #0x40; ret
///   main.pair() (int, int):
///                          mov x0, #3; str x0, [sp, #8]; mov x0, #4
///                          str x0, [sp, #16]; ret
///   main.sum() int:        str x30, [sp, #-0x30]!; bl main.pair; bl main.add
///                          ldr x0, [sp, #24]; str x0, [sp, #0x38]
///                          ldr x30, [sp], #0x30; ret
const std::vector<uint8_t> &arm64Code() {
  static const std::vector<uint8_t> Code = [] {
    const uint32_t Words[] = {
        0xF94007E0, 0xF9400BE1, 0x8B010000, 0xF9000FE0, 0xD65F03C0, 0xD503201F,
        0xD503201F, 0xD503201F, 0xF81D0FFE, 0xF9401FE0, 0xF90007E0, 0xD28000A0,
        0xF9000BE0, 0x97FFFFF3, 0xF9400FE0, 0x91000400, 0xF90023E0, 0xF84307FE,
        0xD65F03C0, 0xD503201F, 0xF94007E0, 0xF9400BE1, 0x8B010000, 0xF9000FE0,
        0x52800021, 0x390083E1, 0xD65F03C0, 0xD503201F, 0xF81C0FFE, 0xF94027E0,
        0xF90007E0, 0xD2800060, 0xF9000BE0, 0x97FFFFF3, 0xF9400FE0, 0x394083E1,
        0x8B010000, 0xF9002BE0, 0xF84407FE, 0xD65F03C0, 0xD2800060, 0xF90007E0,
        0xD2800080, 0xF9000BE0, 0xD65F03C0, 0xD503201F, 0xD503201F, 0xD503201F,
        0xF81D0FFE, 0x97FFFFF7, 0x97FFFFCE, 0xF9400FE0, 0xF9001FE0, 0xF84307FE,
        0xD65F03C0,
    };
    std::vector<uint8_t> Bytes(sizeof(Words));
    std::memcpy(Bytes.data(), Words, sizeof(Words));
    return Bytes;
  }();
  return Code;
}

const Routine ARM64Routines[] = {
    {"main.add", 0x00, 24}, {"main.caller", 0x20, 16}, {"main.sumok", 0x50, 25},
    {"main.use", 0x70, 16}, {"main.pair", 0xA0, 16},   {"main.sum", 0xC0, 8},
};

/// What the pipeline makes of \p Routines, stack-ABI Go code in \p Code: its
/// result and the C of every routine, HighC or, with \p LLVM, LLVM C.  With
/// \p Only, a run of those routines alone.
struct Decompiled {
  GoTestImage Image;
  /// Outlives the module in Result.
  llvm::LLVMContext Context;
  PipelineResult Result;
  std::string Source;

  Decompiled(Arch A, const std::vector<uint8_t> &Code,
             llvm::ArrayRef<Routine> Routines, bool LLVM = false,
             std::set<va_t> Only = {})
      : Image(A) {
    Image.writeText(kCode, Code);
    GoModuleInfo Module;
    Module.StackArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + kTextSize}};
    for (const Routine &R : Routines) {
      Symbol Function = Symbol::makeFunc(kCode + R.Offset);
      Function.Name = R.Name;
      Image.Img.Symbols.push_back(std::move(Function));
      Module.FrameArguments.push_back(
          {kCode + R.Offset, R.FrameBytes, std::nullopt});
    }
    Image.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = LLVM;
    Options.OnlyFunctionEntries = std::move(Only);
    Result = Pipeline().run(Image.Img, Context, Options);
    if (!Result.Success)
      return;
    llvm::raw_string_ostream OS(Source);
    CEmitterOptions C;
    C.TheArch = Image.Img.Arch;
    C.Format = Image.Img.Format;
    C.Image = &Image.Img;
    if (!(LLVM ? LLVMCEmitter().emit(*Result.LlvmModule, OS, C, nullptr,
                                     &Image.Img)
               : HighCEmitter().emit(Result.HighFuncs, OS, C)))
      Source.clear();
  }

  const MedFunc *function(const char *Name) const {
    for (const MedFunc &F : Result.MedFuncs)
      if (F.Name == Name)
        return &F;
    return nullptr;
  }

  /// The C of the routine \p Name, from its signature on.
  std::string body(const std::string &Name) const {
    size_t At = 0;
    while ((At = Source.find(" " + Name + "(", At)) != std::string::npos &&
           Source.compare(Source.find('\n', At) - 2, 2, " {") != 0)
      ++At;
    if (At == std::string::npos)
      return {};
    return Source.substr(At, Source.find("\n}", At) - At);
  }
};

llvm::ArrayRef<Routine> routines(Arch A) {
  return A == Arch::X64 ? llvm::ArrayRef<Routine>(X64Routines)
                        : llvm::ArrayRef<Routine>(ARM64Routines);
}

const std::vector<uint8_t> &code(Arch A) {
  return A == Arch::X64 ? x64Code() : arm64Code();
}

TEST(GoStackResults, ACallReturnsTheResultItsCalleeStores) {
  // The callee's store of its result was a dead store to a copy of its own,
  // and its caller read a word of its frame nobody wrote: no placeholder, a
  // stale value.
  for (const Arch A : {Arch::X64, Arch::AArch64})
    for (const bool LLVM : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(A) * 2 + LLVM);
      const Decompiled D(A, code(A), routines(A), LLVM);
      ASSERT_TRUE(D.Result.Success) << D.Result.Error;
      ASSERT_FALSE(D.Source.empty());
      const MedFunc *Add = D.function("main.add");
      ASSERT_NE(Add, nullptr);
      // One result, the word after the two arguments, and still two
      // parameters.
      ASSERT_EQ(Add->GoStackResults.size(), 1u);
      EXPECT_EQ(Add->GoStackResults[0], (MedGoStackResult{16, 8}));
      EXPECT_EQ(Add->Params.size(), 2u);
      EXPECT_EQ(D.Source.find("__builtin_trap"), std::string::npos) << D.Source;
      source_call_execution_test::compileAndRun(D.Source + R"C(
int main(void) {
  if (main_add(3, 4) != 7)
    return 1;
  for (int64_t x = -6; x < 40; x += 5)
    if (main_caller(x) != x + 6)
      return 2;
  return 0;
}
)C");
    }
}

TEST(GoStackResults, SeveralResultsReturnAsTheirRecord) {
  // An int and a bool: the bool is one byte of its word, which the caller
  // reads as a byte.
  for (const Arch A : {Arch::X64, Arch::AArch64})
    for (const bool LLVM : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(A) * 2 + LLVM);
      const Decompiled D(A, code(A), routines(A), LLVM);
      ASSERT_TRUE(D.Result.Success) << D.Result.Error;
      const MedFunc *SumOk = D.function("main.sumok");
      ASSERT_NE(SumOk, nullptr);
      ASSERT_EQ(SumOk->GoStackResults.size(), 2u);
      EXPECT_EQ(SumOk->GoStackResults[0], (MedGoStackResult{16, 8}));
      EXPECT_EQ(SumOk->GoStackResults[1], (MedGoStackResult{24, 1}));
      EXPECT_EQ(SumOk->MultiReturn.size(), 2u);
      source_call_execution_test::compileAndRun(D.Source + R"C(
int main(void) {
  for (int64_t a = -3; a < 30; a += 3)
    if (main_use(a) != a + 4)
      return 1;
  return 0;
}
)C");
    }
}

TEST(GoStackResults, AResultWrittenThroughItsAddressIsReturned) {
  // A named result a callee writes through its address: the function's
  // return reads the word as the callee left it.
  for (const bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const Decompiled D(Arch::X64, x64Code(), X64Routines, LLVM);
    ASSERT_TRUE(D.Result.Success) << D.Result.Error;
    source_call_execution_test::compileAndRun(D.Source + R"C(
int main(void) { return main_named() != 41; }
)C");
  }
}

TEST(GoStackResults, AResultReadBackIsNoParameter) {
  // `main.bump` stores its result and then adds one to it where it lies.
  // The read made the word a stack parameter, which each caller passed an
  // unknown value for.
  const Decompiled D(Arch::X64, x64Code(), X64Routines);
  ASSERT_TRUE(D.Result.Success) << D.Result.Error;
  const MedFunc *Bump = D.function("main.bump");
  ASSERT_NE(Bump, nullptr);
  EXPECT_TRUE(Bump->Params.empty());
  EXPECT_TRUE(Bump->MutableStackParamHomes.empty());
  source_call_execution_test::compileAndRun(D.Source + R"C(
int main(void) { return main_bump() != 42; }
)C");
}

TEST(GoStackResults, ARoutineThatJumpsOnReturnsWhatItsTargetDoes) {
  // `main.wrap` adds one to its first argument where it lies and jumps to
  // `main.add`, which returns its result in the frame the two share.  The
  // jump passes on the words of the target's record, the result's too, which
  // is no parameter: each caller passed an unknown value for it.
  for (const bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const Decompiled D(Arch::X64, x64Code(), X64Routines, LLVM);
    ASSERT_TRUE(D.Result.Success) << D.Result.Error;
    const MedFunc *Wrap = D.function("main.wrap");
    ASSERT_NE(Wrap, nullptr);
    ASSERT_EQ(Wrap->GoStackResults.size(), 1u);
    EXPECT_EQ(Wrap->GoStackResults[0], (MedGoStackResult{16, 8}));
    EXPECT_EQ(Wrap->Params.size(), 2u);
    EXPECT_EQ(D.Source.find("__builtin_trap"), std::string::npos) << D.Source;
    source_call_execution_test::compileAndRun(D.Source + R"C(
int main(void) {
  if (main_wrap(3, 4) != 8)
    return 1;
  for (int64_t x = -6; x < 40; x += 5)
    if (main_viaWrap(x) != x + 7)
      return 2;
  return 0;
}
)C");
  }
}

TEST(GoStackResults, AResultNoCallerReadsLeavesNoStore) {
  const Decompiled D(Arch::X64, x64Code(), X64Routines);
  ASSERT_TRUE(D.Result.Success) << D.Result.Error;
  const MedFunc *Ignore = D.function("main.ignore");
  ASSERT_NE(Ignore, nullptr);
  EXPECT_TRUE(Ignore->GoStackResults.empty());
  const std::string Body = D.body("main_ignore");
  EXPECT_NE(D.Source.find("void main_ignore(int64_t arg0) {"),
            std::string::npos)
      << D.Source;
  EXPECT_NE(Body.find("main_add(arg0, 5);"), std::string::npos) << Body;
  EXPECT_EQ(Body.find(" = "), std::string::npos) << Body;
}

TEST(GoStackResults, AResultPassedOnInPlaceIsTheNextCallsArgument) {
  // `main.add(main.pair())`: the two results lie where `main.add` takes its
  // arguments, so the compiler stores nothing between the calls.  `main.pair`
  // writes no register, so its call defined none either.
  // Two words of results are one record on x86-64 and arm64's register pair.
  for (const Arch A : {Arch::X64, Arch::AArch64})
    for (const bool LLVM : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(A) * 2 + LLVM);
      const Decompiled D(A, code(A), routines(A), LLVM);
      ASSERT_TRUE(D.Result.Success) << D.Result.Error;
      const MedFunc *Pair = D.function("main.pair");
      ASSERT_NE(Pair, nullptr);
      ASSERT_EQ(Pair->GoStackResults.size(), 2u);
      EXPECT_EQ(D.Source.find("__builtin_trap"), std::string::npos) << D.Source;
      source_call_execution_test::compileAndRun(D.Source + R"C(
int main(void) { return main_sum() != 7; }
)C");
    }
}

TEST(GoStackResults, ACallThroughAFunctionValueTakesNoEarlierResult) {
  // `main.one(a)` leaves its result in the word after its argument, where a
  // call with two arguments would take its second.  The call through the
  // function value stored one word, and no callee says it reads more.
  const Decompiled D(Arch::X64, x64Code(), X64Routines);
  ASSERT_TRUE(D.Result.Success) << D.Result.Error;
  const std::string Body = D.body("main_ind");
  EXPECT_NE(Body.find("main_one(arg1);"), std::string::npos) << Body;
  EXPECT_NE(Body.find(")(arg1);"), std::string::npos) << Body;
}

TEST(GoStackResults, ARegisterACalleeKeepsIsNoArgument) {
  // `bsf rdi, rsi` keeps RDI where RSI is zero, and Go's count of trailing
  // zeros replaces it there: `main.ctz` reads no register as it was entered.
  // Its summary lists the read all the same, and a caller bound RDI as an
  // operand of the call: the caller's own RDI became a parameter, and a
  // function that takes the platform's registers returns no stack result.
  // `main.probe`, whose record has no word to say it takes none, shows the
  // call binds nothing.
  //   main.ctz(x) int:    mov rsi, [rsp+8]; bsf rdi, rsi; mov r8d, 0x40
  //                       cmovz rdi, r8; mov [rsp+16], rdi; ret
  //   main.viaCtz(x) int: sub rsp, 0x18; mov rax, [rsp+0x20]; mov [rsp], rax
  //                       call main.ctz; mov rax, [rsp+8]; inc rax
  //                       mov [rsp+0x28], rax; add rsp, 0x18; ret
  //   main.probe():       sub rsp, 0x18; mov qword ptr [rsp], 8
  //                       call main.ctz; add rsp, 0x18; ret
  const std::vector<uint8_t> Code = {
      0x48, 0x8B, 0x74, 0x24, 0x08, 0x48, 0x0F, 0xBC, 0xFE, 0x41, 0xB8, 0x40,
      0x00, 0x00, 0x00, 0x49, 0x0F, 0x44, 0xF8, 0x48, 0x89, 0x7C, 0x24, 0x10,
      0xC3, 0x0F, 0x1F, 0x80, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xEC, 0x18,
      0x48, 0x8B, 0x44, 0x24, 0x20, 0x48, 0x89, 0x04, 0x24, 0xE8, 0xCE, 0xFF,
      0xFF, 0xFF, 0x48, 0x8B, 0x44, 0x24, 0x08, 0x48, 0xFF, 0xC0, 0x48, 0x89,
      0x44, 0x24, 0x28, 0x48, 0x83, 0xC4, 0x18, 0xC3, 0x90, 0x66, 0x66, 0x2E,
      0x0F, 0x1F, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x48, 0x83, 0xEC, 0x18,
      0x48, 0xC7, 0x04, 0x24, 0x08, 0x00, 0x00, 0x00, 0xE8, 0x9F, 0xFF, 0xFF,
      0xFF, 0x48, 0x83, 0xC4, 0x18, 0xC3};
  const Routine Routines[] = {{"main.ctz", 0x00, 16},
                              {"main.viaCtz", 0x20, 16},
                              {"main.probe", 0x50, 0}};
  for (const bool LLVM : {false, true}) {
    SCOPED_TRACE(LLVM);
    const Decompiled D(Arch::X64, Code, Routines, LLVM);
    ASSERT_TRUE(D.Result.Success) << D.Result.Error;
    const MedFunc *Via = D.function("main.viaCtz");
    ASSERT_NE(Via, nullptr);
    EXPECT_TRUE(Via->StackArgumentsOnly);
    EXPECT_EQ(Via->Params.size(), 1u);
    EXPECT_EQ(Via->GoStackResults.size(), 1u);
    const MedFunc *Probe = D.function("main.probe");
    ASSERT_NE(Probe, nullptr);
    EXPECT_TRUE(Probe->StackArgumentsOnly);
    EXPECT_TRUE(Probe->Params.empty());
    source_call_execution_test::compileAndRun(D.Source + R"C(
int main(void) {
  main_probe();
  return main_viaCtz(8) != 4 || main_viaCtz(1) != 1 || main_viaCtz(0) != 65 ||
         main_viaCtz(-1) != 1 || main_viaCtz((int64_t)1 << 62) != 63;
}
)C");
  }
}

TEST(GoStackResults, ARecordOfStackWordsLeavesNoRegisterArgument) {
  // `main.leaves` adds R9 as it found it: under the stack ABI that is no
  // argument, whatever its caller left there.  Read as one, it made the
  // function take the platform's registers, its caller pass R9 on as a
  // parameter of its own, and neither return its stack result.
  //   main.leaves(x) int: mov rax, [rsp+8]; add rax, r9; mov [rsp+16], rax
  //                       ret
  //   main.user(x) int:   sub rsp, 0x18; mov rax, [rsp+0x20]; mov [rsp], rax
  //                       call main.leaves; mov rax, [rsp+8]
  //                       mov [rsp+0x28], rax; add rsp, 0x18; ret
  const std::vector<uint8_t> Code = {
      0x48, 0x8B, 0x44, 0x24, 0x08, 0x4C, 0x01, 0xC8, 0x48, 0x89,
      0x44, 0x24, 0x10, 0xC3, 0x66, 0x90, 0x48, 0x83, 0xEC, 0x18,
      0x48, 0x8B, 0x44, 0x24, 0x20, 0x48, 0x89, 0x04, 0x24, 0xE8,
      0xDE, 0xFF, 0xFF, 0xFF, 0x48, 0x8B, 0x44, 0x24, 0x08, 0x48,
      0x89, 0x44, 0x24, 0x28, 0x48, 0x83, 0xC4, 0x18, 0xC3};
  const Routine Routines[] = {{"main.leaves", 0x00, 16},
                              {"main.user", 0x10, 16}};
  const Decompiled D(Arch::X64, Code, Routines);
  ASSERT_TRUE(D.Result.Success) << D.Result.Error;
  for (const char *Name : {"main.leaves", "main.user"}) {
    SCOPED_TRACE(Name);
    const MedFunc *F = D.function(Name);
    ASSERT_NE(F, nullptr);
    EXPECT_TRUE(F->StackArgumentsOnly);
    EXPECT_EQ(F->Params.size(), 1u);
    EXPECT_EQ(F->GoStackResults.size(), 1u);
  }
  // The caller passes its one argument and returns the result; the unknown
  // value is where it is read.
  const std::string User = D.body("main_user");
  EXPECT_NE(User.find("main_leaves(arg0)"), std::string::npos) << User;
  EXPECT_EQ(User.find("__builtin_trap"), std::string::npos) << User;
  EXPECT_NE(D.body("main_leaves").find("incoming register has no definition"),
            std::string::npos)
      << D.Source;
}

TEST(GoStackResults, ARunOfOneRoutineReturnsWhatTheImageDoes) {
  // The callee is left out of the run: its body is read for its results as
  // it is for its arguments, the routine it jumps to as well.
  for (const char *Name : {"main.caller", "main.viaWrap"}) {
    SCOPED_TRACE(Name);
    const bool Wrapped = std::string(Name) == "main.viaWrap";
    const Decompiled D(Arch::X64, x64Code(), X64Routines, /*LLVM=*/false,
                       {kCode + (Wrapped ? 0x0F0u : 0x010u)});
    ASSERT_TRUE(D.Result.Success) << D.Result.Error;
    EXPECT_EQ(D.Source.find("__builtin_trap"), std::string::npos) << D.Source;
    EXPECT_NE(D.Source.find(Wrapped ? "main_wrap(arg0, (uint64_t)5);"
                                    : "main_add(arg0, (uint64_t)5);"),
              std::string::npos)
        << D.Source;
    // The routine left out is the oracle's own.
    source_call_execution_test::compileAndRun(D.Source + (Wrapped ? R"C(
int64_t wrap(int64_t a, int64_t b) __asm__("main.wrap");
int64_t wrap(int64_t a, int64_t b) { return a + 1 + b; }
int main(void) {
  for (int64_t x = -6; x < 40; x += 5)
    if (main_viaWrap(x) != x + 7)
      return 1;
  return 0;
}
)C"
                                                                  : R"C(
int64_t add(int64_t a, int64_t b) __asm__("main.add");
int64_t add(int64_t a, int64_t b) { return a + b; }
int main(void) {
  for (int64_t x = -6; x < 40; x += 5)
    if (main_caller(x) != x + 6)
      return 1;
  return 0;
}
)C"));
  }
}

} // namespace
