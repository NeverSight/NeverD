//===- GoFloatResultsTests.cpp - Floating results of Go calls -------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Go's internal ABI returns a call's floating results in X0-X14 (arm64
/// F0-F15), numbered apart from its integer ones.  These tests run the C of
/// small register-ABI routines and check each caller reads what its callee
/// returns in both banks.
///
//===----------------------------------------------------------------------===//

#include "../language/GoRuntimeEHTestsDetail.h"
#include "SourceCallExecution.h"
#include "gtest/gtest.h"

#include "neverd/backend/c/CEmitterOptions.h"
#include "neverd/backend/c/HighC/HighCEmitter.h"
#include "neverd/backend/c/LLVMC/LLVMCEmitter.h"
#include "neverd/lift/AArch64/AArch64Regs.h"
#include "neverd/lift/X86/X86Regs.h"
#include "neverd/pipeline/Pipeline.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#include <cstring>
#include <string>
#include <vector>

namespace {
using namespace neverd;
using namespace neverd::go_eh_test;

constexpr va_t kCode = kTextVA + 0x100;

/// The routines of the fixture and where each starts in its code.
struct Routine {
  const char *Name;
  uint32_t Offset;
};
const Routine Routines[] = {
    {"main.split", 0x00},   {"main.diff", 0x20},    {"main.pair", 0x60},
    {"main.usepair", 0x80}, {"main.fwd", 0xC0},     {"main.usefwd", 0xE0},
    {"main.one", 0x120},    {"main.useone", 0x140},
};

/// x86-64 routines, as Go's compiler passes and returns them: p in RAX,
/// flag in RBX, integer results from RAX and floating ones from X0.
///   main.split(p *[2]float64) (float64, float64):
///                       movsd xmm0, [rax]; movsd xmm1, [rax+8]; ret
///   main.diff(p, flag) float64:
///                       push rbp; mov rbp, rsp; sub rsp, 16; mov [rsp], rbx
///                       call main.split; cmp qword [rsp], 0; je 1f
///                       subsd xmm0, xmm1; 1: add rsp, 16; pop rbp; ret
///   main.pair(p *struct{n int; f float64}) (int, float64):
///                       movsd xmm0, [rax+8]; mov rax, [rax]; ret
///   main.usepair(p, flag) float64: as main.diff, calling main.pair, with
///                       xorps xmm1, xmm1; cvtsi2sd xmm1, rax
///                       addsd xmm0, xmm1
///   main.fwd(p) (float64, float64):
///                       push rbp; mov rbp, rsp; call main.split; pop rbp
///                       ret
///   main.usefwd(p, flag) float64: as main.diff, calling main.fwd
///   main.one(p) float64: movsd xmm0, [rax]; ret
///   main.useone(p, flag) float64: as main.diff, calling main.one, which
///                       leaves X1 as it found it
const std::vector<uint8_t> &x64Code() {
  static const std::vector<uint8_t> Code = {
      0xF2, 0x0F, 0x10, 0x00, 0xF2, 0x0F, 0x10, 0x48, 0x08, 0xC3, 0xCC, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x55, 0x48, 0x89, 0xE5,
      0x48, 0x83, 0xEC, 0x10, 0x48, 0x89, 0x1C, 0x24, 0xE8, 0xCF, 0xFF, 0xFF,
      0xFF, 0x48, 0x83, 0x3C, 0x24, 0x00, 0x74, 0x04, 0xF2, 0x0F, 0x5C, 0xC1,
      0x48, 0x83, 0xC4, 0x10, 0x5D, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0xF2, 0x0F, 0x10, 0x40, 0x08, 0x48, 0x8B, 0x00, 0xC3, 0xCC, 0xCC, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x55, 0x48, 0x89, 0xE5,
      0x48, 0x83, 0xEC, 0x10, 0x48, 0x89, 0x1C, 0x24, 0xE8, 0xCF, 0xFF, 0xFF,
      0xFF, 0x48, 0x83, 0x3C, 0x24, 0x00, 0x74, 0x0C, 0x0F, 0x57, 0xC9, 0xF2,
      0x48, 0x0F, 0x2A, 0xC8, 0xF2, 0x0F, 0x58, 0xC1, 0x48, 0x83, 0xC4, 0x10,
      0x5D, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0x55, 0x48, 0x89, 0xE5, 0xE8, 0x37, 0xFF, 0xFF, 0xFF, 0x5D, 0xC3, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x55, 0x48, 0x89, 0xE5,
      0x48, 0x83, 0xEC, 0x10, 0x48, 0x89, 0x1C, 0x24, 0xE8, 0xCF, 0xFF, 0xFF,
      0xFF, 0x48, 0x83, 0x3C, 0x24, 0x00, 0x74, 0x04, 0xF2, 0x0F, 0x5C, 0xC1,
      0x48, 0x83, 0xC4, 0x10, 0x5D, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0xF2, 0x0F, 0x10, 0x00, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
      0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x55, 0x48, 0x89, 0xE5,
      0x48, 0x83, 0xEC, 0x10, 0x48, 0x89, 0x1C, 0x24, 0xE8, 0xCF, 0xFF, 0xFF,
      0xFF, 0x48, 0x83, 0x3C, 0x24, 0x00, 0x74, 0x04, 0xF2, 0x0F, 0x5C, 0xC1,
      0x48, 0x83, 0xC4, 0x10, 0x5D, 0xC3,
  };
  return Code;
}

/// The same on arm64: p in R0, flag in R1, results from R0 and F0.
///   main.split:   ldr d0, [x0]; ldr d1, [x0, #8]; ret
///   main.diff:    stp x29, x30, [sp, #-32]!; mov x29, sp; str x1, [sp, #16]
///                 bl main.split; ldr x1, [sp, #16]; cbz x1, 1f
///                 fsub d0, d0, d1; 1: ldp x29, x30, [sp], #32; ret
///   main.pair:    ldr d0, [x0, #8]; ldr x0, [x0]; ret
///   main.usepair: as main.diff, calling main.pair, with scvtf d1, x0;
///                 fadd d0, d0, d1
///   main.fwd:     stp x29, x30, [sp, #-16]!; mov x29, sp; bl main.split
///                 ldp x29, x30, [sp], #16; ret
///   main.usefwd:  as main.diff, calling main.fwd
///   main.one:     ldr d0, [x0]; ret
///   main.useone:  as main.diff, calling main.one
const std::vector<uint8_t> &arm64Code() {
  static const std::vector<uint8_t> Code = [] {
    const uint32_t Words[] = {
        0xFD400000, 0xFD400401, 0xD65F03C0, 0xD503201F, 0xD503201F, 0xD503201F,
        0xD503201F, 0xD503201F, 0xA9BE7BFD, 0x910003FD, 0xF9000BE1, 0x97FFFFF5,
        0xF9400BE1, 0xB4000041, 0x1E613800, 0xA8C27BFD, 0xD65F03C0, 0xD503201F,
        0xD503201F, 0xD503201F, 0xD503201F, 0xD503201F, 0xD503201F, 0xD503201F,
        0xFD400400, 0xF9400000, 0xD65F03C0, 0xD503201F, 0xD503201F, 0xD503201F,
        0xD503201F, 0xD503201F, 0xA9BE7BFD, 0x910003FD, 0xF9000BE1, 0x97FFFFF5,
        0xF9400BE1, 0xB4000061, 0x9E620001, 0x1E612800, 0xA8C27BFD, 0xD65F03C0,
        0xD503201F, 0xD503201F, 0xD503201F, 0xD503201F, 0xD503201F, 0xD503201F,
        0xA9BF7BFD, 0x910003FD, 0x97FFFFCE, 0xA8C17BFD, 0xD65F03C0, 0xD503201F,
        0xD503201F, 0xD503201F, 0xA9BE7BFD, 0x910003FD, 0xF9000BE1, 0x97FFFFF5,
        0xF9400BE1, 0xB4000041, 0x1E613800, 0xA8C27BFD, 0xD65F03C0, 0xD503201F,
        0xD503201F, 0xD503201F, 0xD503201F, 0xD503201F, 0xD503201F, 0xD503201F,
        0xFD400000, 0xD65F03C0, 0xD503201F, 0xD503201F, 0xD503201F, 0xD503201F,
        0xD503201F, 0xD503201F, 0xA9BE7BFD, 0x910003FD, 0xF9000BE1, 0x97FFFFF5,
        0xF9400BE1, 0xB4000041, 0x1E613800, 0xA8C27BFD, 0xD65F03C0,
    };
    std::vector<uint8_t> Bytes(sizeof(Words));
    std::memcpy(Bytes.data(), Words, sizeof(Words));
    return Bytes;
  }();
  return Code;
}

/// What the pipeline makes of the fixture on \p A: its result and the C of
/// every routine, HighC or, with \p LLVM, LLVM C.
struct Decompiled {
  GoTestImage Image;
  /// Outlives the module in Result.
  llvm::LLVMContext Context;
  PipelineResult Result;
  std::string Source;

  Decompiled(Arch A, bool LLVM) : Image(A) {
    Image.writeText(kCode, A == Arch::X64 ? x64Code() : arm64Code());
    GoModuleInfo Module;
    Module.PclnTabMagic = 0xFFFFFFF1u;
    Module.RegisterArguments = true;
    Module.GoCode = {{kTextVA, kTextVA + kTextSize}};
    for (const Routine &R : Routines) {
      Symbol Function = Symbol::makeFunc(kCode + R.Offset);
      Function.Name = R.Name;
      Image.Img.Symbols.push_back(std::move(Function));
    }
    Image.Img.ExceptionMetadata.GoModule = std::move(Module);
    PipelineOptions Options;
    Options.EmitDumpOutput = false;
    Options.LiftMode = LLVM;
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

  /// The call \p Caller makes.
  const MedOp *call(const char *Caller) const {
    const MedFunc *F = function(Caller);
    if (!F)
      return nullptr;
    for (const MedBlock &Block : F->Blocks)
      for (const MedOp &Op : Block.Ops)
        if (Op.Opcode == NdOp::CALL)
          return &Op;
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

/// The registers and widths of \p Fields, and of floats.
std::vector<std::tuple<uint64_t, uint16_t, bool>>
fields(llvm::ArrayRef<MedReturnReg> Fields) {
  std::vector<std::tuple<uint64_t, uint16_t, bool>> Shape;
  for (const MedReturnReg &Field : Fields)
    Shape.emplace_back(Field.RegOff, Field.Size, Field.IsFP);
  return Shape;
}

uint64_t integerResult(Arch A) {
  return A == Arch::X64 ? x86reg::RAX : a64reg::X0;
}

/// Whether this host builds and runs the C of \p A: x86-64 C keeps the SSE
/// state helpers its floating operations print, inline assembly only an
/// x86-64 host assembles; arm64 C is portable.
bool runsHere(Arch A) {
#if defined(__x86_64__) || defined(_M_X64)
  return true;
#else
  return A != Arch::X64;
#endif
}

uint64_t floatResult(Arch A, int Position) {
  return A == Arch::X64
             ? x86reg::VectorBase + Position * x86reg::VectorRegStride
             : a64reg::V0 + Position * (a64reg::V(1) - a64reg::V0);
}

/// The C a test appends to the fixture's: \p Main, with D(x), the double in
/// the low bytes of x whatever C types it as (LLVM C may type a Go result in
/// X0 as the whole register), and a pointer passed to a routine that types
/// its first parameter an integer.
std::string driver(const std::string &Main) {
  return "#define D(x) ((union { __typeof__(x) v; double d; }){x}.d)\n" + Main;
}
const llvm::StringRef DriverFlags[] = {"-Wno-int-conversion"};

TEST(GoFloatResults, ACallReturnsBothFloatsItsCalleeDoes) {
  // main.split returns two floats and no integer: its record holds X0 and
  // X1 alone, which main.diff reads below a branch, apart from the call's
  // block.  The struct-return heuristic gave Go calls a RAX field from the
  // return's default operand, and no record where the reads were not
  // straight after the call.
  for (const Arch A : {Arch::X64, Arch::AArch64})
    for (const bool LLVM : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(A) * 2 + LLVM);
      const Decompiled D(A, LLVM);
      ASSERT_TRUE(D.Result.Success) << D.Result.Error;
      ASSERT_FALSE(D.Source.empty());
      const MedFunc *Split = D.function("main.split");
      ASSERT_NE(Split, nullptr);
      const std::vector<std::tuple<uint64_t, uint16_t, bool>> Floats = {
          {floatResult(A, 0), 8, true}, {floatResult(A, 1), 8, true}};
      EXPECT_EQ(fields(Split->MultiReturn), Floats);
      const MedOp *Call = D.call("main.diff");
      ASSERT_NE(Call, nullptr);
      ASSERT_TRUE(Call->RegisterReturnFields);
      EXPECT_EQ(fields(*Call->RegisterReturnFields), Floats);
      EXPECT_EQ(D.body("main_diff").find("__builtin_trap"), std::string::npos)
          << D.body("main_diff");
      if (!runsHere(A))
        continue;
      source_call_execution_test::compileAndRun(D.Source + driver(R"C(
int main(void) {
  static double p[2] = {7.5, 2.25};
  if (D(main_diff(p, 1)) != 5.25 || D(main_diff(p, 0)) != 7.5)
    return 1;
  return 0;
}
)C"),
                                                DriverFlags);
    }
}

TEST(GoFloatResults, ACallReturnsAnIntegerBesideAFloat) {
  // main.pair returns an int in RAX and a float in X0, each the first of
  // its bank.
  for (const Arch A : {Arch::X64, Arch::AArch64})
    for (const bool LLVM : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(A) * 2 + LLVM);
      const Decompiled D(A, LLVM);
      ASSERT_TRUE(D.Result.Success) << D.Result.Error;
      const MedFunc *Pair = D.function("main.pair");
      ASSERT_NE(Pair, nullptr);
      const std::vector<std::tuple<uint64_t, uint16_t, bool>> Results = {
          {integerResult(A), 8, false}, {floatResult(A, 0), 8, true}};
      EXPECT_EQ(fields(Pair->MultiReturn), Results);
      const MedOp *Call = D.call("main.usepair");
      ASSERT_NE(Call, nullptr);
      ASSERT_TRUE(Call->RegisterReturnFields);
      EXPECT_EQ(fields(*Call->RegisterReturnFields), Results);
      EXPECT_EQ(D.body("main_usepair").find("__builtin_trap"),
                std::string::npos)
          << D.body("main_usepair");
      if (!runsHere(A))
        continue;
      source_call_execution_test::compileAndRun(D.Source + driver(R"C(
int main(void) {
  static struct { int64_t n; double f; } s = {3, 0.5};
  if (D(main_usepair(&s, 1)) != 3.5 || D(main_usepair(&s, 0)) != 0.5)
    return 1;
  return 0;
}
)C"),
                                                DriverFlags);
    }
}

TEST(GoFloatResults, AForwardingCallReturnsWhatItsCalleeDoes) {
  // main.fwd hands back what main.split returns, so its callers' read of
  // X1 is a read of main.split's.
  for (const Arch A : {Arch::X64, Arch::AArch64})
    for (const bool LLVM : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(A) * 2 + LLVM);
      const Decompiled D(A, LLVM);
      ASSERT_TRUE(D.Result.Success) << D.Result.Error;
      const MedFunc *Fwd = D.function("main.fwd");
      ASSERT_NE(Fwd, nullptr);
      const std::vector<std::tuple<uint64_t, uint16_t, bool>> Floats = {
          {floatResult(A, 0), 8, true}, {floatResult(A, 1), 8, true}};
      EXPECT_EQ(fields(Fwd->MultiReturn), Floats);
      const MedOp *Call = D.call("main.usefwd");
      ASSERT_NE(Call, nullptr);
      ASSERT_TRUE(Call->RegisterReturnFields);
      EXPECT_EQ(fields(*Call->RegisterReturnFields), Floats);
      if (!runsHere(A))
        continue;
      source_call_execution_test::compileAndRun(D.Source + driver(R"C(
int main(void) {
  static double p[2] = {-1.5, 0.25};
  if (D(main_usefwd(p, 1)) != -1.75 || D(main_usefwd(p, 0)) != -1.5)
    return 1;
  return 0;
}
)C"),
                                                DriverFlags);
    }
}

TEST(GoFloatResults, AFloatNoReturnWritesIsNoResult) {
  // main.one writes X0 alone, so no return of it supplies X1: main.useone's
  // read of X1 after the call is no result of main.one, whose record stays
  // the float it returns.  The read keeps what X1 held before the call,
  // which main.one leaves alone.
  for (const Arch A : {Arch::X64, Arch::AArch64})
    for (const bool LLVM : {false, true}) {
      SCOPED_TRACE(static_cast<unsigned>(A) * 2 + LLVM);
      const Decompiled D(A, LLVM);
      ASSERT_TRUE(D.Result.Success) << D.Result.Error;
      const MedFunc *One = D.function("main.one");
      ASSERT_NE(One, nullptr);
      EXPECT_TRUE(One->MultiReturn.empty());
      const MedOp *Call = D.call("main.useone");
      ASSERT_NE(Call, nullptr);
      EXPECT_FALSE(Call->RegisterReturnFields);
      EXPECT_EQ(D.body("main_useone").find("field_"), std::string::npos)
          << D.body("main_useone");
    }
}
} // namespace
