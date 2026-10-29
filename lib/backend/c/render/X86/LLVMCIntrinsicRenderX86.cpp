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

#include "X86DescriptorState.h"
#include "X86ScalarSystem.h"
#include "X86SystemRegister.h"

#include "neverd/backend/Limits.h"
#include "neverd/backend/c/render/CTypeFormat.h"
#include "neverd/backend/c/render/LLVMC/LLVMCIntrinsicRender.h"
#include "neverd/backend/c/render/X86/X86SegmentAsm.h"
#include "neverd/backend/llvm/X86/LLVMX86AddressSpaces.h"
#include "neverd/backend/llvm/X86/X86SystemRegisterAsm.h"
#include "neverd/ir/intrinsics/Intrinsics.h"
#include "neverd/ir/intrinsics/X86/X86SegmentRegisters.h"

#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicsX86.h"
#include "llvm/Support/AtomicOrdering.h"
#include "llvm/Support/ModRef.h"

#include <cstring>
#include <stdexcept>
#include <utility>

namespace neverd {

std::optional<InlineAsmRender>
renderX86TimestampCall(Arch TheArch, const llvm::CallBase &Call,
                       const std::string &ResultName, bool ResultLive) {
  const auto *Callee = Call.getCalledFunction();
  if (!Callee || Callee->getName() != "llvm.x86.rdtscp")
    return std::nullopt;
  const auto *Pair = llvm::dyn_cast<llvm::StructType>(Call.getType());
  const auto Attributes = llvm::Intrinsic::getAttributes(
      Call.getContext(), llvm::Intrinsic::x86_rdtscp,
      Callee->getFunctionType());
  if ((TheArch != Arch::X86 && TheArch != Arch::X64) ||
      !Callee->isIntrinsic() || !Callee->isDeclaration() ||
      Callee->getIntrinsicID() != llvm::Intrinsic::x86_rdtscp ||
      !Call.arg_empty() || Call.getNumOperandBundles() ||
      Call.getFunctionType()->isVarArg() ||
      Call.getCallingConv() != llvm::CallingConv::C || Call.doesNotReturn() ||
      Call.getMemoryEffects() != Attributes.getMemoryEffects() ||
      Callee->hasFnAttribute(llvm::Attribute::Speculatable) ||
      Call.hasFnAttr(llvm::Attribute::Speculatable) || !Pair ||
      Pair->isPacked() || Pair->hasName() || Pair->getNumElements() != 2 ||
      !Pair->getElementType(0)->isIntegerTy(64) ||
      !Pair->getElementType(1)->isIntegerTy(32) ||
      (ResultLive && ResultName.empty()))
    throw std::invalid_argument("unsupported x86 RDTSCP call contract");
  if (ResultLive)
    return InlineAsmRender{ResultName + ".field_0 = __rdtscp(&" + ResultName +
                               ".field_1);\n",
                           true};
  return InlineAsmRender{
      "do { unsigned int neverd_tsc_aux; (void)__rdtscp(&neverd_tsc_aux); } "
      "while (0);\n",
      true};
}

namespace {

static const AsmToCEntry X86AsmTable[] = {
#define M(a, c) {a, c},
#include "LLVMCAsmToCX86.inc"
#undef M
};

} // anonymous namespace

namespace {
std::optional<X86DescriptorStateNative>
descriptorNativePlan(Arch TargetArch, llvm::StringRef Name) {
  if (!Name.starts_with("llvm.x86."))
    return std::nullopt;
  const auto Suffix = Name.drop_front(9);
  bool Known = false;
  for (const auto Id : X86DescriptorStateInventory) {
    std::string Mnemonic = intrinsicAsmMnemonic(Id);
    if (isX86DescriptorTable16(Id)) {
      Mnemonic.pop_back();
      Mnemonic += "16";
    }
    Known |= Suffix == Mnemonic || Suffix.starts_with(Mnemonic + ".");
  }
  if (!Known)
    return std::nullopt;
  for (const auto Id : X86DescriptorStateInventory)
    for (bool Memory : {false, true})
      for (unsigned Bytes : {0u, 1u, 2u, 4u, 8u})
        for (bool Explicit : {false, true}) {
          const auto P = x86DescriptorStateNative(
              Id, TargetArch, Memory, Bytes, NdMemoryAddressSpace::Default,
              Explicit ? std::optional<X86DescriptorAddress>(
                             {TargetArch == Arch::X64 ? 64u : 32u, 0})
                       : std::nullopt);
          if (P && P->LLVMName == Name)
            return P;
        }
  return std::nullopt;
}
} // namespace

bool isX86DescriptorStateNativeName(llvm::StringRef Name) {
  if (!Name.starts_with("llvm.x86."))
    return false;
  return descriptorNativePlan(Arch::X64, Name).has_value() ||
         descriptorNativePlan(Arch::X86, Name).has_value();
}

std::optional<InlineAsmRender>
renderX86DescriptorStateCall(Arch TheArch, BinaryFormat Format,
                             const llvm::CallBase &Call,
                             const std::string &ResultName, bool ResultLive,
                             const std::vector<std::string> &Args) {
  auto Reject = []() -> std::optional<InlineAsmRender> {
    throw std::invalid_argument(
        "unsupported x86 descriptor state call contract");
  };
  std::optional<X86DescriptorStateNative> P;
  bool DroppedVerifyResult = false;
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (const auto *Callee = Call.getCalledFunction()) {
    if (!isX86DescriptorStateNativeName(Callee->getName()))
      return std::nullopt;
    P = descriptorNativePlan(TheArch, Callee->getName());
    if (!P || !Callee->isIntrinsic() || !Callee->isDeclaration())
      return Reject();
    if (P->Address) {
      if (Call.arg_size() != 3)
        return Reject();
      const auto *Bits =
          llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(1));
      const auto *Prefix =
          llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(2));
      if (!Bits || !Bits->getType()->isIntegerTy(32) || !Prefix ||
          !Prefix->getType()->isIntegerTy(32))
        return Reject();
      P = x86DescriptorStateNative(
          P->Id, TheArch, true, P->ResultBytes, NdMemoryAddressSpace::Default,
          X86DescriptorAddress{unsigned(Bits->getZExtValue()),
                               unsigned(Prefix->getZExtValue())});
      if (!P)
        return Reject();
    }
  } else if (Asm) {
    const auto Text = Asm->getAsmString();
    // The mnemonic filter avoids walking the inventory for unrelated asm.
    bool Known = false;
    for (const auto Id : X86DescriptorStateInventory)
      Known |= llvm::StringRef(Text).starts_with(
          std::string(intrinsicAsmMnemonic(Id)) + " ");
    if (!Known)
      return std::nullopt;
    const unsigned WordBits = TheArch == Arch::X64 ? 64 : 32;
    for (const auto Id : X86DescriptorStateInventory)
      for (bool Memory : {false, true})
        for (unsigned Bytes : {0u, 1u, 2u, 4u, 8u})
          for (unsigned Bits : {0u, WordBits, WordBits / 2})
            for (unsigned Prefix :
                 {0u, 0x26u, 0x2eu, 0x36u, 0x3eu, 0x64u, 0x65u}) {
              if (!Bits && Prefix)
                continue;
              const auto Candidate = x86DescriptorStateNative(
                  Id, TheArch, Memory, Bytes, NdMemoryAddressSpace::Default,
                  Bits ? std::optional<X86DescriptorAddress>({Bits, Prefix})
                       : std::nullopt);
              if (!Candidate)
                continue;
              auto Match = [&](std::string Constraints) {
                if (Candidate->Text == Text &&
                    Constraints == Asm->getConstraintString() &&
                    (Candidate->ResultBytes ? Call.getType()->isIntegerTy(
                                                  8 * Candidate->ResultBytes)
                                            : Call.getType()->isVoidTy())) {
                  P = Candidate;
                  DroppedVerifyResult = false;
                }
                if (!Candidate->Verify)
                  return;
                auto VoidText = Candidate->Text.substr(
                    0, Candidate->Text.find("\n\tsetz $0"));
                const auto Input = VoidText.find("$1");
                if (Input != std::string::npos)
                  VoidText.replace(Input, 2, "$0");
                if (VoidText == Text && Call.getType()->isVoidTy() &&
                    Constraints.substr(3) == Asm->getConstraintString()) {
                  P = Candidate;
                  DroppedVerifyResult = true;
                }
              };
              Match(Candidate->Constraints);
              if (Candidate->Address) {
                // Retain the exact older fixed-BX spelling, while new output
                // avoids reserved realignment and PIC bases.
                auto Constraints = Candidate->Constraints;
                Constraints.replace(
                    Constraints.find(Candidate->InputConstraint),
                    Candidate->InputConstraint.size(), "{bx}");
                Match(Constraints);
              }
              // Earlier lifted FS/GS accesses already normalized the offset
              // but allowed any base register. Keep that exact legacy form.
              if (Candidate->Address && Candidate->Address->Bits == WordBits &&
                  (Prefix == 0x64 || Prefix == 0x65)) {
                auto Constraints = Candidate->Constraints;
                Constraints.replace(
                    Constraints.find(Candidate->InputConstraint),
                    Candidate->InputConstraint.size(), "r");
                Match(Constraints);
              }
            }
    if (!P || !Asm->hasSideEffects() || Asm->isAlignStack() ||
        Asm->canThrow() || Asm->getDialect() != llvm::InlineAsm::AD_ATT)
      return Reject();
  } else {
    return std::nullopt;
  }
  if (Call.getNumOperandBundles() || Call.doesNotReturn() ||
      Call.getCallingConv() != llvm::CallingConv::C ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      Call.getFunctionType()->isVarArg() ||
      Call.arg_size() !=
          (Asm ? unsigned(P->OperandBytes != 0) : P->arguments()) ||
      Args.size() != Call.arg_size() ||
      (P->ResultBytes && !DroppedVerifyResult
           ? !Call.getType()->isIntegerTy(8 * P->ResultBytes)
           : !Call.getType()->isVoidTy()) ||
      (ResultLive && P->ResultBytes && ResultName.empty()))
    return Reject();
  if (P->OperandBytes) {
    const auto *Type = Call.getArgOperand(0)->getType();
    if (P->Memory && !P->Address) {
      const bool LegacyOffset = Asm && Type->isIntegerTy(8 * P->OperandBytes);
      if (!LegacyOffset &&
          (!Type->isPointerTy() || Type->getPointerAddressSpace() != 0))
        return Reject();
    } else if (!Type->isIntegerTy(8 * P->OperandBytes)) {
      return Reject();
    }
  }
  const std::string Prefix = !P->ResultBytes ? ""
                             : ResultLive    ? ResultName + " = "
                                             : "(void)";
  return InlineAsmRender{
      Prefix +
          renderX86DescriptorStateExpression(
              *P, P->OperandBytes ? std::vector<std::string>{Args[0]}
                                  : std::vector<std::string>{}) +
          ";\n",
      Format == BinaryFormat::COFF && P->Memory && !P->Address &&
          (P->Id == Intrinsic::Sidt || P->Id == Intrinsic::Lidt ||
           P->Id == Intrinsic::Invlpg)};
}

bool isX86ScalarSystemNativeName(llvm::StringRef Name) {
  return x86ScalarSystemNative(std::string_view(Name.data(), Name.size()))
      .has_value();
}

std::optional<InlineAsmRender>
renderX86ScalarSystemCall(Arch TheArch, BinaryFormat Format,
                          const llvm::CallBase &Call,
                          const std::string &ResultName, bool ResultLive,
                          const std::vector<std::string> &Args) {
  auto Reject = []() -> std::optional<InlineAsmRender> {
    throw std::invalid_argument("unsupported scalar x86 system call contract");
  };
  std::optional<X86ScalarSystemNative> P;
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (const auto *Callee = Call.getCalledFunction()) {
    P = x86ScalarSystemNative(Callee->getName().str());
    if (!P)
      return std::nullopt;
    if (!Callee->isIntrinsic() || !Callee->isDeclaration())
      return Reject();
  } else if (Asm) {
    for (const auto &Candidate : X86ScalarSystemNativeInventory) {
      std::string Text = Candidate.Mnemonic;
      if (Candidate.Port)
        Text += Candidate.Write ? " $0, $1" : " $1, $0";
      else if (!Candidate.Write && TheArch == Arch::X64)
        Text += "\n\tshlq $$32, %rdx\n\torq %rdx, %rax";
      if (Text == Asm->getAsmString()) {
        P = Candidate;
        break;
      }
    }
    if (!P)
      return std::nullopt;
    if (!Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
        Asm->getDialect() != llvm::InlineAsm::AD_ATT)
      return Reject();
  } else {
    return std::nullopt;
  }
  const bool SplitMsr = Asm && !P->Port && P->Write;
  const bool Reverse = Asm && P->Port && P->Write;
  if ((TheArch != Arch::X86 && TheArch != Arch::X64) ||
      Call.getNumOperandBundles() || Call.doesNotReturn() ||
      Call.getCallingConv() != llvm::CallingConv::C ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      Call.getFunctionType()->isVarArg() ||
      Call.arg_size() != P->arguments() + unsigned(SplitMsr) ||
      Args.size() != Call.arg_size() ||
      (P->Write ? !Call.getType()->isVoidTy()
                : !Call.getType()->isIntegerTy(P->Bytes * 8)) ||
      (ResultLive && !P->Write && ResultName.empty()))
    return Reject();
  std::vector<std::string> Arguments;
  bool ImmediatePort = false;
  for (unsigned I = 0; I < Call.arg_size(); ++I) {
    const unsigned Input = Reverse ? 1 - I : I;
    const auto *Value = Call.getArgOperand(Input);
    const unsigned Bytes = SplitMsr ? 4 : P->argumentBytes(I);
    if (Asm && P->Port && I == 0)
      if (const auto *Constant = llvm::dyn_cast<llvm::ConstantInt>(Value))
        ImmediatePort = Constant->getType()->isIntegerTy(8) &&
                        Constant->getZExtValue() <= UINT8_MAX;
    if (!Value->getType()->isIntegerTy(Bytes * 8) && !(I == 0 && ImmediatePort))
      return Reject();
    // The N constraint names an unsigned instruction byte. A signed C
    // spelling such as -128 must not widen to port 0xff80. DX operands keep
    // their full 16-bit value; only a validated immediate is zero-extended.
    Arguments.push_back(
        I == 0 && ImmediatePort
            ? "0x" + llvm::utohexstr(
                         llvm::cast<llvm::ConstantInt>(Value)->getZExtValue())
            : Args[Input]);
  }
  if (Asm) {
    std::string Constraints;
    if (P->Port)
      Constraints = std::string(P->Write ? "{ax}," : "={ax},") +
                    (ImmediatePort ? "N" : "{dx}") + ",~{memory}";
    else if (P->Write)
      Constraints = "{ecx},{eax},{edx},~{memory}";
    else
      Constraints = TheArch == Arch::X64 ? "={rax},{ecx},~{rdx},~{memory}"
                                         : "=A,{ecx},~{memory}";
    if (Constraints != Asm->getConstraintString())
      return Reject();
  }
  if (SplitMsr) {
    Arguments[1] = "((uint64_t)(uint32_t)(" + Arguments[2] +
                   ") << 32) | (uint32_t)(" + Arguments[1] + ")";
    Arguments.pop_back();
  }
  const std::string Prefix = P->Write     ? ""
                             : ResultLive ? ResultName + " = "
                                          : "(void)";
  return InlineAsmRender{
      Prefix + renderX86ScalarSystemExpression(*P, Arguments) + ";\n",
      Format == BinaryFormat::COFF};
}

bool isX86SystemRegisterNativeName(llvm::StringRef Name) {
  if (!Name.consume_front("llvm.x86."))
    return false;
  Name.consume_back("32");
  return Name == "readcr" || Name == "writecr" || Name == "readdr" ||
         Name == "writedr";
}

std::optional<InlineAsmRender>
renderX86SystemRegisterCall(Arch TheArch, BinaryFormat Format,
                            const llvm::CallBase &Call,
                            const std::string &ResultName, bool ResultLive,
                            const std::vector<std::string> &Args) {
  auto Reject = []() -> std::optional<InlineAsmRender> {
    throw std::invalid_argument(
        "unsupported x86 system register call contract");
  };
  std::optional<X86SystemRegisterAsm> P;
  unsigned FirstValue = 0;
  if (const auto *Callee = Call.getCalledFunction();
      Callee && isX86SystemRegisterNativeName(Callee->getName())) {
    if (!Callee->isIntrinsic() || !Callee->isDeclaration() || Call.arg_empty())
      return Reject();
    const auto *Index =
        llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(0));
    if (!Index || !Index->getType()->isIntegerTy(32))
      return Reject();
    auto Access = Callee->getName().drop_front(9);
    Access.consume_back("32");
    const bool Debug = Access.ends_with("dr");
    const std::string CName =
        "__" + Access.str() +
        (Debug ? "" : std::to_string(Index->getZExtValue()));
    P = x86SystemRegisterAsm(intrinsicFromName(CName.c_str()), TheArch,
                             Index->getZExtValue());
    if (!P || P->LLVMName != Callee->getName() ||
        Call.getCallingConv() != llvm::CallingConv::C)
      return Reject();
    FirstValue = 1;
  } else {
    const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
    if (!Asm)
      return std::nullopt;
    llvm::StringRef Text = Asm->getAsmString();
    if (!Text.contains("%cr") && !Text.contains("%dr"))
      return std::nullopt;
    if (!Text.consume_front("mov "))
      return Reject();
    auto [First, Second] = Text.split(',');
    First = First.trim();
    Second = Second.trim();
    const bool Read = Second == "$0";
    const auto Reg = Read ? First : Second;
    uint64_t Index;
    if ((!Reg.starts_with("%cr") && !Reg.starts_with("%dr")) ||
        Reg.drop_front(3).getAsInteger(10, Index))
      return Reject();
    const bool Debug = Reg.starts_with("%dr");
    const std::string CName = std::string(Read ? "__read" : "__write") +
                              (Debug ? "dr" : "cr" + std::to_string(Index));
    P = x86SystemRegisterAsm(intrinsicFromName(CName.c_str()), TheArch, Index);
    if (!P || P->Text != Asm->getAsmString() ||
        P->Constraints != Asm->getConstraintString() ||
        !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
        Asm->getDialect() != llvm::InlineAsm::AD_ATT)
      return Reject();
  }
  if (Call.getNumOperandBundles() ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      Call.doesNotReturn() || Call.getFunctionType()->isVarArg() ||
      Call.arg_size() != FirstValue + unsigned(P->Write) ||
      Args.size() != Call.arg_size() ||
      (P->Write
           ? !Call.getType()->isVoidTy() || !Call.getArgOperand(FirstValue)
                                                 ->getType()
                                                 ->isIntegerTy(8 * P->Bytes)
           : !Call.getType()->isIntegerTy(8 * P->Bytes)) ||
      (ResultLive && !P->Write && ResultName.empty()))
    return Reject();

  const bool Microsoft = Format == BinaryFormat::COFF;
  const std::string Prefix = P->Write     ? ""
                             : ResultLive ? ResultName + " = "
                                          : "(void)";
  return InlineAsmRender{
      Prefix +
          renderX86SystemRegisterExpression(
              *P, Microsoft, {Args.begin() + FirstValue, Args.end()}) +
          ";\n",
      Microsoft};
}

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
                                      "~{memory},~{dirflag},~{flags}";
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

std::optional<X86RepMovs> classifyX86RepMovs(Arch TheArch,
                                             const llvm::CallInst &Call) {
  const auto *IA = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!IA || (TheArch != Arch::X86 && TheArch != Arch::X64) ||
      !Call.use_empty() || !IA->hasSideEffects() || IA->isAlignStack() ||
      IA->canThrow() || IA->getDialect() != llvm::InlineAsm::AD_ATT)
    return std::nullopt;
  const unsigned Bits = TheArch == Arch::X86 ? 32 : 64;
  const auto *Ret = llvm::dyn_cast<llvm::StructType>(Call.getType());
  if (!Ret || !Ret->isLiteral() || Ret->getNumElements() != 3 ||
      llvm::any_of(Ret->elements(),
                   [&](llvm::Type *T) { return !T->isIntegerTy(Bits); }))
    return std::nullopt;
  for (unsigned Bytes : {1u, 2u, 4u, 8u}) {
    if (Bytes == 8 && TheArch == Arch::X86)
      continue;
    const char Suffix = Bytes == 1   ? 'b'
                        : Bytes == 2 ? 'w'
                        : Bytes == 4 ? 'l'
                                     : 'q';
    const std::string Rep = std::string("rep movs") + Suffix;
    for (auto Dir :
         {X86RepMovs::Forward, X86RepMovs::Backward, X86RepMovs::Dynamic}) {
      const bool Dynamic = Dir == X86RepMovs::Dynamic;
      const std::string Asm =
          Dir == X86RepMovs::Forward ? Rep
          : Dir == X86RepMovs::Backward
              ? "std\n\t" + Rep + "\n\tcld"
              : "test $6,$6\n\tje 1f\n\tstd\n\t1:\n\t" + Rep + "\n\tcld";
      const std::string Constraints = std::string("={si},={di},={cx},0,1,2,") +
                                      (Dynamic ? "r," : "") +
                                      "~{memory},~{dirflag},~{flags}";
      if (IA->getAsmString() != Asm ||
          IA->getConstraintString() != Constraints ||
          Call.arg_size() != (Dynamic ? 4u : 3u) ||
          llvm::any_of(Call.args(), [&](const llvm::Use &Arg) {
            return !Arg->getType()->isIntegerTy(Bits);
          }))
        continue;
      return X86RepMovs{Dir, Bytes, Bits};
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

std::optional<InlineAsmRender>
renderX86PrefetchCall(Arch TheArch, const llvm::CallBase &Call,
                      const std::vector<std::string> &Args) {
  const auto *Asm = llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand());
  if (!Asm)
    return std::nullopt;
  const llvm::StringRef Text = Asm->getAsmString();
  const auto Mnemonic = Text.split(' ').first;
  bool Prefetch = false;
  for (const auto Id :
       {Intrinsic::PrefetchT0, Intrinsic::PrefetchT1, Intrinsic::PrefetchT2,
        Intrinsic::PrefetchNta, Intrinsic::PrefetchW, Intrinsic::PrefetchWT1})
    Prefetch |= Mnemonic == intrinsicAsmMnemonic(Id);
  if (!Prefetch)
    return std::nullopt;
  auto Reject = []() -> std::optional<InlineAsmRender> {
    throw std::invalid_argument("unsupported x86 prefetch call contract");
  };
  if ((TheArch != Arch::X86 && TheArch != Arch::X64) || Args.size() != 1 ||
      Call.arg_size() != 1 || !Call.getType()->isVoidTy() ||
      !Call.getArgOperand(0)->getType()->isIntegerTy(
          TheArch == Arch::X64 ? 64 : 32) ||
      Call.getFunctionType()->isVarArg() || Call.getNumOperandBundles() ||
      Call.doesNotReturn() || Call.getCallingConv() != llvm::CallingConv::C ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      !Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
      Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
      Asm->getConstraintString() != "r,~{memory}")
    return Reject();
  for (const std::string Segment : {"", "fs", "gs"}) {
    const auto Input = Segment.empty() ? "($0)" : "%" + Segment + ":($0)";
    if (Text != Mnemonic.str() + " " + Input)
      continue;
    const auto Output = Segment.empty() ? "(%0)" : "%%" + Segment + ":(%0)";
    // C operands remain operands. Substituting an address expression into
    // MS-style assembly loses its memory indirection and cannot name a GEP.
    return InlineAsmRender{"__asm__ volatile(\"" + Mnemonic.str() + " " +
                               Output + "\" : : \"r\"((uintptr_t)(" + Args[0] +
                               ")) : \"memory\");\n",
                           false};
  }
  return Reject();
}

InlineAsmRender renderX86InlineAsm(Arch TheArch, const std::string &AsmStr,
                                   const std::string &Mnemonic,
                                   bool IsStructReturn,
                                   const std::string &ResultName,
                                   bool ResultLive,
                                   const std::vector<std::string> &Args) {
  if (AsmStr == intrinsicAsmMnemonic(Intrinsic::Int1)) {
    if (IsStructReturn || ResultLive || !Args.empty())
      throw std::runtime_error("unsupported INT1 inline assembly contract");
    return {"__asm__ volatile(\"" + AsmStr + "\" ::: \"memory\");\n", false};
  }
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

  // CR/DR accesses require the complete CallInst contract, not just text.
  if (llvm::StringRef(AsmStr).contains("%cr") ||
      llvm::StringRef(AsmStr).contains("%dr"))
    throw std::invalid_argument(
        "x86 system register assembly needs a checked call");

  {
    llvm::StringRef Text(AsmStr);
    if (Text.consume_front("mov ")) {
      auto [First, Second] = Text.split(',');
      First = First.trim();
      Second = Second.trim();
      const bool Read = Second == "$0";
      const llvm::StringRef Reg = Read ? First : Second;
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
    std::vector<std::string> Operands = Args;
    // Port I/O takes the port first in C (`__outbyte(port, data)`), where
    // the assembly writes the data first (`outb $0, $1`).  An immediate port
    // is the unsigned byte the instruction holds: 0x80, not -128.
    const llvm::StringRef CName(X86C);
    const bool PortIn =
        CName == "__inbyte" || CName == "__inword" || CName == "__indword";
    const bool PortOut =
        CName == "__outbyte" || CName == "__outword" || CName == "__outdword";
    if ((PortIn && Operands.size() == 1) || (PortOut && Operands.size() == 2)) {
      if (PortOut)
        std::swap(Operands[0], Operands[1]);
      int64_t Port = 0;
      if (!llvm::StringRef(Operands[0]).getAsInteger(10, Port) && Port < 0 &&
          Port >= -128)
        Operands[0] =
            "0x" + llvm::utohexstr(static_cast<uint64_t>(Port) & 0xFF);
    }
    std::string Result;
    if (ResultLive && !ResultName.empty())
      Result = ResultName + " = ";
    Result += std::string(X86C) + "(";
    for (size_t I = 0; I < Operands.size(); ++I) {
      if (I > 0)
        Result += ", ";
      Result += Operands[I];
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

const char *renderX86DebugBreak(BinaryFormat Format) {
  return Format == BinaryFormat::COFF ? "__debugbreak();\n"
                                      : "__builtin_debugtrap();\n";
}

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
  for (unsigned Depth = 0;
       Offset && Depth < ::neverd::limits::backend::
                             kLLVMCIntrinsicRenderX86SegmentedLoadDepthLimit;
       ++Depth) {
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
