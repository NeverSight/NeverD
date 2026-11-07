//===- X86SegmentMove.cpp - Segment selector C rendering -----------------===//
//
// NeverD Decompiler
//
//===----------------------------------------------------------------------===//
/// \file
/// Keep native and old-SDK segment moves on one checked C rendering path.
//===----------------------------------------------------------------------===//

#include "neverd/backend/c/render/X86/X86SegmentAsm.h"
#include "neverd/backend/llvm/X86/X86SegmentMoveNative.h"

#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/ModRef.h"

#include <stdexcept>

namespace neverd {
namespace {

std::string expression(const X86SegmentMoveNative &P, uint64_t Number,
                       llvm::StringRef Operand) {
  if (!x86SegmentMoveRegisterIsValid(Number, P.Read))
    throw std::invalid_argument("invalid x86 segment move register");
  const std::string Segment = x86SegmentRegisterName(Number);
  std::string Native = std::string(P.Builtin) + "(" + std::to_string(Number);
  if (P.Memory)
    Native += ", (" + std::string(P.Read ? "void" : "const void") +
              " *)(uintptr_t)(" + Operand.str() + ")";
  else if (!P.Read)
    Native += ", (uint16_t)(" + Operand.str() + ")";
  Native += ")";
  std::string Fallback;
  if (P.Memory) {
    Fallback =
        "({ struct __attribute__((packed, may_alias)) { uint16_t value; } "
        "*neverd_pointer = (void *)(uintptr_t)(" +
        Operand.str() + "); ";
    if (P.Read)
      Fallback += "__asm__ volatile(\"mov %%" + Segment +
                  ", %0\" : \"=m\"(neverd_pointer->value) : : \"memory\"); })";
    else
      Fallback += "__asm__ volatile(\"mov %0, %%" + Segment +
                  "\" : : \"m\"(neverd_pointer->value) : \"memory\"); })";
  } else if (P.Read) {
    Fallback = "({ uint16_t neverd_selector; __asm__ volatile(\"mov %%" +
               Segment +
               ", %0\" : \"=r\"(neverd_selector)); neverd_selector; })";
  } else {
    Fallback = "__asm__ volatile(\"mov %w0, %%" + Segment +
               "\" : : \"r\"((uint32_t)(" + Operand.str() + ")) : \"memory\")";
  }
  return "NEVERD_X86_SEGMENT_MOVE(" + Native + ", " + Fallback + ")";
}

} // namespace

std::string x86SegmentReadText(llvm::StringRef Name) {
  const auto Segment = x86SegmentRegisterNamed(Name);
  if (!Segment)
    throw std::invalid_argument("invalid x86 segment read register");
  return expression(X86SegmentMoveInventory[0], unsigned(*Segment), "");
}

std::string x86SegmentWriteText(llvm::StringRef Name, llvm::StringRef Value) {
  const auto Segment = x86SegmentRegisterNamed(Name);
  if (!Segment)
    throw std::invalid_argument("invalid x86 segment write register");
  return expression(X86SegmentMoveInventory[2], unsigned(*Segment), Value);
}

bool isX86SegmentMoveNativeName(llvm::StringRef Name) {
  for (const auto &P : X86SegmentMoveInventory)
    if (Name == P.LLVMName)
      return true;
  return false;
}

std::optional<std::string>
renderX86SegmentMoveCall(Arch TheArch, const llvm::CallBase &Call,
                         const std::string &Result, bool ResultLive,
                         const std::vector<std::string> &Arguments) {
  const X86SegmentMoveNative *Plan = nullptr;
  uint64_t Number = 0;
  unsigned Operand = 0;
  if (const auto *Callee = Call.getCalledFunction()) {
    for (const auto &P : X86SegmentMoveInventory)
      if (Callee->getName() == P.LLVMName)
        Plan = &P;
    if (!Plan)
      return std::nullopt;
    if (Call.arg_size() != (Plan->Read && !Plan->Memory ? 1U : 2U) ||
        !Call.getArgOperand(0)->getType()->isIntegerTy(32))
      throw std::invalid_argument("invalid native x86 segment move signature");
    const auto *Index =
        llvm::dyn_cast<llvm::ConstantInt>(Call.getArgOperand(0));
    if (!Index ||
        !x86SegmentMoveRegisterIsValid(Index->getZExtValue(), Plan->Read))
      throw std::invalid_argument("invalid native x86 segment move register");
    Number = Index->getZExtValue();
    Operand = 1;
    if (Plan->Memory) {
      const auto *Pointer =
          llvm::dyn_cast<llvm::PointerType>(Call.getArgOperand(1)->getType());
      if (!Pointer || Pointer->getAddressSpace())
        throw std::invalid_argument(
            "segment memory move requires an ordinary pointer");
    } else if (!Plan->Read &&
               !Call.getArgOperand(1)->getType()->isIntegerTy(16)) {
      throw std::invalid_argument("segment write requires a 16-bit selector");
    }
  } else if (const auto *Asm =
                 llvm::dyn_cast<llvm::InlineAsm>(Call.getCalledOperand())) {
    for (unsigned I = 0; const char *Name = x86SegmentRegisterName(I); ++I) {
      if (Asm->getAsmString() == std::string("mov %") + Name + ", $0")
        Plan = &X86SegmentMoveInventory[0];
      else if (Asm->getAsmString() == std::string("mov ${0:w}, %") + Name)
        Plan = &X86SegmentMoveInventory[2];
      if (Plan) {
        Number = I;
        break;
      }
    }
    if (!Plan)
      return std::nullopt;
    if (!Asm->hasSideEffects() || Asm->isAlignStack() || Asm->canThrow() ||
        Asm->getDialect() != llvm::InlineAsm::AD_ATT ||
        Asm->getConstraintString() != (Plan->Read ? "=r" : "r,~{memory}") ||
        Call.arg_size() != (Plan->Read ? 0U : 1U) ||
        (!Plan->Read && !Call.getArgOperand(0)->getType()->isIntegerTy(32)))
      throw std::invalid_argument("invalid x86 segment assembly contract");
  } else {
    return std::nullopt;
  }
  const bool Value = Plan->Read && !Plan->Memory;
  if ((TheArch != Arch::X86 && TheArch != Arch::X64) ||
      Call.getNumOperandBundles() || Call.doesNotReturn() ||
      Call.hasFnAttr(llvm::Attribute::Speculatable) ||
      Call.getCallingConv() != llvm::CallingConv::C ||
      Call.getMemoryEffects() != llvm::MemoryEffects::unknown() ||
      Call.getFunctionType()->isVarArg() ||
      Arguments.size() != Call.arg_size() ||
      (Value && ResultLive && Result.empty()) ||
      (Value ? !Call.getType()->isIntegerTy(16) : !Call.getType()->isVoidTy()))
    throw std::invalid_argument("invalid x86 segment move result or target");
  std::string Code;
  if (Value && ResultLive)
    Code = Result + " = ";
  Code += expression(*Plan, Number, Value ? "" : Arguments[Operand]);
  return Code + ";\n";
}

} // namespace neverd
