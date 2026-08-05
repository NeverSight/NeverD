//===- LLVMCIntrinsicRender.h - LLVM intrinsic rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Renders LLVM intrinsic calls to inline assembly or ACLE C source.
///
/// Implementation split across:
///   LLVMCIntrinsicRender.cpp      — arch-dispatching: renderInlineAsm,
///                                   renderFence, renderDebugBreak
///   LLVMCIntrinsicRenderX86.cpp   — x86 inline-asm rendering & table
///   LLVMCIntrinsicRenderARM.cpp   — ARM/AArch64 inline-asm rendering & table
///
//===----------------------------------------------------------------------===//

#ifndef NEVERD_BACKEND_C_RENDER_LLVMC_LLVMCINTRINSICRENDER_H
#define NEVERD_BACKEND_C_RENDER_LLVMC_LLVMCINTRINSICRENDER_H
#include "neverd/Common.h"

#include "llvm/Support/AtomicOrdering.h"

#include <string>
#include <vector>

namespace neverd {

struct AsmToCEntry {
  const char *AsmStr;
  const char *CName;
};

struct InlineAsmRender {
  std::string Code;
  bool SetIntrinsics = false;
};

//--- Arch-specific (LLVMCIntrinsicRenderX86.cpp) ---
const char *lookupX86AsmToC(const char *Mnem);
InlineAsmRender
renderX86InlineAsm(const std::string &AsmStr, const std::string &Mnemonic,
                   bool IsStructReturn, const std::string &ResultName,
                   bool ResultLive, const std::vector<std::string> &Args);
std::string renderX86Fence(llvm::AtomicOrdering Ordering);
const char *renderX86DebugBreak();

//--- Arch-specific (LLVMCIntrinsicRenderARM.cpp) ---
const char *lookupArmAsmToC(const char *Mnem);
InlineAsmRender renderARMInlineAsm(const std::string &AsmStr,
                                   const std::string &Mnemonic,
                                   const std::string &ResultName,
                                   bool ResultLive,
                                   const std::vector<std::string> &Args);
std::string renderARMFence(llvm::AtomicOrdering Ordering);
const char *renderARMDebugBreak();

//--- Dispatchers (LLVMCIntrinsicRender.cpp) ---
InlineAsmRender renderInlineAsm(Arch TheArch, const std::string &AsmStr,
                                bool IsStructReturn,
                                const std::string &ResultName, bool ResultLive,
                                const std::vector<std::string> &Args);

std::string renderFence(Arch TheArch, llvm::AtomicOrdering Ordering);

/// Returns the arch-appropriate debug-break statement (with trailing newline).
const char *renderDebugBreak(Arch TheArch);

} // namespace neverd

#endif // NEVERD_BACKEND_C_RENDER_LLVMC_LLVMCINTRINSICRENDER_H
