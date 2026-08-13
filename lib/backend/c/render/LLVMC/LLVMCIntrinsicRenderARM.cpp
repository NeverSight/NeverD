//===- LLVMCIntrinsicRenderARM.cpp - ARM inline-asm rendering ---*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// ARM/AArch64-specific inline-assembly-to-C rendering: asm-mnemonic lookup
/// table and GCC __asm__ volatile / ACLE translation.
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"

#include "llvm/Support/AtomicOrdering.h"

#include <cstring>

namespace neverd {

namespace {

static const AsmToCEntry ARMAsmTable[] = {
#define M(a, c) {a, c},
#include "AsmToCARM.inc"
#undef M
};

} // anonymous namespace

const char *lookupArmAsmToC(const char *Mnem) {
  for (const auto &E : ARMAsmTable)
    if (std::strcmp(Mnem, E.AsmStr) == 0)
      return E.CName;
  return nullptr;
}

InlineAsmRender renderARMInlineAsm(const std::string &AsmStr,
                                   const std::string &Mnemonic,
                                   const std::string &ResultName,
                                   bool ResultLive,
                                   const std::vector<std::string> &Args) {
  if (Mnemonic == "sel" && Args.size() >= 2) {
    std::string Result;
    if (ResultLive && !ResultName.empty())
      Result = ResultName + " = ";
    Result += "__sel(" + Args[0] + ", " + Args[1] + ");\n";
    return {Result, true};
  }

  const char *ArmC = lookupArmAsmToC(AsmStr.c_str());
  if (!ArmC)
    ArmC = lookupArmAsmToC(Mnemonic.c_str());
  if (ArmC)
    return {std::string(ArmC) + ";\n", true};

  std::string Result = "__asm__ volatile(\"" + AsmStr + "\"";
  if (!Args.empty()) {
    Result += " : : ";
    for (size_t I = 0; I < Args.size(); ++I) {
      if (I > 0)
        Result += ", ";
      Result += "\"r\"(" + Args[I] + ")";
    }
    Result += " :";
  } else {
    Result += " :::";
  }
  Result += " \"memory\");\n";
  return {Result, false};
}

std::string renderARMFence(llvm::AtomicOrdering Ordering) {
  switch (Ordering) {
  case llvm::AtomicOrdering::SequentiallyConsistent:
    return "__dmb(0xF);\n";
  case llvm::AtomicOrdering::Acquire:
    return "__dmb(0x9);\n";
  case llvm::AtomicOrdering::Release:
    return "__dmb(0xA);\n";
  default:
    return "__asm__ volatile(\"dmb ish\" ::: \"memory\");\n";
  }
}

const char *renderARMDebugBreak() { return "__builtin_debugtrap();\n"; }

} // namespace neverd
