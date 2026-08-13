//===- RewriteCodegenHarness.h - rewrite codegen + Unicorn verify -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
//
// Shared harness for the rewrite-backend codegen round-trip suite
// (RewriteCodegen*RTTests.cpp).  Verifies that the rewrite backend
// (AddressModelBackend) produces correct machine code by:
//   1. Building LLVM IR programmatically for a simple function
//   2. Compiling via Codegen::compileForRewrite() to get fixed-up bytes
//   3. Loading the bytes into Unicorn Engine at the target VA
//   4. Emulating and checking register results
//
// This lets us verify rewrite-backend output for architectures we cannot
// natively run (x86-64 on arm64 macOS, ARM32 on arm64 macOS, etc.).
//
// Declarations only: the definitions live in RewriteCodegenHarness.cpp so
// the twenty-plus round-trip TUs that share them do not emit duplicate
// symbols.
//
//===----------------------------------------------------------------------===//


#ifndef NEVERD_UNITTESTS_SEMANTIC_COMMON_REWRITECODEGENHARNESS_H
#define NEVERD_UNITTESTS_SEMANTIC_COMMON_REWRITECODEGENHARNESS_H

#include "neverd/backend/codegen/BinaryRewriter.h"
#include "neverd/backend/codegen/CodeGen.h"
#include "neverd/object/SectionNames.h"
#include "neverd/pass/mir/MIRPass.h"
#include "neverd/pass/mir/NOPPass.h"

#include "UnicornSemanticFixture.h"

#include "llvm/ADT/SmallString.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/BinaryRewrite.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/TargetSelect.h"

#include <cassert>
#include <functional>
#include <memory>
#include <optional>

namespace rwcg {

inline constexpr uint64_t CODE_VA = 0x400000;
inline constexpr uint64_t DATA_VA = 0x500000;

// VA of the tiny "external_fn" stub the external-call tests plant in Unicorn.
// The stub doubles its argument:
//   x86-64 stub:  lea eax, [rdi+rdi]; ret
//   AArch64 stub: add w0, w0, w0; ret
//   ARM32 stub:   add r0, r0, r0; bx lr
inline constexpr uint64_t EXT_FN_VA = 0x600000;

using ResolveCallback =
    std::function<std::optional<uint64_t>(llvm::StringRef, uint32_t)>;

void ensureLLVMTargets();

// int add(int a, int b) { return a + b; }
std::unique_ptr<llvm::Module> buildAddIR(llvm::LLVMContext &Ctx,
                                         const char *Triple);

// int compute(int x) { return x * 3 + 7; }
std::unique_ptr<llvm::Module> buildComputeIR(llvm::LLVMContext &Ctx,
                                             const char *Triple);

// @counter = global i32 42; int get_and_inc() { counter++; return counter; }
std::unique_ptr<llvm::Module> buildDataRefIR(llvm::LLVMContext &Ctx,
                                             const char *Triple);

// int helper(int x) { return x * 2 + 1; }
// int caller(int a, int b) { return helper(a) + helper(b); }
std::unique_ptr<llvm::Module> buildCrossCallIR(llvm::LLVMContext &Ctx,
                                               const char *Triple);

// declare i32 @external_fn(i32); int wrap_ext(int x) { return external_fn(x + 10); }
// DSOLocal controls whether the call goes direct (dso_local=true) or via @PLT.
std::unique_ptr<llvm::Module> buildExternalCallIR(llvm::LLVMContext &Ctx,
                                                  const char *Triple,
                                                  bool DSOLocal = true);

// int (*external_slot)(int) = external_fn;
// int call_external_ptr(int x) { return external_slot(x); }
std::unique_ptr<llvm::Module>
buildExternalFunctionPointerIR(llvm::LLVMContext &Ctx, const char *Triple);

// int sum_to(int n) { int s=0; for(int i=1;i<=n;i++) s+=i; return s; }
std::unique_ptr<llvm::Module> buildLoopIR(llvm::LLVMContext &Ctx,
                                          const char *Triple);

// int classify(int x) { switch(x) { case 0..3: return 10..40; default: -1; } }
std::unique_ptr<llvm::Module> buildSwitchIR(llvm::LLVMContext &Ctx,
                                            const char *Triple);

// @g1 = 111, @g2 = 222; int sum_globals() { g1 += 1; g2 += 2; return g1 + g2; }
std::unique_ptr<llvm::Module> buildMultiGlobalIR(llvm::LLVMContext &Ctx,
                                                 const char *Triple);

// int float_add_trunc(int a, int b) { return (int)((float)a + (float)b); }
std::unique_ptr<llvm::Module> buildFloatAddTruncIR(llvm::LLVMContext &Ctx,
                                                   const char *Triple);

// f0(x)=x+1, ..., f9(x)=f8(x)+1; chain_entry(x) = f9(x) -> x + 10
std::unique_ptr<llvm::Module> buildChainedFunctionsIR(llvm::LLVMContext &Ctx,
                                                      const char *Triple);

// int64_t wide_mul(int64_t a, int64_t b) { return a * b + 7; }
std::unique_ptr<llvm::Module> buildWideMulIR(llvm::LLVMContext &Ctx,
                                             const char *Triple);

// int abs_diff(int a, int b) { int d = a - b; return d >= 0 ? d : -d; }
std::unique_ptr<llvm::Module> buildAbsDiffIR(llvm::LLVMContext &Ctx,
                                             const char *Triple);

// Isolated smax/smin clamp builder -- diagnoses LLVM fork codegen.
std::unique_ptr<llvm::Module> buildSmaxSminClampIR(llvm::LLVMContext &Ctx,
                                                   const char *Triple);

// Compile Mod for in-place rewriting at the fixed CODE_VA / DATA_VA layout.
llvm::mc_rewrite::RewriteResult compileRewrite(llvm::Module &Mod, neverd::Arch Ar,
                                               neverd::BinaryFormat Fmt,
                                               ResolveCallback Resolve = nullptr);

// Same, but with caller-chosen text/data virtual addresses.
llvm::mc_rewrite::RewriteResult
compileRewriteWithVAs(llvm::Module &Mod, neverd::Arch Ar, neverd::BinaryFormat Fmt,
                      uint64_t CodeVA, uint64_t DataVA,
                      ResolveCallback Resolve = nullptr);

const llvm::mc_rewrite::RewriteSection *
findDataSection(const llvm::mc_rewrite::RewriteResult &R);

const llvm::mc_rewrite::RewriteSection *
findTextSection(const llvm::mc_rewrite::RewriteResult &R);

// MachO prefixes symbols with '_'; this helper checks both variants.
uint64_t findSymbolVA(const llvm::mc_rewrite::RewriteResult &R, const char *Name,
                      uint64_t Default);

} // namespace rwcg

#endif // NEVERD_UNITTESTS_SEMANTIC_COMMON_REWRITECODEGENHARNESS_H
