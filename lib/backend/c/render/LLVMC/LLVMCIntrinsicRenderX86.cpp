//===- LLVMCIntrinsicRenderX86.cpp - x86 inline-asm rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86-specific inline-assembly-to-C rendering: asm-mnemonic lookup table
/// and MSVC __asm / intrinsic translation (CPUID, XGETBV, etc.).
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"

#include "llvm/Support/AtomicOrdering.h"

#include <cstring>

namespace neverd {

namespace {

static const AsmToCEntry X86AsmTable[] = {
#define M(a, c) {a, c},
#include "AsmToCX86.inc"
#undef M
};

} // anonymous namespace

const char *lookupX86AsmToC(const char *Mnem) {
  for (const auto &E : X86AsmTable)
    if (std::strcmp(Mnem, E.AsmStr) == 0)
      return E.CName;
  return nullptr;
}

InlineAsmRender
renderX86InlineAsm(const std::string &AsmStr, const std::string &Mnemonic,
                   bool IsStructReturn, const std::string &ResultName,
                   bool ResultLive, const std::vector<std::string> &Args) {
  if (Mnemonic == "cpuid") {
    std::string Leaf = Args.empty() ? "0" : Args[0];
    if (IsStructReturn)
      return {"int cpuInfo[4]; __cpuid(cpuInfo, " + Leaf + ");\n", true};
    return {"{ int cpuInfo[4]; __cpuid(cpuInfo, " + Leaf + "); }\n", true};
  }

  if (Mnemonic == "xgetbv") {
    std::string ECX = Args.empty() ? "0" : Args[0];
    if (IsStructReturn) {
      return {"uint32_t xcr[2]; { uint64_t _t = _xgetbv(" + ECX +
                  "); xcr[0] = (uint32_t)_t; xcr[1] = (uint32_t)(_t >> "
                  "32); }\n",
              true};
    }
    std::string Result;
    if (ResultLive && !ResultName.empty())
      Result = ResultName + " = ";
    Result += "_xgetbv(" + ECX + ");\n";
    return {Result, true};
  }

  const char *X86C = lookupX86AsmToC(AsmStr.c_str());
  if (!X86C)
    X86C = lookupX86AsmToC(Mnemonic.c_str());
  if (X86C) {
    std::string Result;
    if (ResultLive && !ResultName.empty())
      Result = ResultName + " = ";
    Result += std::string(X86C) + "(";
    for (size_t I = 0; I < Args.size(); ++I) {
      if (I > 0)
        Result += ", ";
      Result += Args[I];
    }
    Result += ");\n";
    return {Result, true};
  }

  std::string Result = "__asm { " + Mnemonic;
  for (size_t I = 0; I < Args.size(); ++I) {
    Result += (I == 0 ? " " : ", ");
    Result += Args[I];
  }
  Result += " }\n";
  return {Result, false};
}

std::string renderX86Fence(llvm::AtomicOrdering Ordering) {
  switch (Ordering) {
  case llvm::AtomicOrdering::SequentiallyConsistent:
    return "_mm_mfence();\n";
  case llvm::AtomicOrdering::Acquire:
    return "_mm_lfence();\n";
  case llvm::AtomicOrdering::Release:
    return "_mm_sfence();\n";
  default:
    return "__asm { mfence }\n";
  }
}

const char *renderX86DebugBreak() { return "__debugbreak();\n"; }

} // namespace neverd
