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

  // MOV to/from a control or debug register, as MedLLVM emits it
  // (`mov %cr8, $0` / `mov $0, %cr8`): print the MSVC intrinsic.
  {
    llvm::StringRef Text(AsmStr);
    if (Text.consume_front("mov ")) {
      auto [First, Second] = Text.split(',');
      First = First.trim();
      Second = Second.trim();
      const bool Read = Second == "$0";
      const llvm::StringRef Reg = Read ? First : Second;
      const bool IsCr = Reg.starts_with("%cr");
      const bool IsDr = Reg.starts_with("%dr");
      unsigned N = 0;
      if ((IsCr || IsDr) && !Reg.drop_front(3).getAsInteger(10, N) && N <= 15 &&
          (Read || First == "$0")) {
        const std::string Index = std::to_string(N);
        std::string Result;
        if (Read) {
          if (ResultLive && !ResultName.empty())
            Result = ResultName + " = ";
          Result +=
              IsCr ? "__readcr" + Index + "()" : "__readdr(" + Index + ")";
        } else {
          const std::string Value = Args.empty() ? "0" : Args[0];
          Result = IsCr ? "__writecr" + Index + "(" + Value + ")"
                        : "__writedr(" + Index + ", " + Value + ")";
        }
        return {Result + ";\n", true};
      }
    }
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
