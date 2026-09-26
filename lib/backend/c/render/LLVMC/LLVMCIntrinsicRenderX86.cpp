//===- LLVMCIntrinsicRenderX86.cpp - x86 inline-asm rendering -*- C++ -*-===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
///
/// \file
/// x86-specific LLVM-to-C rendering: asm-mnemonic lookup, MSVC __asm /
/// intrinsic translation (CPUID, XGETBV, `__fastfail`, GS/FS loads).
///
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/backend/llvm/LLVMX86AddressSpaces.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/AtomicOrdering.h"

#include <cstring>

namespace neverd {

namespace {

static const AsmToCEntry X86AsmTable[] = {
#define M(a, c) {a, c},
#include "LLVMCAsmToCX86.inc"
#undef M
};

} // anonymous namespace

std::optional<X86RepStos> classifyX86RepStos(Arch TheArch,
                                             const llvm::CallInst &Call) {
  const auto *IA = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!IA || (TheArch != Arch::X86 && TheArch != Arch::X64) ||
      !Call.use_empty() || !IA->hasSideEffects() || IA->isAlignStack() ||
      IA->canThrow() || IA->getDialect() != llvm::InlineAsm::AD_ATT)
    return std::nullopt;
  const unsigned Bits = TheArch == Arch::X86 ? 32 : 64;
  const auto *Ret = llvm::dyn_cast<llvm::StructType>(Call.getType());
  if (!Ret || !Ret->isLiteral() || Ret->getNumElements() != 2 ||
      !Ret->getElementType(0)->isIntegerTy(Bits) ||
      !Ret->getElementType(1)->isIntegerTy(Bits))
    return std::nullopt;
  for (unsigned Bytes : {1u, 2u, 4u, 8u}) {
    if (Bytes == 8 && TheArch == Arch::X86)
      continue;
    const char Suffix = Bytes == 1   ? 'b'
                        : Bytes == 2 ? 'w'
                        : Bytes == 4 ? 'l'
                                     : 'q';
    const std::string Rep = std::string("rep stos") + Suffix;
    for (auto Dir :
         {X86RepStos::Forward, X86RepStos::Backward, X86RepStos::Dynamic}) {
      const bool Dynamic = Dir == X86RepStos::Dynamic;
      const std::string Asm =
          Dir == X86RepStos::Forward ? Rep
          : Dir == X86RepStos::Backward
              ? "std\n\t" + Rep + "\n\tcld"
              : "test $5,$5\n\tje 1f\n\tstd\n\t1:\n\t" + Rep + "\n\tcld";
      const std::string Constraints = std::string("={di},={cx},0,1,{ax},") +
                                      (Dynamic ? "r," : "") +
                                      "~{memory},~{dirflag},~{cc}";
      if (IA->getAsmString() != Asm ||
          IA->getConstraintString() != Constraints ||
          Call.arg_size() != (Dynamic ? 4u : 3u) ||
          !Call.getArgOperand(0)->getType()->isIntegerTy(Bits) ||
          !Call.getArgOperand(1)->getType()->isIntegerTy(Bits) ||
          !Call.getArgOperand(2)->getType()->isIntegerTy(Bytes * 8) ||
          (Dynamic && !Call.getArgOperand(3)->getType()->isIntegerTy(Bits)))
        continue;
      return X86RepStos{Dir, Bytes, Bits};
    }
  }
  return std::nullopt;
}

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

bool isX86FastFailName(llvm::StringRef Name) {
  return stripLeadingUnderscores(Name) == "fastfail";
}

std::string renderX86SegmentedLoad(
    Arch TheArch, const llvm::LoadInst &LI,
    const std::function<std::string(const llvm::Value *)> &ValueStr) {
  if (TheArch != Arch::X86 && TheArch != Arch::X64)
    return {};
  const unsigned AS = LI.getPointerAddressSpace();
  const bool GS = isLLVMX86GSAddressSpace(AS);
  if (!GS && !isLLVMX86FSAddressSpace(AS))
    return {};

  llvm::Type *Ty = LI.getType();
  unsigned Size = 0;
  if (Ty->isIntegerTy())
    Size = Ty->getIntegerBitWidth() / 8;
  else if (Ty->isPointerTy())
    Size = TheArch == Arch::X86 ? 4 : 8;
  const char *Intrinsic = x86SegmentedReadIntrinsic(GS, Size);
  if (!Intrinsic)
    return {};

  const llvm::Value *Offset = LI.getPointerOperand();
  for (unsigned Depth = 0; Offset && Depth < 4; ++Depth) {
    Offset = Offset->stripPointerCasts();
    if (const auto *CE = llvm::dyn_cast<llvm::ConstantExpr>(Offset)) {
      if (CE->getOpcode() == llvm::Instruction::IntToPtr) {
        Offset = CE->getOperand(0);
        continue;
      }
    }
    if (const auto *I2P = llvm::dyn_cast<llvm::IntToPtrInst>(Offset)) {
      Offset = I2P->getOperand(0);
      continue;
    }
    break;
  }
  if (!Offset)
    return {};
  return std::string(Intrinsic) + "(" + ValueStr(Offset) + ")";
}

} // namespace neverd
