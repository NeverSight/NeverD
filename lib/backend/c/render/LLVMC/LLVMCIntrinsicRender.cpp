//===- LLVMCIntrinsicRender.cpp - LLVM intrinsic rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Arch-dispatching entry points for inline-asm rendering, fence emission,
/// and debug-break selection.  Architecture-specific implementations live in
/// LLVMCIntrinsicRenderX86.cpp and LLVMCIntrinsicRenderARM.cpp.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"

namespace neverd {

InlineAsmRender renderInlineAsm(Arch TheArch, const std::string &AsmStr,
                                bool IsStructReturn,
                                const std::string &ResultName, bool ResultLive,
                                const std::vector<std::string> &Args) {

  std::string Mnemonic = AsmStr;
  auto SpacePos = Mnemonic.find(' ');
  if (SpacePos != std::string::npos)
    Mnemonic = Mnemonic.substr(0, SpacePos);

  if (TheArch == Arch::ARM || TheArch == Arch::AArch64)
    return renderARMInlineAsm(AsmStr, Mnemonic, ResultName, ResultLive, Args);

  if (TheArch == Arch::X86 || TheArch == Arch::X64)
    return renderX86InlineAsm(AsmStr, Mnemonic, IsStructReturn, ResultName,
                              ResultLive, Args);

  return {"__asm__ volatile(\"" + AsmStr + "\" ::: \"memory\");\n", false};
}

std::string renderFence(Arch TheArch, llvm::AtomicOrdering Ordering) {
  if (TheArch == Arch::ARM || TheArch == Arch::AArch64)
    return renderARMFence(Ordering);
  return renderX86Fence(Ordering);
}

const char *renderDebugBreak(Arch TheArch) {
  if (TheArch == Arch::ARM || TheArch == Arch::AArch64)
    return renderARMDebugBreak();
  return renderX86DebugBreak();
}

} // namespace neverd
