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
#include "neverd/ir/intrinsics/IntrinsicInstructionText.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/AtomicOrdering.h"

#include <cstring>

namespace neverd {

namespace {

static const AsmToCEntry ARMAsmTable[] = {
#define M(a, c) {a, c},
#include "LLVMCAsmToCARM.inc"
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

  // A barrier runs as the ACLE intrinsic of its own option: `dsb ishst` is
  // `__dsb(0xA)`, not the mnemonic's default.
  if (Mnemonic == "dmb" || Mnemonic == "dsb" || Mnemonic == "isb") {
    const llvm::StringRef Option =
        llvm::StringRef(AsmStr).drop_front(Mnemonic.size()).trim();
    const std::optional<uint64_t> CRm = Option.empty()
                                            ? std::optional<uint64_t>(0xF)
                                            : armBarrierOptionValue(Option);
    if (CRm && Args.empty())
      return {"__" + Mnemonic + "(0x" + llvm::utohexstr(*CRm) + ");" +
                  (Option.empty() ? "" : " /* " + AsmStr + " */") + "\n",
              true};
  }
  const char *ArmC = lookupArmAsmToC(AsmStr.c_str());
  // A trap or a breakpoint builtin takes no code: the instruction it was
  // lifted from goes beside it (`__builtin_trap(); /* udf #0xFE */`).
  std::string Comment;
  if (!ArmC) {
    ArmC = lookupArmAsmToC(Mnemonic.c_str());
    if (ArmC && AsmStr != Mnemonic)
      Comment = " /* " + AsmStr + " */";
  }
  if (ArmC)
    return {std::string(ArmC) + ";" + Comment + "\n", true};

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
