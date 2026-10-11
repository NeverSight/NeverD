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
#include "neverd/backend/c/render/X86SegmentAsm.h"
#include "neverd/backend/llvm/LLVMX86AddressSpaces.h"
#include "neverd/ir/intrinsics/X86SegmentRegisters.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/AtomicOrdering.h"

#include <cstring>
#include <stdexcept>

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

InlineAsmRender renderX86InlineAsm(Arch TheArch, const std::string &AsmStr,
                                   const std::string &Mnemonic,
                                   bool IsStructReturn,
                                   const std::string &ResultName,
                                   bool ResultLive,
                                   const std::vector<std::string> &Args) {
  // A value-returning `int $$N`, as MedLLVM emits it with register operands.
  {
    llvm::StringRef Text(AsmStr);
    unsigned Vector = 0;
    if (Text.consume_front("int $$") && !Text.getAsInteger(10, Vector) &&
        Vector <= 0xFF) {
      const auto Regs = x86DebugServiceRegisters();
      if (Args.empty() || (TheArch == Arch::X64 && Vector == 0x2D &&
                           Args.size() == Regs.size())) {
        std::vector<std::pair<const char *, std::string>> Inputs;
        for (size_t I = 0; I < Args.size(); ++I)
          Inputs.emplace_back(Regs[I], Args[I]);
        return {
            renderX86InterruptAsm(Vector, Inputs,
                                  ResultLive ? llvm::StringRef(ResultName) : "",
                                  TheArch == Arch::X64 ? "rax" : "eax"),
            false};
      }
    }
  }

  // RDMSR/WRMSR as MedLLVM emits them: the selector in ECX, the value as
  // one 64-bit result (read) or as its EAX/EDX halves (write).
  if (llvm::StringRef(AsmStr).starts_with("rdmsr") && Args.size() == 1) {
    std::string Result;
    if (ResultLive && !ResultName.empty())
      Result = ResultName + " = ";
    return {Result + "__readmsr(" + Args[0] + ");\n", true};
  }
  if (AsmStr == "wrmsr" && Args.size() == 3)
    return {"__writemsr(" + Args[0] + ", ((unsigned __int64)(" + Args[2] +
                ") << 32) | (uint32_t)(" + Args[1] + "));\n",
            true};

  // MXCSR memory forms use a 32-bit load/store, not the address as the CSR
  // value. memcpy keeps unaligned and aliased addresses valid and evaluates
  // the address once. Block scope keeps the temporary private to each effect.
  if (Mnemonic == "ldmxcsr" || Mnemonic == "stmxcsr") {
    if (Args.size() == 1 && !IsStructReturn && !ResultLive) {
      for (const char *Segment : {"fs", "gs"})
        if (AsmStr == Mnemonic + " %" + Segment + ":($0)")
          return {"__asm__ volatile(\"" + Mnemonic + " %%" + Segment +
                      ":(%0)\" : : \"r\"((uintptr_t)(" + Args[0] +
                      ")) : \"memory\");\n",
                  false};
      if (AsmStr == Mnemonic + " ($0)") {
        std::string Temporary = "neverd_mxcsr";
        while (Args[0].find(Temporary) != std::string::npos)
          Temporary += '_';
        if (Mnemonic == "ldmxcsr")
          return {"{ uint32_t " + Temporary + "; __builtin_memcpy(&" +
                      Temporary + ", (const void *)(uintptr_t)(" + Args[0] +
                      "), 4); _mm_setcsr(" + Temporary + "); }\n",
                  true};
        return {"{ uint32_t " + Temporary +
                    " = _mm_getcsr(); __builtin_memcpy((void *)(uintptr_t)(" +
                    Args[0] + "), &" + Temporary + ", 4); }\n",
                true};
      }
    }
    throw std::runtime_error("unsupported MXCSR inline assembly contract");
  }

  // PUSHF/POPF of the whole EFLAGS image, as MedLLVM emits them.
  if (llvm::StringRef(AsmStr).starts_with("pushf") && Args.empty()) {
    std::string Result;
    if (ResultLive && !ResultName.empty())
      Result = ResultName + " = ";
    return {Result + "__readeflags();\n", true};
  }
  if ((AsmStr == "pushq $0\n\tpopfq" || AsmStr == "pushl $0\n\tpopfl") &&
      Args.size() == 1)
    return {"__writeeflags(" + Args[0] + ");\n", true};

  if (Mnemonic == "cpuid") {
    if (AsmStr != "cpuid" || !IsStructReturn || ResultName.empty() ||
        Args.size() != 2)
      throw std::runtime_error("unsupported CPUID inline assembly contract");
    // Preserve each ordered query even when its outputs are unused. The ECX
    // subleaf and the original memory barrier are part of the lifted effect.
    return {"__asm__ volatile(\"cpuid\" : \"=a\"(" + ResultName +
                "[0]), \"=b\"(" + ResultName + "[1]), \"=c\"(" + ResultName +
                "[2]), \"=d\"(" + ResultName + "[3]) : \"a\"((uint32_t)(" +
                Args[0] + ")), \"c\"((uint32_t)(" + Args[1] +
                ")) : \"memory\");\n",
            false};
  }

  if (Mnemonic == "xgetbv") {
    if (AsmStr != "xgetbv" || !IsStructReturn || ResultName.empty() ||
        Args.size() != 1)
      throw std::runtime_error("unsupported XGETBV inline assembly contract");
    return {"__asm__ volatile(\"xgetbv\" : \"=a\"(" + ResultName +
                "[0]), \"=d\"(" + ResultName + "[1]) : \"c\"((uint32_t)(" +
                Args[0] + ")) : \"memory\");\n",
            false};
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
      // A segment selector move (`mov %cs, $0` / `mov ${0:w}, %ds`).
      llvm::StringRef Segment = Reg;
      if (Segment.consume_front("%") && x86SegmentRegisterNamed(Segment) &&
          (Read || (First == "${0:w}" && Args.size() == 1))) {
        if (!Read)
          return {x86SegmentWriteText(Segment, Args[0]) + ";\n", false};
        std::string Result;
        if (ResultLive && !ResultName.empty())
          Result = ResultName + " = ";
        return {Result + x86SegmentReadText(Segment) + ";\n", false};
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
